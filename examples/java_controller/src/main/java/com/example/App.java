package com.example;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.Channels;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.SocketChannel;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

class RpcClient implements AutoCloseable {
    private final String socketPath;
    private SocketChannel channel;
    private DataOutputStream out;
    private DataInputStream in;
    private final ExecutorService executorService = Executors.newSingleThreadExecutor();
    private final AtomicInteger requestIdCounter = new AtomicInteger(0);
    private final ConcurrentMap<String, CompletableFuture<JSONObject>> pendingRequests = new ConcurrentHashMap<>();
    private final BlockingQueue<JSONObject> eventQueue = new LinkedBlockingQueue<>();

//     private static final ByteOrder IPC_BYTE_ORDER = ByteOrder.LITTLE_ENDIAN;
    private static final ByteOrder IPC_BYTE_ORDER = ByteOrder.BIG_ENDIAN;

    public RpcClient(String pipeName) {
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

        // 2. !!! 恢复为阻塞模式 !!! DataInputStream 必须工作在阻塞模式下
        channel.configureBlocking(true);

        // 3. 使用缓冲流，防止碎片化写入
        out = new DataOutputStream(new java.io.BufferedOutputStream(Channels.newOutputStream(channel)));
        in = new DataInputStream(new java.io.BufferedInputStream(Channels.newInputStream(channel)));

        System.out.println("Connected to " + socketPath);

        // 启动接收线程
        executorService.submit(this::receiveMessages);
    }

    private void receiveMessages() {
        System.out.println("[Receiver] Thread started, waiting for data...");
        try {
            while (!Thread.currentThread().isInterrupted() && channel.isOpen()) {
                byte[] lenBytes = new byte[4];
                // 这里会阻塞等待，直到有数据过来
                in.readFully(lenBytes);

                ByteBuffer receivedLengthBuffer = ByteBuffer.wrap(lenBytes);
                receivedLengthBuffer.order(IPC_BYTE_ORDER);
                int responseLength = receivedLengthBuffer.getInt();

                // System.out.println("[Receiver] Got message length: " + responseLength); // 调试用

                byte[] responseBytes = new byte[responseLength];
                in.readFully(responseBytes);
                String responseStr = new String(responseBytes, "UTF-8");

                // System.out.println("[Receiver] Body: " + responseStr); // 调试用

                JSONObject response = new JSONObject(responseStr);

                if (response.has("request_id")) {
                    String reqId = response.getString("request_id");
                    CompletableFuture<JSONObject> future = pendingRequests.remove(reqId);
                    if (future != null) {
                        future.complete(response);
                    } else {
                        System.err.println("[Receiver] Warning: No pending future for reqId: " + reqId);
                    }
                } else if (response.has("event")) {
                    System.out.println("Received event: " + response);
                    eventQueue.put(response);
                }
            }
        } catch (IOException e) {
            if (!Thread.currentThread().isInterrupted()) {
                System.out.println("[Receiver] Connection closed: " + e.getMessage());
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            close();
        }
    }

    public JSONObject sendRequest(JSONObject request, long timeout, TimeUnit unit)
            throws ExecutionException, InterruptedException, TimeoutException {
        String reqId = "req-" + requestIdCounter.incrementAndGet();
        request.put("request_id", reqId);

        CompletableFuture<JSONObject> future = new CompletableFuture<>();
        pendingRequests.put(reqId, future);

        try {
            String requestStr = request.toString();
            byte[] requestBytes = requestStr.getBytes("UTF-8");

            ByteBuffer lengthBuffer = ByteBuffer.allocate(4);
            lengthBuffer.order(IPC_BYTE_ORDER);
            lengthBuffer.putInt(requestBytes.length);

            synchronized (out) {
                out.write(lengthBuffer.array());
                out.write(requestBytes);
                out.flush(); // 必须 Flush，否则数据可能滞留在 Buffer 中
            }
            return future.get(timeout, unit);
        } catch (IOException e) {
            pendingRequests.remove(reqId);
            throw new ExecutionException("Failed to send request", e);
        } catch (TimeoutException e) {
            pendingRequests.remove(reqId);
            future.completeExceptionally(e);
            System.err.println("Request " + reqId + " timed out!");
            throw e;
        }
    }

    public JSONObject sendRequest(JSONObject request) throws ExecutionException, InterruptedException, TimeoutException {
        return sendRequest(request, 5, TimeUnit.SECONDS);
    }

    // ... 其余 getEventQueue 和 close 方法保持不变 ...
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

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("Usage: java -jar java-controller.jar <pipe_name>");
            return;
        }

        try (RpcClient client = new RpcClient(args[0])) {
            // 1. 连接超时设置为 2 秒
            client.connect(2000);

            runTest("Register Point Struct", () -> {
                testRegisterPointStruct(client);
                return null;
            });
            String libraryId = runTest("Load Library", () -> testLoadLibrary(client));
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

    // 这里的测试方法现在使用默认的 sendRequest (5秒超时) 或显式指定超时

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
        // 使用默认超时 (5s)
        JSONObject response = client.sendRequest(request);
        assert "success".equals(response.getString("status"));
    }

    private static String testLoadLibrary(RpcClient client) throws Exception {
        JSONObject request = new JSONObject()
            .put("command", "load_library")
            .put("payload", new JSONObject().put("path", LIBRARY_PATH));
        // 加载库可能较慢，给 10 秒超时
        JSONObject response = client.sendRequest(request, 10, TimeUnit.SECONDS);
        assert "success".equals(response.getString("status"));
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
        assert "success".equals(response.getString("status"));
        int result = response.getJSONObject("data").getJSONObject("return").getInt("value");
        assert result == 30;
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
        assert "success".equals(response.getString("status"));
        String result = response.getJSONObject("data").getJSONObject("return").getString("value");
        assert "Hello, Java World".equals(result);
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
        assert "success".equals(regResponse.getString("status"));
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
        // 这是一个异步触发，我们不需要等待很长时间
        client.sendRequest(callRequest, 2, TimeUnit.SECONDS);

        // 3. Wait for and verify the event (这里是事件队列的超时)
        JSONObject event = client.getEventQueue().poll(5, TimeUnit.SECONDS);
        assert event != null;
        assert "invoke_callback".equals(event.getString("event"));
        JSONObject payload = event.getJSONObject("payload");
        assert callbackId.equals(payload.getString("callback_id"));
        JSONArray args = payload.getJSONArray("args");
        assert "Hello from Java!".equals(args.getJSONObject(0).getString("value"));
        assert 123 == args.getJSONObject(1).getInt("value");
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
        assert "success".equals(response.getString("status"));

        JSONObject data = response.getJSONObject("data");
        int returnValue = data.getJSONObject("return").getInt("value");
        assert returnValue == 0;

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
        assert expectedString.equals(bufferContent);
        assert updatedSize != null && updatedSize == expectedString.length();
    }
}