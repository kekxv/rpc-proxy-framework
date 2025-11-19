package com.example.rpc;

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
import java.util.function.Consumer;

/**
 * 跨平台 RPC 客户端 (Java 8+)
 * <p>
 * 1. Windows: 使用 RandomAccessFile 访问 Named Pipe
 * 2. Linux/Mac: 使用 junixsocket 访问 Unix Domain Socket
 * 3. 支持 try-with-resources (AutoCloseable)
 */
public class RpcClient implements AutoCloseable {

    // 基础资源
    private final String pipeName;
    private final AtomicBoolean running = new AtomicBoolean(false);
    private Thread receiveThread;

    // IO 流 (统一抽象)
    private DataInputStream inputStream;
    private DataOutputStream outputStream;

    // Windows 特有资源
    private RandomAccessFile windowsPipeRaf;
    // Linux/Mac 特有资源
    private Closeable unixSocket; // 使用 Closeable 避免直接依赖 Socket 类导致编译问题

    // 状态管理
    private final AtomicInteger requestIdCounter = new AtomicInteger(0);
    private final Map<String, CompletableFuture<JSONObject>> pendingRequests = new ConcurrentHashMap<>();
    private Consumer<JSONObject> eventHandler = (json) -> System.out.println("Unhandled Event: " + json);

    public RpcClient(String pipeName) {
        this.pipeName = pipeName;
    }

    /**
     * 连接逻辑，自动判断操作系统
     */
    public void connect() throws IOException {
        String os = System.getProperty("os.name").toLowerCase();

        if (os.contains("win")) {
            connectWindows();
        } else {
            connectUnix();
        }

        running.set(true);

        // 启动接收线程
        receiveThread = new Thread(this::receiveLoop, "Rpc-Receiver-" + pipeName);
        receiveThread.setDaemon(true); // 守护线程，防止阻塞 JVM 退出
        receiveThread.start();

        System.out.println("Connected to " + pipeName);
    }

    // --- Windows 实现: 基于文件的 Named Pipe ---
    private void connectWindows() throws IOException {
        // Java 中访问 Windows 命名管道的标准方式是当做文件处理
        // 路径格式: \\.\pipe\name
        String path = "\\\\.\\pipe\\" + pipeName;
        try {
            // "rw" 模式打开
            windowsPipeRaf = new RandomAccessFile(path, "rw");

            // 包装成流以便使用 readInt/writeBytes 等方法
            // RandomAccessFile 自身不是 Stream，需要适配
            InputStream is = new InputStream() {
                @Override
                public int read() throws IOException { return windowsPipeRaf.read(); }
                @Override
                public int read(byte[] b, int off, int len) throws IOException { return windowsPipeRaf.read(b, off, len); }
            };
            OutputStream os = new OutputStream() {
                @Override
                public void write(int b) throws IOException { windowsPipeRaf.write(b); }
                @Override
                public void write(byte[] b, int off, int len) throws IOException { windowsPipeRaf.write(b, off, len); }
            };

            this.inputStream = new DataInputStream(new BufferedInputStream(is));
            this.outputStream = new DataOutputStream(new BufferedOutputStream(os));

        } catch (FileNotFoundException e) {
            throw new IOException("Could not connect to Windows pipe: " + path + " (Check if server is running)", e);
        }
    }

    // --- Linux/Unix 实现: 基于 junixsocket ---
    private void connectUnix() throws IOException {
        // 路径格式: /tmp/name
        File socketFile = new File("/tmp/" + pipeName);

        // 使用 junixsocket 库
        AFUNIXSocket socket = AFUNIXSocket.newInstance();
        socket.connect(AFUNIXSocketAddress.of(socketFile));

        this.unixSocket = socket;
        this.inputStream = new DataInputStream(socket.getInputStream());
        this.outputStream = new DataOutputStream(socket.getOutputStream());
    }

    /**
     * 发送请求
     */
    public JSONObject sendRequest(JSONObject request, long timeout, TimeUnit unit)
            throws InterruptedException, ExecutionException, TimeoutException, IOException {

        String reqId = "req-" + requestIdCounter.incrementAndGet();
        // 拷贝一份避免副作用
        JSONObject reqToSend = new JSONObject(request.toString());
        reqToSend.put("request_id", reqId);

        CompletableFuture<JSONObject> future = new CompletableFuture<>();
        pendingRequests.put(reqId, future);

        try {
            byte[] bodyBytes = reqToSend.toString().getBytes(StandardCharsets.UTF_8);

            // 发送锁，防止多线程写入错乱
            synchronized (outputStream) {
                // C++ htonl 对应 Java 的 writeInt (Big Endian)
                outputStream.writeInt(bodyBytes.length);
                outputStream.write(bodyBytes);
                outputStream.flush();
            }
            System.out.println("--> Sending Request [" + reqToSend.optString("command") + "] id=" + reqId);

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

    public void setEventHandler(Consumer<JSONObject> handler) {
        this.eventHandler = handler;
    }

    /**
     * 接收线程循环 (对应 C++ 的 receive_messages)
     */
    private void receiveLoop() {
        try {
            while (running.get()) {
                // 1. 读取长度 (4 bytes, Big Endian)
                // readInt 是阻塞的，直到读到数据或 EOF
                int bodyLen;
                try {
                    bodyLen = inputStream.readInt();
                } catch (EOFException e) {
                    // 连接关闭
                    break;
                }

                // C++ ntohl 对应 Java readInt (默认就是网络字节序)

                // 2. 读取 Body
                byte[] bodyBytes = new byte[bodyLen];
                inputStream.readFully(bodyBytes); // readFully 确保读满 buffer

                String jsonStr = new String(bodyBytes, StandardCharsets.UTF_8);
                JSONObject response = new JSONObject(jsonStr);

                // 3. 分发处理
                if (response.has("request_id")) {
                    String reqId = response.getString("request_id");
                    CompletableFuture<JSONObject> future = pendingRequests.remove(reqId);
                    if (future != null) {
                        // C++: promise->set_value(response)
                        future.complete(response);
                        System.out.println("<-- Received Response id=" + reqId);
                    }
                } else if (response.has("event")) {
                    if (eventHandler != null) {
                        eventHandler.accept(response);
                    }
                }
            }
        } catch (IOException e) {
            if (running.get()) {
                System.err.println("Connection broken: " + e.getMessage());
            }
        } finally {
            close(); // 确保资源清理
        }
    }

    @Override
    public void close() {
        if (running.compareAndSet(true, false)) {
            System.out.println("Closing connection...");

            // 1. 关闭流/Socket
            try {
                if (inputStream != null) inputStream.close();
                if (outputStream != null) outputStream.close();
                if (windowsPipeRaf != null) windowsPipeRaf.close();
                if (unixSocket != null) unixSocket.close();
            } catch (IOException ignored) {}

            // 2. 清理 Pending Requests
            for (CompletableFuture<JSONObject> future : pendingRequests.values()) {
                future.completeExceptionally(new IOException("Client closed"));
            }
            pendingRequests.clear();
        }
    }
}