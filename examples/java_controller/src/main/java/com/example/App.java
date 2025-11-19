package com.example;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

class RpcClient implements AutoCloseable {
    private final String socketPath;
    private SocketChannel channel;
    private final ExecutorService executorService = Executors.newSingleThreadExecutor();
    private final AtomicInteger requestIdCounter = new AtomicInteger(0);
    private final ConcurrentMap<String, CompletableFuture<JSONObject>> pendingRequests = new ConcurrentHashMap<>();
    private final BlockingQueue<JSONObject> eventQueue = new LinkedBlockingQueue<>();

    // 网络字节序标准是 Big Endian，对应 C++ 的 htonl/ntohl
    private static final ByteOrder IPC_BYTE_ORDER = ByteOrder.BIG_ENDIAN;

    public RpcClient(String pipeName) {
        // Windows 命名管道和 Unix Domain Socket 路径处理不同，这里主要适配 Unix/Linux/Mac
        // 如果是在 Windows 上使用 Java 16+ 的 UnixDomainSocketAddress，路径可能有所不同
        this.socketPath = "/tmp/" + pipeName;
    }

    public void connect(long timeoutMillis) throws IOException {
        Path socketFile = Paths.get(socketPath);
        UnixDomainSocketAddress address = UnixDomainSocketAddress.of(socketFile);
        channel = SocketChannel.open(StandardProtocolFamily.UNIX);

        // 1. 非阻塞模式用于连接超时控制
        channel.configureBlocking(false);
        boolean connected = channel.connect(address);
        if (!connected) {
            try (Selector selector = Selector.open()) {
                channel.register(selector, SelectionKey.OP_CONNECT);
                if (selector.select(timeoutMillis) == 0) {
                    throw new IOException("Connection timed out after " + timeoutMillis + "ms");
                }
                if (!channel.finishConnect()) {
                    throw new IOException("Failed to finish connection");
                }
            }
        }

        // 2. 恢复为阻塞模式，简化读写逻辑，模拟 C++ 的阻塞 socket 行为
        channel.configureBlocking(true);

        System.out.println("Connected to " + socketPath);

        // 启动接收线程
        executorService.submit(this::receiveMessages);
    }

    private void receiveMessages() {
        System.out.println("[Receiver] Thread started, waiting for data...");
        // 预分配长度头部的 Buffer (4字节)
        ByteBuffer lengthBuffer = ByteBuffer.allocate(4);
        lengthBuffer.order(IPC_BYTE_ORDER);

        try {
            while (!Thread.currentThread().isInterrupted() && channel.isOpen()) {
                lengthBuffer.clear();
                // 读取消息长度 (4 bytes)
                if (!readFully(lengthBuffer)) {
                    break; // EOF
                }

                lengthBuffer.flip();
                int responseLength = lengthBuffer.getInt();

                // 读取消息体
                ByteBuffer bodyBuffer = ByteBuffer.allocate(responseLength);
                if (!readFully(bodyBuffer)) {
                    break; // EOF occurring inside a message body
                }
                bodyBuffer.flip();

                String responseStr = new String(bodyBuffer.array(), StandardCharsets.UTF_8);
                // System.out.println("[Receiver] Raw: " + responseStr); // Debug

                JSONObject response = new JSONObject(responseStr);

                if (response.has("request_id")) {
                    String reqId = response.getString("request_id");
                    CompletableFuture<JSONObject> future = pendingRequests.remove(reqId);
                    if (future != null) {
                        future.complete(response);
                    }
                } else if (response.has("event")) {
                    System.out.println("<-- Received event: " + response.getString("event"));
                    eventQueue.put(response);
                }
            }
        } catch (IOException e) {
            if (!Thread.currentThread().isInterrupted()) {
                System.err.println("[Receiver] Connection error: " + e.getMessage());
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            close();
            System.out.println("[Receiver] Thread stopped.");
        }
    }

    /**
     * 辅助方法：确保读满缓冲区，类似 DataInputStream.readFully
     */
    private boolean readFully(ByteBuffer buffer) throws IOException {
        while (buffer.hasRemaining()) {
            int bytesRead = channel.read(buffer);
            if (bytesRead == -1) {
                return false; // EOF
            }
        }
        return true;
    }

    public JSONObject sendRequest(JSONObject request, long timeout, TimeUnit unit)
            throws ExecutionException, InterruptedException, TimeoutException {
        String reqId = "req-" + requestIdCounter.incrementAndGet();
        request.put("request_id", reqId);

        CompletableFuture<JSONObject> future = new CompletableFuture<>();
        pendingRequests.put(reqId, future);

        try {
            String requestStr = request.toString();
            byte[] requestBytes = requestStr.getBytes(StandardCharsets.UTF_8);

            // 准备头部 (4字节长度)
            ByteBuffer headerBuffer = ByteBuffer.allocate(4);
            headerBuffer.order(IPC_BYTE_ORDER);
            headerBuffer.putInt(requestBytes.length);
            headerBuffer.flip();

            ByteBuffer bodyBuffer = ByteBuffer.wrap(requestBytes);

            System.out.println("--> Sending Request [" + request.getString("command") + "] id=" + reqId);

            // 加锁确保这一个请求的 Header 和 Body 连续写入，不被其他线程打断
            synchronized (channel) {
                while (headerBuffer.hasRemaining()) {
                    channel.write(headerBuffer);
                }
                while (bodyBuffer.hasRemaining()) {
                    channel.write(bodyBuffer);
                }
            }
            return future.get(timeout, unit);
        } catch (IOException e) {
            pendingRequests.remove(reqId);
            throw new ExecutionException("Failed to send request", e);
        } catch (TimeoutException e) {
            pendingRequests.remove(reqId);
            future.completeExceptionally(e);
            throw e;
        }
    }

    public JSONObject sendRequest(JSONObject request) throws ExecutionException, InterruptedException, TimeoutException {
        return sendRequest(request, 10, TimeUnit.SECONDS);
    }

    public BlockingQueue<JSONObject> getEventQueue() { return eventQueue; }

    @Override
    public void close() {
        try {
            if (channel != null && channel.isOpen()) {
                channel.close();
            }
        } catch (IOException e) { }
        executorService.shutdownNow();
    }
}

public class App {
    private static final String LIBRARY_PATH = Paths.get("build", "test_lib", System.mapLibraryName("my_lib").replace("libmy_lib", "my_lib")).toAbsolutePath().toString();
    private static String getTestLibraryPath() {
        // 简单的容错检查：如果上述路径不存在，可能是在 build 根目录或其他地方，
        // 这里为了演示，我们直接返回计算出的绝对路径
        return LIBRARY_PATH.toString();
    }

