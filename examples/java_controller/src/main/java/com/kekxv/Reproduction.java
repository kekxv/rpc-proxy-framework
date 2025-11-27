package com.kekxv;

import com.kekxv.rpc.RpcClient;
import org.json.JSONArray;
import org.json.JSONObject;

import java.util.concurrent.TimeUnit;

public class Reproduction {
    public static void main(String[] args) {
        if (args.length < 1) {
            System.err.println("Usage: java Reproduction <pipe_name>");
            System.exit(1);
        }
        String pipeName = args[0];
        System.out.println("Connecting to pipe: " + pipeName);

        try (RpcClient client = new RpcClient(pipeName)) {
            client.connect();
            System.out.println("Connected.");

            // 1. Register a callback
            JSONObject registerCallbackReq = new JSONObject();
            registerCallbackReq.put("command", "register_callback");
            JSONObject payload = new JSONObject();
            payload.put("return_type", "void");
            // Callback signature in C: void (*callback_fn)(const char* message, int value)
            payload.put("args_type", new JSONArray().put("string").put("int32"));
            registerCallbackReq.put("payload", payload);

            JSONObject resp = client.sendRequest(registerCallbackReq);
            if (!"success".equals(resp.getString("status"))) {
                throw new RuntimeException("Failed to register callback: " + resp);
            }
            String callbackId = resp.getJSONObject("data").getString("callback_id");
            System.out.println("Callback registered: " + callbackId);

            // 2. Load library
            String libPath = System.getProperty("user.dir") + "/build/test_lib/my_lib.dylib"; // Default for macOS
             String os = System.getProperty("os.name").toLowerCase();
            if (os.contains("win")) {
                 libPath = System.getProperty("user.dir") + "\\build\\test_lib\\Debug\\my_lib.dll";
            } else if (os.contains("linux")) {
                 libPath = System.getProperty("user.dir") + "/build/test_lib/my_lib.so";
            }
            
            JSONObject loadLibReq = new JSONObject();
            loadLibReq.put("command", "load_library");
            loadLibReq.put("payload", new JSONObject().put("path", libPath));
            resp = client.sendRequest(loadLibReq);
             if (!"success".equals(resp.getString("status"))) {
                throw new RuntimeException("Failed to load library: " + resp);
            }
            String libId = resp.getJSONObject("data").getString("library_id");
            System.out.println("Library loaded: " + libId);

            // 3. Call a function that triggers the callback
            // void call_my_callback(void (*cb)(const char*, int), const char* msg);
            JSONObject callFuncReq = new JSONObject();
            callFuncReq.put("command", "call_function");
            JSONObject callPayload = new JSONObject();
            callPayload.put("library_id", libId);
            callPayload.put("function_name", "call_my_callback"); 
            callPayload.put("return_type", "void");
            
            JSONArray funcArgs = new JSONArray();
            
            // Arg 1: The callback
            JSONObject arg1 = new JSONObject();
            arg1.put("type", "callback");
            arg1.put("value", callbackId);
            funcArgs.put(arg1);
            
            // Arg 2: The message string
            JSONObject arg2 = new JSONObject();
            arg2.put("type", "string");
            arg2.put("value", "Hello from Java Reproduction!");
            funcArgs.put(arg2);
            
            callPayload.put("args", funcArgs);
            callFuncReq.put("payload", callPayload);

            System.out.println("Calling function to trigger callback...");
            
            JSONObject callResp = client.sendRequest(callFuncReq);
            System.out.println("Function call returned. Response: " + callResp.toString());
            
            if (!"success".equals(callResp.getString("status"))) {
                 System.err.println("Function call failed: " + callResp.getString("error_message"));
                 System.exit(1);
            }
            
            JSONObject event = client.getEvent(2, TimeUnit.SECONDS);
            if (event != null) {
                System.out.println("SUCCESS: Received event: " + event);
            } else {
                System.err.println("FAILURE: Did not receive expected callback event.");
                System.exit(1);
            }

        } catch (Exception e) {
            e.printStackTrace();
            System.exit(1);
        }
    }
}
