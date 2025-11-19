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
    // 存储等待响应的 Future
    private final ConcurrentMap<String, CompletableFuture<JSONObject>> pendingRequests = new ConcurrentHashMap<>();
    private final BlockingQueue<JSONObject> eventQueue = new LinkedBlockingQueue<>();

    public RpcClient(String pipeName) {
        this.socketPath = "/tmp/" + pipeName;
    }

    /**
     * 连接服务器，支持超时时间
     * @param timeoutMillis 连接超时时间（毫秒）
     */
    public void connect(long timeoutMillis) throws IOException {
        Path socketFile = Paths.get(socketPath);
        UnixDomainSocketAddress address = UnixDomainSocketAddress.of(socketFile);

        channel = SocketChannel.open(StandardProtocolFamily.UNIX);

        // 1. 设置为非阻塞模式以便控制连接超时
        channel.configureBlocking(false);

        boolean connected = channel.connect(address);

        if (!connected) {
            // 使用 Selector 等待连接完成
            try (Selector selector = Selector.open()) {
                channel.register(selector, SelectionKey.OP_CONNECT);

                // select 返回 0 表示超时
                if (selector.select(timeoutMillis) == 0) {
                    throw new IOException("Connection timed out after " + timeoutMillis + "ms");
                }

                // 完成连接
                if (!channel.finishConnect()) {
                    throw new IOException("Failed to finish connection");
                }
            }
        }

        // 2. 恢复为阻塞模式 (DataInputStream 需要阻塞模式)
        channel.configureBlocking(true);

        // 3. 初始化流
        out = new DataOutputStream(Channels.newOutputStream(channel));
        in = new DataInputStream(Channels.newInputStream(channel));

        System.out.println("Connected to " + socketPath);
        executorService.submit(this::receiveMessages);
    }

    private void receiveMessages() {
        try {
            while (!Thread.currentThread().isInterrupted() && channel.isOpen()) {
                // 读取长度头 (4字节)
                byte[] lenBytes = new byte[4];
                in.readFully(lenBytes); // 这里会阻塞，直到收到数据或连接断开

                ByteBuffer receivedLengthBuffer = ByteBuffer.wrap(lenBytes);
                receivedLengthBuffer.order(ByteOrder.BIG_ENDIAN);
                int responseLength = receivedLengthBuffer.getInt();

                // 读取消息体
                byte[] responseBytes = new byte[responseLength];
                in.readFully(responseBytes);

                String responseStr = new String(responseBytes, "UTF-8");
                JSONObject response = new JSONObject(responseStr);

                if (response.has("request_id")) {
                    String reqId = response.getString("request_id");
                    CompletableFuture<JSONObject> future = pendingRequests.remove(reqId);
                    if (future != null) {
                        future.complete(response);
                    }
                } else if (response.has("event")) {
                    System.out.println("Received event: " + response);
                    eventQueue.put(response);
                }
            }
        } catch (IOException e) {
            // 连接关闭是正常的退出流程，如果是意外关闭则打印日志
            if (!Thread.currentThread().isInterrupted()) {
                System.out.println("Connection closed or error: " + e.getMessage());
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            // 确保发生错误时清理资源
            close();
        }
    }

    /**
     * 发送请求，支持自定义超时
     */
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
            lengthBuffer.order(ByteOrder.BIG_ENDIAN);
            lengthBuffer.putInt(requestBytes.length);

            synchronized (out) {
                out.write(lengthBuffer.array());
                out.write(requestBytes);
                out.flush();
            }

            // 等待结果，直到超时
            return future.get(timeout, unit);

        } catch (IOException e) {
            // 发送失败，立即清理
            pendingRequests.remove(reqId);
            throw new ExecutionException("Failed to send request", e);
        } catch (TimeoutException e) {
            // 超时发生，必须清理 map 中的 future，防止内存泄漏
            pendingRequests.remove(reqId);
            // 如果超时，我们可以选择让 future 异常结束，以防后续数据迟到导致逻辑混乱
            future.completeExceptionally(e);
            throw e;
        }
    }

    // 提供一个默认超时的重载方法 (例如 5 秒)
    public JSONObject sendRequest(JSONObject request) throws ExecutionException, InterruptedException, TimeoutException {
        return sendRequest(request, 5, TimeUnit.SECONDS);
    }

    public BlockingQueue<JSONObject> getEventQueue() {
        return eventQueue;
    }

    @Override
    public void close() {
        try {
            // 关闭 Channel 会导致 receiveMessages 中的 readFully 抛出异常从而结束线程
            if (channel != null && channel.isOpen()) {
                channel.close();
            }
        } catch (IOException e) {
            // Ignore
        }
        // 清理所有挂起的请求，通知它们连接已关闭
        for (CompletableFuture<JSONObject> future : pendingRequests.values()) {
            future.completeExceptionally(new IOException("Client is closing"));
        }
        pendingRequests.clear();
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