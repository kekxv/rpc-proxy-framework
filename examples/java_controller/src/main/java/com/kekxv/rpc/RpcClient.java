package com.kekxv.rpc;

import org.json.JSONObject;
// 如果只在 Windows 运行且不想引入 jar 包，可以注释掉下面这两行 junixsocket 的引用
import org.newsclub.net.unix.AFUNIXSocket;
import org.newsclub.net.unix.AFUNIXSocketAddress;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * 跨平台 RPC 客户端 (Java 8+)
 */
public class RpcClient implements AutoCloseable {

    private final String pipeName;
    // 使用 AtomicBoolean 确保状态切换的原子性
    private final AtomicBoolean running = new AtomicBoolean(false);

    // 接收线程 (volatile 确保可见性)
    private volatile Thread receiveThread;

    // IO 流
    private DataInputStream inputStream;
    private DataOutputStream outputStream;

    // 平台特定资源
    private RandomAccessFile windowsPipeRaf;
    private Closeable unixSocket;

    private final AtomicInteger requestIdCounter = new AtomicInteger(0);
    private final Map<String, CompletableFuture<JSONObject>> pendingRequests = new ConcurrentHashMap<>();
    private final BlockingQueue<JSONObject> eventQueue = new LinkedBlockingQueue<>();

    public RpcClient(String pipeName) {
        this.pipeName = pipeName;
    }

    public void connect() throws IOException {
        // 防止重复连接
        if (running.get()) {
            return;
        }

        String os = System.getProperty("os.name").toLowerCase();
        if (os.contains("win")) {
            connectWindows();
        } else {
            connectUnix();
        }

        running.set(true);

        receiveThread = new Thread(this::receiveLoop, "Rpc-Receiver-" + pipeName);
        receiveThread.setDaemon(true);
        receiveThread.start();
    }

    private void connectWindows() throws IOException {
        String path = "\\\\.\\pipe\\" + pipeName;
        try {
            windowsPipeRaf = new RandomAccessFile(path, "rw");
            InputStream is = new InputStream() {
                @Override public int read() throws IOException { return windowsPipeRaf.read(); }
                @Override public int read(byte[] b, int off, int len) throws IOException { return windowsPipeRaf.read(b, off, len); }
            };
            OutputStream os = new OutputStream() {
                @Override public void write(int b) throws IOException { windowsPipeRaf.write(b); }
                @Override public void write(byte[] b, int off, int len) throws IOException { windowsPipeRaf.write(b, off, len); }
            };
            this.inputStream = new DataInputStream(new BufferedInputStream(is));
            this.outputStream = new DataOutputStream(new BufferedOutputStream(os));
        } catch (FileNotFoundException e) {
            throw new IOException("Could not connect to Windows pipe: " + path, e);
        }
    }

    private void connectUnix() throws IOException {
        File socketFile = new File("/tmp/" + pipeName);
        AFUNIXSocket socket = AFUNIXSocket.newInstance();
        socket.connect(AFUNIXSocketAddress.of(socketFile));
        this.unixSocket = socket;
        this.inputStream = new DataInputStream(socket.getInputStream());
        this.outputStream = new DataOutputStream(socket.getOutputStream());
    }

    public JSONObject sendRequest(JSONObject request, long timeout, TimeUnit unit)
            throws InterruptedException, ExecutionException, TimeoutException, IOException {

        // 1. 快速检查状态，避免在关闭后尝试发送
        if (!running.get()) {
            throw new IOException("Client is closed");
        }

        String reqId = "req-" + requestIdCounter.incrementAndGet();
        JSONObject reqToSend = new JSONObject(request.toString());
        reqToSend.put("request_id", reqId);

        CompletableFuture<JSONObject> future = new CompletableFuture<>();
        pendingRequests.put(reqId, future);

        try {
            byte[] bodyBytes = reqToSend.toString().getBytes(StandardCharsets.UTF_8);

            synchronized (outputStream) {
                // Double check inside lock
                if (!running.get()) {
                    throw new IOException("Client closed while sending");
                }
                outputStream.writeInt(bodyBytes.length);
                outputStream.write(bodyBytes);
                outputStream.flush();
            }
            return future.get(timeout, unit);
        } catch (IOException e) {
            pendingRequests.remove(reqId);
            throw e;
        } catch (InterruptedException | ExecutionException | TimeoutException e) {
            pendingRequests.remove(reqId);
            future.cancel(true);
            throw e;
        }
    }

