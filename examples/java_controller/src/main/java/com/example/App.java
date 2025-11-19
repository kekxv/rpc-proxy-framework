package com.example;

import com.example.rpc.RpcClient;
import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

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

    public static void main(String[] args) {
        if (args.length < 1) {
            System.err.println("Usage: java -jar app.jar <pipe_name>");
            return;
        }

        String pipeName = args[0];

        // try-with-resources: 自动调用 close()，释放 pipe handle
        try (RpcClient client = new RpcClient(pipeName)) {

            client.connect();

            // 准备一个队列来同步等待 Callback 事件 (类似 C++ 的 simple example)
            BlockingQueue<JSONObject> eventQueue = new LinkedBlockingQueue<>();
            client.setEventHandler(json -> {
                System.out.println("<-- Received Event [" + json.optString("event") + "]: " + json);
                eventQueue.offer(json);
            });

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

            // 4. Callback
            runTest("Callback Functionality", () -> {
                JSONObject regReq = new JSONObject()
                    .put("command", "register_callback")
                    .put("payload", new JSONObject()
                        .put("return_type", "void")
                        .put("args_type", new JSONArray().put("string").put("int32"))
                    );
                JSONObject regRes = client.sendRequest(regReq);
                String cbId = regRes.getJSONObject("data").getString("callback_id");

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

                // 等待事件
                JSONObject event = eventQueue.poll(2, TimeUnit.SECONDS);
                if (event == null) throw new RuntimeException("Callback event timeout");
                if (!"invoke_callback".equals(event.optString("event"))) {
                    throw new RuntimeException("Wrong event type");
                }
            });

            // 5. Write Out Buff
            runTest("Write Out Buff Functionality", () -> {
                int bufferCapacity = 64;
                JSONObject req = new JSONObject()
                    .put("command", "call_function")
                    .put("payload", new JSONObject()
                        .put("library_id", libraryId[0])
                        .put("function_name", "writeOutBuff")
                        .put("return_type", "int32")
                        .put("args", new JSONArray()
                            .put(new JSONObject().put("type", "buffer").put("direction", "out").put("size", bufferCapacity))
                            .put(new JSONObject().put("type", "pointer").put("target_type", "int32").put("direction", "inout").put("value", bufferCapacity))
                        )
                    );
                JSONObject res = client.sendRequest(req);

                JSONObject data = res.getJSONObject("data");
                int retVal = data.getJSONObject("return").getInt("value");
                if (retVal != 0) throw new RuntimeException("writeOutBuff returned non-zero");

                JSONArray outParams = data.getJSONArray("out_params");
                String bufferContent = "";
                int updatedSize = -1;

                for (int i = 0; i < outParams.length(); i++) {
                    JSONObject p = outParams.getJSONObject(i);
                    if (p.getInt("index") == 0) bufferContent = p.getString("value");
                    if (p.getInt("index") == 1) updatedSize = p.getInt("value");
                }

                String expected = "Hello from writeOutBuff!";
                if (!expected.equals(bufferContent)) throw new RuntimeException("Buffer content mismatch");
                if (updatedSize != expected.length()) throw new RuntimeException("Size mismatch");
            });

        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}