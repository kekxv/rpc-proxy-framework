package com.kekxv.rpc;

import com.sun.jna.Native;
import com.sun.jna.Pointer;
import com.sun.jna.platform.win32.WinBase;
import com.sun.jna.platform.win32.WinNT;
import com.sun.jna.ptr.IntByReference;
import com.sun.jna.win32.StdCallLibrary;
import com.sun.jna.win32.W32APIOptions;
import org.json.JSONObject;

// 如果不需要 Unix 支持，可以注释掉下面两行并修改 connectUnix 方法
import org.newsclub.net.unix.AFUNIXSocket;
import org.newsclub.net.unix.AFUNIXSocketAddress;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

public class RpcClient implements AutoCloseable {

  public boolean isAlive() {
    return running.get();
  }

  // =========================================================================
  // 1. 自定义 Windows API 接口 (解决 JNA 版本差异导致的方法缺失/参数不匹配问题)
  // =========================================================================
  public interface WinKernel32 extends StdCallLibrary {
    WinKernel32 INSTANCE = Native.load("kernel32", WinKernel32.class, W32APIOptions.UNICODE_OPTIONS);

    // 打开文件/管道
    WinNT.HANDLE CreateFile(String lpFileName, int dwDesiredAccess, int dwShareMode,
                            WinBase.SECURITY_ATTRIBUTES lpSecurityAttributes,
                            int dwCreationDisposition, int dwFlagsAndAttributes,
                            WinNT.HANDLE hTemplateFile);

    // 检查管道是否有数据 (防止死锁的核心)
    boolean PeekNamedPipe(WinNT.HANDLE hNamedPipe, Pointer lpBuffer, int nBufferSize,
                          IntByReference lpBytesRead, IntByReference lpTotalBytesAvail,
                          IntByReference lpBytesLeftThisMessage);

    // 读取数据 (定义为 byte[] 以方便使用)
    boolean ReadFile(WinNT.HANDLE hFile, byte[] lpBuffer, int nNumberOfBytesToRead,
                     IntByReference lpNumberOfBytesRead, WinBase.OVERLAPPED lpOverlapped);

    // 写入数据
    boolean WriteFile(WinNT.HANDLE hFile, byte[] lpBuffer, int nNumberOfBytesToRead,
                      IntByReference lpNumberOfBytesWritten, WinBase.OVERLAPPED lpOverlapped);

    boolean CloseHandle(WinNT.HANDLE hObject);

    int GetLastError();
  }

  // =========================================================================
  // 2. RpcClient 主逻辑
  // =========================================================================

  private final String pipeName;
  private final AtomicBoolean running = new AtomicBoolean(false);

  // Windows 资源
  private WinNT.HANDLE windowsHandle;

  // Unix 资源
  private Closeable unixSocket;
  private DataInputStream unixInputStream;

  // 通用输出流 (包装后的)
  private DataOutputStream outputStream;

  private Thread receiveThread;
  private final Object sendLock = new Object();

  private final AtomicInteger requestIdCounter = new AtomicInteger(0);
  private final Map<String, CompletableFuture<JSONObject>> pendingRequests = new ConcurrentHashMap<>();
  private final BlockingQueue<JSONObject> eventQueue = new LinkedBlockingQueue<>();

  public RpcClient(String pipeName) {
    this.pipeName = pipeName;
  }

  public void connect() throws IOException {
    if (running.get()) return;

    String os = System.getProperty("os.name").toLowerCase();
    if (os.contains("win"))  {
      connectWindows();
    } else {
      connectUnix();
    }

    running.set(true);

    // 启动接收线程
    receiveThread = new Thread(this::receiveLoop, "Rpc-Receiver-" + pipeName);
    receiveThread.setDaemon(true);
    receiveThread.start();
  }