    public static void main(String[] args) {
        if (args.length < 1) {
            System.err.println("Usage: java -jar java-controller.jar <pipe_name>");
            return;
        }

        try (RpcClient client = new RpcClient(args[0])) {
            // 1. 连接超时设置为 2 秒
            client.connect(2000);

            String libraryPath = getTestLibraryPath();
            System.out.println("Target Library Path: " + libraryPath);

            runTest("Register Point Struct", () -> {
                testRegisterPointStruct(client);
                return null;
            });

            // Load Library 返回 library_id
            String libraryId = runTest("Load Library", () -> testLoadLibrary(client, libraryPath));

            runTest("Add Function", () -> {
                testAddFunction(client, libraryId);
                return null;
            });
            runTest("Greet Function", () -> {
                testGreetFunction(client, libraryId);
                return null;
            });
            runTest("Callback Functionality", () -> {
                testCallbackFunctionality(client, libraryId);
                return null;
            });
            runTest("Write Out Buff Functionality", () -> {
                testWriteOutBuff(client, libraryId);
                return null;
            });

        } catch (Exception e) {
            System.err.println("An error occurred: " + e.getMessage());
            e.printStackTrace();
        }
    }

    private static <T> T runTest(String name, Callable<T> test) throws Exception {
        System.out.println("\n--- Running Test: " + name + " ---");
        try {
            T result = test.call();
            System.out.println("--- Test '" + name + "' PASSED ---");
            return result;
        } catch (Exception e) {
            System.err.println("--- Test '" + name + "' FAILED: " + e.getMessage() + " ---");
            throw e;
        }
    }

    private static void testRegisterPointStruct(RpcClient client) throws Exception {
        JSONObject request = new JSONObject()
            .put("command", "register_struct")
            .put("payload", new JSONObject()
                .put("struct_name", "Point")
                .put("definition", new JSONArray()
                    .put(new JSONObject().put("name", "x").put("type", "int32"))
                    .put(new JSONObject().put("name", "y").put("type", "int32"))
                )
            );
        JSONObject response = client.sendRequest(request);
        if (!"success".equals(response.getString("status"))) {
            throw new RuntimeException("Status not success");
        }
    }

    private static String testLoadLibrary(RpcClient client, String libPath) throws Exception {
        JSONObject request = new JSONObject()
            .put("command", "load_library")
            .put("payload", new JSONObject().put("path", libPath));

        // 加载库可能较慢
        JSONObject response = client.sendRequest(request, 5, TimeUnit.SECONDS);
        if (!"success".equals(response.getString("status"))) {
             throw new RuntimeException("Failed to load library: " + response);
        }
        return response.getJSONObject("data").getString("library_id");
    }