    public JSONObject sendRequest(JSONObject request) throws Exception {
        return sendRequest(request, 10, TimeUnit.SECONDS);
    }

    /**
     * 从事件队列获取事件
     * @param timeout 超时时间
     * @param unit 时间单位
     * @return 事件的 JSONObject 形式
     * @throws InterruptedException 如果线程在等待时被中断
     * @throws TimeoutException 如果在指定时间内没有事件可用
     */
    public JSONObject getEvent(long timeout, TimeUnit unit) throws InterruptedException, TimeoutException {
        // 检查状态，如果已关闭直接抛出异常，避免死等
        if (!running.get() && eventQueue.isEmpty()) {
            throw new InterruptedException("Client is closed");
        }
        JSONObject event = eventQueue.poll(timeout, unit);
        if (event == null) {
            throw new TimeoutException("Timeout waiting for event.");
        }
        return event;
    }

    /**
     * 清空事件队列
     */
    public void clearEvents() {
        eventQueue.clear();
    }

    private void receiveLoop() {
        try {
            while (running.get()) {
                int bodyLen;
                try {
                    // 如果 socket 关闭，readInt 通常抛出 EOFException 或 SocketException
                    bodyLen = inputStream.readInt();
                } catch (EOFException | java.net.SocketException e) {
                    break; // 正常退出
                }

                byte[] bodyBytes = new byte[bodyLen];
                inputStream.readFully(bodyBytes);

                String jsonStr = new String(bodyBytes, StandardCharsets.UTF_8);
                JSONObject response = new JSONObject(jsonStr);

                if (response.has("request_id")) {
                    String reqId = response.getString("request_id");
                    CompletableFuture<JSONObject> future = pendingRequests.remove(reqId);
                    if (future != null) {
                        future.complete(response);
                    }
                } else if (response.has("event")) {
                    // 如果队列满了，offer 不会阻塞，避免阻塞接收线程
                    if (!eventQueue.offer(response)) {
                        System.err.println("Warning: Event queue full, dropping event: " + response);
                    }
                }
            }
        } catch (IOException e) {
            // 只有在运行状态下发生的 IO 异常才打印，避免 close() 导致的噪音
            if (running.get()) {
                System.err.println("Connection error: " + e.getMessage());
            }
        } catch (Exception e) {
             if (running.get()) e.printStackTrace();
        } finally {
            // 确保线程退出时彻底清理资源
            close();
        }
    }

    /**
     * 完善后的 close 逻辑
     */
    @Override
    public void close() {
        // 1. 原子性地将状态置为 false，防止多线程并发调用 close
        if (!running.compareAndSet(true, false)) {
            return;
        }

        // 2. 显式中断接收线程
        // 虽然关闭 Stream 也会让 readInt 抛异常，但 interrupt 是更标准的线程停止方式
        // 注意：不要中断当前线程（如果是 receiveLoop 内部调用的 close）
        Thread t = receiveThread;
        if (t != null && t != Thread.currentThread()) {
            t.interrupt();
        }

        // 3. 独立关闭各项资源，使用辅助方法避免一个异常导致后续资源未关闭
        // 即使包装流(Buffered/Data)关闭了，显式关闭底层资源(RAF/Socket)也是一种好习惯
        closeQuietly(inputStream);
        closeQuietly(outputStream);
        closeQuietly(unixSocket);
        closeQuietly(windowsPipeRaf);

        // 4. 快速失败所有正在等待的请求
        // 这样 sendRequest 中的 future.get() 会立刻抛出 ExecutionException，而不是等到超时
        IOException closedEx = new IOException("Client closed");
        for (CompletableFuture<JSONObject> future : pendingRequests.values()) {
            future.completeExceptionally(closedEx);
        }
        pendingRequests.clear();

        // 5. (可选) 清理事件队列
        eventQueue.clear();
    }

    /**
     * 辅助方法：安静地关闭资源，吞掉异常
     */
    private void closeQuietly(AutoCloseable resource) {
        if (resource != null) {
            try {
                resource.close();
            } catch (Exception ignored) {
                // 关闭时的异常通常可以忽略
            }
        }
    }
}