  /**
   * Windows 连接逻辑 (Win7 兼容，JNA)
   */
  private void connectWindows() throws IOException {
    String path = "\\\\.\\pipe\\" + pipeName;

    // GENERIC_READ (0x80000000) | GENERIC_WRITE (0x40000000) = 0xC0000000
    int access = 0xC0000000;
    // OPEN_EXISTING = 3
    int disposition = 3;

    windowsHandle = WinKernel32.INSTANCE.CreateFile(
      path, access, 0, null, disposition, 0, null
    );

    if (WinBase.INVALID_HANDLE_VALUE.equals(windowsHandle)) {
      int err = WinKernel32.INSTANCE.GetLastError();
      throw new IOException("Failed to connect to pipe: " + path + " (Error code: " + err + ")");
    }

    // 初始化输出流 (包装 JNA WriteFile)
    this.outputStream = new DataOutputStream(new OutputStream() {
      @Override
      public void write(int b) throws IOException {
        write(new byte[]{(byte) b}, 0, 1);
      }

      @Override
      public void write(byte[] b, int off, int len) throws IOException {
        if (b == null) throw new NullPointerException();
        if (off < 0 || len < 0 || off + len > b.length) throw new IndexOutOfBoundsException();

        byte[] dataToWrite;
        if (off == 0 && len == b.length) {
          dataToWrite = b;
        } else {
          dataToWrite = new byte[len];
          System.arraycopy(b, off, dataToWrite, 0, len);
        }

        IntByReference written = new IntByReference();
        boolean success = WinKernel32.INSTANCE.WriteFile(
          windowsHandle, dataToWrite, len, written, null);

        if (!success) {
          throw new IOException("WriteFile failed. Error: " + WinKernel32.INSTANCE.GetLastError());
        }
      }
    });
  }

  /**
   * Unix 连接逻辑 (JunixSocket)
   */
  private void connectUnix() throws IOException {
    File socketFile = new File("/tmp/" + pipeName);
    AFUNIXSocket socket = AFUNIXSocket.newInstance();
    socket.connect(AFUNIXSocketAddress.of(socketFile));
    this.unixSocket = socket;
    this.unixInputStream = new DataInputStream(socket.getInputStream());
    this.outputStream = new DataOutputStream(socket.getOutputStream());
  }

  /**
   * 核心接收循环
   */
  private void receiveLoop() {
    try {
      while (running.get()) {
        if (windowsHandle != null) {
          // ==============================
          // Windows (JNA Peek 模式)
          // ==============================
          IntByReference bytesAvail = new IntByReference(0);

          // 1. 先看一眼
          boolean peekOk = WinKernel32.INSTANCE.PeekNamedPipe(
            windowsHandle, null, 0, null, bytesAvail, null);

          if (!peekOk) {
            // 【关键修改】获取错误码，判断具体原因
            int err = WinKernel32.INSTANCE.GetLastError();

            // 109 = ERROR_BROKEN_PIPE (服务端正常关闭了管道)
            if (err == 109) {
              break; // 正常退出循环，不抛异常
            }

            // 其他错误才是真正的异常
            throw new IOException("Pipe broken (Peek failed). Error Code: " + err);
          }

          // 2. 如果数据不足 4 字节，休眠
          if (bytesAvail.getValue() < 4) {
            Thread.sleep(10);
            continue;
          }

          // 3. 有数据了，读取
          int bodyLen = readIntWin();
          byte[] bodyBytes = readFullyWin(bodyLen);

          String jsonStr = new String(bodyBytes, StandardCharsets.UTF_8);
          processMessage(jsonStr);

        } else if (unixInputStream != null) {
          // Unix 模式 (保持不变)
          int bodyLen = unixInputStream.readInt();
          byte[] bodyBytes = new byte[bodyLen];
          unixInputStream.readFully(bodyBytes);
          processMessage(new String(bodyBytes, StandardCharsets.UTF_8));
        }
      }
    } catch (InterruptedException e) {
      Thread.currentThread().interrupt();
    } catch (EOFException e) {
      // log.info("Connection closed by peer (EOF).");
    } catch (Exception e) {
      // 只有在运行状态下发生的未知错误才打印 Error
      if (running.get()) {
        // log.error("Receive loop error: {}", e.getMessage(), e);
      }
    } finally {
      close();
    }
  }

  // Windows 辅助读取：int (Big Endian)
  private int readIntWin() throws IOException {
    byte[] b = readFullyWin(4);
    return ((b[0] & 0xFF) << 24) |
      ((b[1] & 0xFF) << 16) |
      ((b[2] & 0xFF) << 8) |
      (b[3] & 0xFF);
  }