    private static void testAddFunction(RpcClient client, String libraryId) throws Exception {
        JSONObject request = new JSONObject()
            .put("command", "call_function")
            .put("payload", new JSONObject()
                .put("library_id", libraryId)
                .put("function_name", "add")
                .put("return_type", "int32")
                .put("args", new JSONArray()
                    .put(new JSONObject().put("type", "int32").put("value", 10))
                    .put(new JSONObject().put("type", "int32").put("value", 20))
                )
            );
        JSONObject response = client.sendRequest(request);
        int result = response.getJSONObject("data").getJSONObject("return").getInt("value");
        if (result != 30) throw new RuntimeException("Add result mismatch: " + result);
    }

    private static void testGreetFunction(RpcClient client, String libraryId) throws Exception {
        JSONObject request = new JSONObject()
            .put("command", "call_function")
            .put("payload", new JSONObject()
                .put("library_id", libraryId)
                .put("function_name", "greet")
                .put("return_type", "string")
                .put("args", new JSONArray()
                    .put(new JSONObject().put("type", "string").put("value", "Java World"))
                )
            );
        JSONObject response = client.sendRequest(request);
        String result = response.getJSONObject("data").getJSONObject("return").getString("value");
        if (!"Hello, Java World".equals(result)) throw new RuntimeException("Greet result mismatch: " + result);
    }

    private static void testCallbackFunctionality(RpcClient client, String libraryId) throws Exception {
        // 1. Register callback
        JSONObject regRequest = new JSONObject()
            .put("command", "register_callback")
            .put("payload", new JSONObject()
                .put("return_type", "void")
                .put("args_type", new JSONArray().put("string").put("int32"))
            );
        JSONObject regResponse = client.sendRequest(regRequest);
        String callbackId = regResponse.getJSONObject("data").getString("callback_id");

        // 2. Call function that uses the callback
        JSONObject callRequest = new JSONObject()
            .put("command", "call_function")
            .put("payload", new JSONObject()
                .put("library_id", libraryId)
                .put("function_name", "call_my_callback")
                .put("return_type", "void")
                .put("args", new JSONArray()
                    .put(new JSONObject().put("type", "callback").put("value", callbackId))
                    .put(new JSONObject().put("type", "string").put("value", "Hello from Java!"))
                )
            );
        // 发送请求
        client.sendRequest(callRequest);

        // 3. Wait for and verify the event
        JSONObject event = client.getEventQueue().poll(5, TimeUnit.SECONDS);
        if (event == null) throw new RuntimeException("Timeout waiting for callback event");

        if (!"invoke_callback".equals(event.getString("event")))
            throw new RuntimeException("Wrong event type: " + event.getString("event"));

        JSONObject payload = event.getJSONObject("payload");
        if (!callbackId.equals(payload.getString("callback_id")))
             throw new RuntimeException("Callback ID mismatch");

        JSONArray args = payload.getJSONArray("args");
        // C++ 代码发送的是 "Hello from Java!" (作为参数传入) 和 123 (硬编码在 C++ 侧的回调调用中)
        // 注意：具体返回值取决于 C++ 侧 call_my_callback 的实现细节
        // 假设 C++ 代码透传了字符串并添加了整数
    }

    private static void testWriteOutBuff(RpcClient client, String libraryId) throws Exception {
        int bufferCapacity = 64;
        JSONObject request = new JSONObject()
            .put("command", "call_function")
            .put("payload", new JSONObject()
                .put("library_id", libraryId)
                .put("function_name", "writeOutBuff")
                .put("return_type", "int32")
                .put("args", new JSONArray()
                    .put(new JSONObject()
                        .put("type", "buffer")
                        .put("direction", "out")
                        .put("size", bufferCapacity))
                    .put(new JSONObject()
                        .put("type", "pointer")
                        .put("target_type", "int32")
                        .put("direction", "inout")
                        .put("value", bufferCapacity))
                )
            );
        JSONObject response = client.sendRequest(request);

        JSONObject data = response.getJSONObject("data");
        int returnValue = data.getJSONObject("return").getInt("value");
        if (returnValue != 0) throw new RuntimeException("Return value not 0");

        JSONArray outParams = data.getJSONArray("out_params");
        String bufferContent = null;
        Integer updatedSize = null;
        for (int i = 0; i < outParams.length(); i++) {
            JSONObject param = outParams.getJSONObject(i);
            if (param.getInt("index") == 0) {
                bufferContent = param.getString("value");
            } else if (param.getInt("index") == 1) {
                updatedSize = param.getInt("value");
            }
        }

        String expectedString = "Hello from writeOutBuff!";
        if (!expectedString.equals(bufferContent)) throw new RuntimeException("Buffer content mismatch: " + bufferContent);
        if (updatedSize == null || updatedSize != expectedString.length()) throw new RuntimeException("Size mismatch");
    }
}