package com.kekxv;

import com.kekxv.rpc.RpcClient;
import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.util.Base64; // Import Base64 for encoding/decoding
import java.util.concurrent.TimeUnit; // Import TimeUnit for timeouts

public class App {

    // 模拟 C++ run_test 宏/函数
    private interface TestFunc {
        void run() throws Exception;
    }

    private static void runTest(String name, TestFunc func) throws Exception {
        System.out.println("\n--- Running Test: " + name + " ---");
        try {
            func.run();
            System.out.println("--- Test '" + name + "' PASSED ---");
        } catch (Exception e) {
            System.err.println("--- Test '" + name + "' FAILED: " + e.getMessage() + " ---");
            throw e; // 向上抛出以便 main 捕获
        }
    }

    // Helper to print byte arrays as hex strings
    private static String bytesToHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder();
        for (byte b : bytes) {
            sb.append(String.format("%02X", b));
        }
        return sb.toString();
    }

    public static void main(String[] args) {
        if (args.length < 1) {
            System.err.println("Usage: java -jar app.jar <pipe_name>");
            return;
        }

        String pipeName = args[0];

        // try-with-resources: 自动调用 close()，释放 pipe handle
        try (RpcClient client = new RpcClient(pipeName)) {

            client.connect();

            // RpcClient 现在内部管理事件队列，不再需要在此处传递 eventHandler
            // BlockingQueue<JSONObject> eventQueue = new LinkedBlockingQueue<>();
            // client.setEventHandler(json -> {
            //     System.out.println("<-- Received Event [" + json.optString("event") + "]: " + json);
            //     eventQueue.offer(json);
            // });

            // 全局变量保存 libId (Java 这里的闭包不像 C++ reference 那么方便，用数组绕一下)
            final String[] libraryId = {null};

            // 1. Register Point Struct
            runTest("Register Point Struct", () -> {
                JSONObject req = new JSONObject()
                    .put("command", "register_struct")
                    .put("payload", new JSONObject()
                        .put("struct_name", "Point")
                        .put("definition", new JSONArray()
                            .put(new JSONObject().put("name", "x").put("type", "int32"))
                            .put(new JSONObject().put("name", "y").put("type", "int32"))
                        )
                    );
                JSONObject res = client.sendRequest(req);
                if (!"success".equals(res.optString("status"))) {
                    throw new RuntimeException("Failed to register struct");
                }
            });

            // 2. Load Library
            runTest("Load Library", () -> {
                // 计算路径，尽量模仿 C++ fs::path 逻辑
                File buildDir = new File("build/test_lib");
                String libName = System.mapLibraryName("my_lib"); // 自动处理 .dll / .so / .dylib
                // mapLibraryName 有时会有 lib 前缀问题，做简单适配
                if (!libName.startsWith("my_lib") && !libName.startsWith("libmy_lib")) {
                     // Fallback logic if needed
                }

                // 简单拼接
                File libPath = new File(buildDir, libName.replace("libmy_lib", "my_lib"));

                JSONObject req = new JSONObject()
                    .put("command", "load_library")
                    .put("payload", new JSONObject().put("path", libPath.getAbsolutePath()));

                JSONObject res = client.sendRequest(req);
                if (!"success".equals(res.optString("status"))) {
                    throw new RuntimeException("Failed to load library");
                }
                libraryId[0] = res.getJSONObject("data").getString("library_id");
            });

            // 3. Add Function
            runTest("Add Function", () -> {
                JSONObject req = new JSONObject()
                    .put("command", "call_function")
                    .put("payload", new JSONObject()
                        .put("library_id", libraryId[0])
                        .put("function_name", "add")
                        .put("return_type", "int32")
                        .put("args", new JSONArray()
                            .put(new JSONObject().put("type", "int32").put("value", 10))
                            .put(new JSONObject().put("type", "int32").put("value", 20))
                        )
                    );
                JSONObject res = client.sendRequest(req);
                int val = res.getJSONObject("data").getJSONObject("return").getInt("value");
                if (val != 30) throw new RuntimeException("Add function failed, got " + val);
            });

            // 4. Single Callback Functionality
            runTest("Single Callback Functionality", () -> {
                client.clearEvents(); // Clear any stale events

                JSONObject regReq = new JSONObject()
                    .put("command", "register_callback")
                    .put("payload", new JSONObject()
                        .put("return_type", "void")
                        .put("args_type", new JSONArray().put("string").put("int32"))
                    );
                JSONObject regRes = client.sendRequest(regReq);
                String cbId = regRes.getJSONObject("data").getString("callback_id");
                System.out.println("Callback registered with ID: " + cbId);

                JSONObject callReq = new JSONObject()
                    .put("command", "call_function")
                    .put("payload", new JSONObject()
                        .put("library_id", libraryId[0])
                        .put("function_name", "call_my_callback")
                        .put("return_type", "void")
                        .put("args", new JSONArray()
                            .put(new JSONObject().put("type", "callback").put("value", cbId))
                            .put(new JSONObject().put("type", "string").put("value", "Hello from Java!"))
                        )
                    );
                client.sendRequest(callReq);
                System.out.println("call_my_callback returned successfully, expecting one event...");

                JSONObject event = client.getEvent(5, TimeUnit.SECONDS);
                if (event == null) throw new RuntimeException("Callback event timeout");
                if (!"invoke_callback".equals(event.optString("event"))) {
                    throw new RuntimeException("Wrong event type: " + event.optString("event"));
                }
                if (!cbId.equals(event.getJSONObject("payload").optString("callback_id"))) {
                    throw new RuntimeException("Callback ID mismatch");
                }
                if (!"Hello from Java!".equals(event.getJSONObject("payload").getJSONArray("args").getJSONObject(0).optString("value"))) {
                    throw new RuntimeException("Callback arg 0 value mismatch");
                }
                System.out.println("Successfully received and verified invoke_callback event.");

                JSONObject unregReq = new JSONObject()
                    .put("command", "unregister_callback")
                    .put("payload", new JSONObject().put("callback_id", cbId));
                client.sendRequest(unregReq);
                System.out.println("Callback unregistered successfully.");
            });

            // 5. Multi-Callback Functionality
            runTest("Multi-Callback Functionality", () -> {
                client.clearEvents(); // Clear any stale events

                JSONObject regReq = new JSONObject()
                    .put("command", "register_callback")
                    .put("payload", new JSONObject()
                        .put("return_type", "void")
                        .put("args_type", new JSONArray().put("string").put("int32"))
                    );
                JSONObject regRes = client.sendRequest(regReq);
                String multiCbId = regRes.getJSONObject("data").getString("callback_id");
                System.out.println("Multi-Callback registered with ID: " + multiCbId);

                int numCalls = 3;
                JSONObject callReq = new JSONObject()
                    .put("command", "call_function")
                    .put("payload", new JSONObject()
                        .put("library_id", libraryId[0])
                        .put("function_name", "call_multi_callbacks")
                        .put("return_type", "void")
                        .put("args", new JSONArray()
                            .put(new JSONObject().put("type", "callback").put("value", multiCbId))
                            .put(new JSONObject().put("type", "int32").put("value", numCalls))
                        )
                    );
                client.sendRequest(callReq);
                System.out.println("call_multi_callbacks returned successfully, expecting " + numCalls + " events...");

                for (int i = 0; i < numCalls; i++) {
                    JSONObject event = client.getEvent(5, TimeUnit.SECONDS);
                    if (event == null) throw new RuntimeException("Multi-callback event " + (i + 1) + " timeout");
                    if (!"invoke_callback".equals(event.optString("event"))) {
                        throw new RuntimeException("Wrong event type for call " + (i + 1) + ": " + event.optString("event"));
                    }
                    if (!multiCbId.equals(event.getJSONObject("payload").optString("callback_id"))) {
                        throw new RuntimeException("Multi-callback ID mismatch for call " + (i + 1));
                    }
                    
                    String expectedMessage = "Message from native code, call " + (i + 1);
                    int expectedValue = i + 1;
                    
                    if (!expectedMessage.equals(event.getJSONObject("payload").getJSONArray("args").getJSONObject(0).optString("value"))) {
                        throw new RuntimeException("Multi-callback arg 0 value mismatch for call " + (i + 1));
                    }
                    if (expectedValue != event.getJSONObject("payload").getJSONArray("args").getJSONObject(1).optInt("value")) {
                        throw new RuntimeException("Multi-callback arg 1 value mismatch for call " + (i + 1));
                    }
                    System.out.println("  Received and verified multi-callback event " + (i + 1) + "/" + numCalls + ": msg='" + event.getJSONObject("payload").getJSONArray("args").getJSONObject(0).optString("value") + "', val=" + event.getJSONObject("payload").getJSONArray("args").getJSONObject(1).optInt("value"));
                }
                
                JSONObject unregReq = new JSONObject()
                    .put("command", "unregister_callback")
                    .put("payload", new JSONObject().put("callback_id", multiCbId));
                client.sendRequest(unregReq);
                System.out.println("Multi-Callback unregistered successfully.");
            });


            // 6. Process Buffer Inout Functionality
            runTest("Process Buffer Inout Functionality", () -> {
                int bufferCapacity = 64;
                byte[] inputRawData = {0x05}; // Input byte 0x05
                String inputBase64 = Base64.getEncoder().encodeToString(inputRawData); // "BQ=="

                byte[] expectedRawOutputPrefix = {(byte)0xAA, 0x06, (byte)0xDE, (byte)0xAD}; // Expected output prefix

                JSONObject req = new JSONObject()
                    .put("command", "call_function")
                    .put("payload", new JSONObject()
                        .put("library_id", libraryId[0])
                        .put("function_name", "process_buffer_inout")
                        .put("return_type", "int32")
                        .put("args", new JSONArray()
                            .put(new JSONObject().put("type", "buffer").put("direction", "inout").put("size", bufferCapacity).put("value", inputBase64))
                            .put(new JSONObject().put("type", "pointer").put("target_type", "int32").put("direction", "inout").put("value", bufferCapacity))
                        )
                    );
                JSONObject res = client.sendRequest(req);

                if (!"success".equals(res.optString("status"))) {
                    throw new RuntimeException("process_buffer_inout call failed: " + res.optString("error_message"));
                }

                JSONObject data = res.getJSONObject("data");
                int retVal = data.getJSONObject("return").getInt("value");
                if (retVal != 0) throw new RuntimeException("process_buffer_inout returned non-zero");

                JSONArray outParams = data.getJSONArray("out_params");
                String outputBase64Value = "";
                int updatedSize = -1;

                for (int i = 0; i < outParams.length(); i++) {
                    JSONObject p = outParams.getJSONObject(i);
                    if (p.getInt("index") == 0) outputBase64Value = p.getString("value");
                    if (p.getInt("index") == 1) updatedSize = p.getInt("value");
                }

                if (outputBase64Value.isEmpty()) throw new RuntimeException("Output buffer not received or empty");
                if (updatedSize == -1) throw new RuntimeException("Updated size not received");

                byte[] decodedOutputBytes = Base64.getDecoder().decode(outputBase64Value);

                if (decodedOutputBytes.length != bufferCapacity) {
                    throw new RuntimeException("Decoded buffer length mismatch: expected " + bufferCapacity + ", got " + decodedOutputBytes.length);
                }

                // Verify prefix
                for (int i = 0; i < expectedRawOutputPrefix.length; i++) {
                    if (decodedOutputBytes[i] != expectedRawOutputPrefix[i]) {
                        throw new RuntimeException("Decoded buffer prefix mismatch at index " + i);
                    }
                }
                // Verify remaining zeros
                for (int i = expectedRawOutputPrefix.length; i < bufferCapacity; i++) {
                    if (decodedOutputBytes[i] != 0x00) {
                        throw new RuntimeException("Remaining buffer not filled with zeros at index " + i);
                    }
                }

                if (updatedSize != expectedRawOutputPrefix.length) {
                    throw new RuntimeException("Updated size mismatch: expected " + expectedRawOutputPrefix.length + ", got " + updatedSize);
                }
                System.out.println("Buffer content verified (prefix: " + bytesToHex(expectedRawOutputPrefix) + ", Size: " + updatedSize + ")");
            });

        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}
