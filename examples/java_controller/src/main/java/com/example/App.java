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

    public RpcClient(String pipeName) {
        this.socketPath = "/tmp/" + pipeName;
    }

    public void connect() throws IOException {
        Path socketFile = Paths.get(socketPath);
        UnixDomainSocketAddress address = UnixDomainSocketAddress.of(socketFile);
        channel = SocketChannel.open(StandardProtocolFamily.UNIX);
        channel.connect(address);
        out = new DataOutputStream(Channels.newOutputStream(channel));
        in = new DataInputStream(Channels.newInputStream(channel));
        System.out.println("Connected to " + socketPath);
        executorService.submit(this::receiveMessages);
    }

    private void receiveMessages() {
        try {
            while (!Thread.currentThread().isInterrupted()) {
                byte[] lenBytes = new byte[4];
                in.readFully(lenBytes);
                ByteBuffer receivedLengthBuffer = ByteBuffer.wrap(lenBytes);
                receivedLengthBuffer.order(ByteOrder.BIG_ENDIAN);
                int responseLength = receivedLengthBuffer.getInt();

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
            System.out.println("Connection closed or error: " + e.getMessage());
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            close();
        }
    }

    public JSONObject sendRequest(JSONObject request) throws ExecutionException, InterruptedException, TimeoutException {
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
        } catch (IOException e) {
            pendingRequests.remove(reqId);
            future.completeExceptionally(e);
        }
        return future.get(10, TimeUnit.SECONDS);
    }
    
    public BlockingQueue<JSONObject> getEventQueue() {
        return eventQueue;
    }

    @Override
    public void close() {
        try {
            if (channel != null && channel.isOpen()) {
                channel.close();
            }
        } catch (IOException e) {
            // Ignore
        }
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
            client.connect();
            
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
        assert "success".equals(response.getString("status"));
    }

    private static String testLoadLibrary(RpcClient client) throws Exception {
        JSONObject request = new JSONObject()
            .put("command", "load_library")
            .put("payload", new JSONObject().put("path", LIBRARY_PATH));
        JSONObject response = client.sendRequest(request);
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
        client.sendRequest(callRequest); // Response will be handled by receiver thread

        // 3. Wait for and verify the event
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