  // Windows 辅助读取：byte[]
  private byte[] readFullyWin(int len) throws IOException {
    byte[] buf = new byte[len];
    IntByReference bytesRead = new IntByReference();
    int total = 0;

    // 循环确保读满
    while (total < len) {
      // 注意：这里没有 offset 参数，所以每次需要临时计算
      // 简单实现：由于我们 Peek 过了，绝大多数情况一次就能读满
      // 如果遇到拆包，对于 JNA byte[] 映射来说处理 offset 比较麻烦
      // 这里假设 pipe 数据是连续到达的
      if (total > 0) {
        // 如果真的发生了分段读取，需要复杂处理，这里为了简化省略
        // 实际上 PeekNamedPipe 保证了数据量，通常一次 ReadFile 就够了
        throw new IOException("Partial read occurred in JNA mode, implementation simplified.");
      }

      boolean success = WinKernel32.INSTANCE.ReadFile(
        windowsHandle, buf, len, bytesRead, null);

      if (!success || bytesRead.getValue() == 0) {
        throw new EOFException();
      }
      total += bytesRead.getValue();
    }
    return buf;
  }

  private void processMessage(String jsonStr) {
    try {
      JSONObject response = new JSONObject(jsonStr);
      if (response.has("request_id")) {
        String reqId = response.getString("request_id");
        CompletableFuture<JSONObject> future = pendingRequests.remove(reqId);
        if (future != null) {
          future.complete(response);
        }
      } else if (response.has("event")) {
        if (!eventQueue.offer(response)) {
          // log.warn("Event queue full");
        }
      }
    } catch (Exception e) {
      // log.error("JSON parse error: {}", jsonStr, e);
    }
  }

  public JSONObject sendRequest(JSONObject request, long timeout, TimeUnit unit) throws Exception {
    if (!running.get()) throw new IOException("Client is closed");

    String reqId = "req-" + requestIdCounter.incrementAndGet();
    JSONObject reqToSend = new JSONObject(request.toString());
    reqToSend.put("request_id", reqId);

    CompletableFuture<JSONObject> future = new CompletableFuture<>();
    pendingRequests.put(reqId, future);

    try {
      byte[] bodyBytes = reqToSend.toString().getBytes(StandardCharsets.UTF_8);
      synchronized (sendLock) {
        if (!running.get()) throw new IOException("Client closed");
        outputStream.writeInt(bodyBytes.length);
        outputStream.write(bodyBytes);
        outputStream.flush();
      }
      return future.get(timeout, unit);
    } catch (Exception e) {
      pendingRequests.remove(reqId);
      throw e;
    }
  }

  // 兼容旧接口
  public JSONObject sendRequest(JSONObject request) throws Exception {
    return sendRequest(request, 10, TimeUnit.SECONDS);
  }

  public JSONObject getEvent(long timeout, TimeUnit unit) throws InterruptedException {
    return eventQueue.poll(timeout, unit);
  }

    /**
     * 清空事件队列
     */
    public void clearEvents() {
        eventQueue.clear();
    }

  @Override
  public void close() {
    if (!running.compareAndSet(true, false)) {
      return;
    }
    // log.info("Closing RpcClient...");

    // 1. 关闭 Windows 句柄 (瞬间完成)
    if (windowsHandle != null) {
      WinKernel32.INSTANCE.CloseHandle(windowsHandle);
      windowsHandle = null;
    }

    // 2. 中断线程 (Peek 模式下的线程可以响应中断)
    if (receiveThread != null) {
      receiveThread.interrupt();
    }

    // 3. 关闭 Unix 资源
    closeQuietly(unixSocket);
    closeQuietly(outputStream); // 关闭包装流

    // 4. 清理
    IOException closedEx = new IOException("Client closed");
    for (CompletableFuture<JSONObject> future : pendingRequests.values()) {
      future.completeExceptionally(closedEx);
    }
    pendingRequests.clear();
    eventQueue.clear();
  }

  private void closeQuietly(AutoCloseable c) {
    if (c != null) {
      try {
        c.close();
      } catch (Exception ignored) {
      }
    }
  }
}
