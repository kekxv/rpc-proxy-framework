package com.example;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.net.UnixDomainSocketAddress;
import java.nio.channels.SocketChannel;
import java.net.StandardProtocolFamily;
import java.nio.channels.Channels;

public class App {

    // Adjust LIBRARY_PATH to be relative to the java_controller directory
    private static final String LIBRARY_PATH = Paths.get( "build", "test_lib",System.mapLibraryName("my_lib").replace("libmy_lib","my_lib")).toAbsolutePath().toString();

    public static void main(String[] args) {
        if (args.length < 1) {
            System.err.println("Usage: java -jar java-controller.jar <pipe_name>");
            return;
        }
        String pipeName = args[0];
        String socketPath = "/tmp/" + pipeName; // For Unix-like systems

        try {
            // For Unix Domain Sockets (Java 16+):
            Path socketFile = Paths.get(socketPath);
            UnixDomainSocketAddress address = UnixDomainSocketAddress.of(socketFile);
            SocketChannel channel = SocketChannel.open(StandardProtocolFamily.UNIX);
            channel.connect(address);
            DataOutputStream out = new DataOutputStream(Channels.newOutputStream(channel));
            DataInputStream in = new DataInputStream(Channels.newInputStream(channel));

            System.out.println("Connecting to " + socketPath + "...");
            System.out.println("Connected.");

            // 1. Register Struct "Point"
            JSONObject registerStructRequest = new JSONObject();
            registerStructRequest.put("command", "register_struct");
            registerStructRequest.put("request_id", "req-1");
            JSONObject pointDefinition = new JSONObject();
            pointDefinition.put("struct_name", "Point");
            JSONArray definitionArray = new JSONArray();
            definitionArray.put(new JSONObject().put("name", "x").put("type", "int32"));
            definitionArray.put(new JSONObject().put("name", "y").put("type", "int32"));
            pointDefinition.put("definition", definitionArray);
            registerStructRequest.put("payload", pointDefinition);
            System.out.println("Registering struct 'Point'");
            JSONObject registerStructResponse = sendAndReceive(out, in, registerStructRequest);
            System.out.println("Response: " + registerStructResponse);

            // 2. Load Library
            JSONObject loadLibraryRequest = new JSONObject();
            loadLibraryRequest.put("command", "load_library");
            loadLibraryRequest.put("request_id", "req-2");
            loadLibraryRequest.put("payload", new JSONObject().put("path", LIBRARY_PATH));
            System.out.println("Loading library: " + LIBRARY_PATH);
            JSONObject loadLibraryResponse = sendAndReceive(out, in, loadLibraryRequest);
            System.out.println("Response: " + loadLibraryResponse);
            String libraryId = loadLibraryResponse.getJSONObject("data").getString("library_id");
            System.out.println("Library loaded with ID: " + libraryId);

            // 3. Call function 'add'
            JSONObject addRequest = new JSONObject();
            addRequest.put("command", "call_function");
            addRequest.put("request_id", "req-3");
            JSONObject addPayload = new JSONObject();
            addPayload.put("library_id", libraryId);
            addPayload.put("function_name", "add");
            addPayload.put("return_type", "int32");
            JSONArray addArgs = new JSONArray();
            addArgs.put(new JSONObject().put("type", "int32").put("value", 10));
            addArgs.put(new JSONObject().put("type", "int32").put("value", 20));
            addPayload.put("args", addArgs);
            addRequest.put("payload", addPayload);
            System.out.println("Calling function 'add' with args (10, 20)");
            JSONObject addResponse = sendAndReceive(out, in, addRequest);
            System.out.println("Response: " + addResponse);
            System.out.println("Result of add(10, 20) is: " + addResponse.getJSONObject("data").getInt("value"));

            // 4. Call function 'greet'
            JSONObject greetRequest = new JSONObject();
            greetRequest.put("command", "call_function");
            greetRequest.put("request_id", "req-4");
            JSONObject greetPayload = new JSONObject();
            greetPayload.put("library_id", libraryId);
            greetPayload.put("function_name", "greet");
            greetPayload.put("return_type", "string");
            JSONArray greetArgs = new JSONArray();
            greetArgs.put(new JSONObject().put("type", "string").put("value", "Java World"));
            greetPayload.put("args", greetArgs);
            greetRequest.put("payload", greetPayload);
            System.out.println("Calling function 'greet' with arg ('Java World')");
            JSONObject greetResponse = sendAndReceive(out, in, greetRequest);
            System.out.println("Response: " + greetResponse);
            System.out.println("Result of greet('Java World') is: '" + greetResponse.getJSONObject("data").getString("value") + "'");

            // 5. Call function 'process_point_by_val'
            JSONObject processPointByValRequest = new JSONObject();
            processPointByValRequest.put("command", "call_function");
            processPointByValRequest.put("request_id", "req-5");
            JSONObject processPointByValPayload = new JSONObject();
            processPointByValPayload.put("library_id", libraryId);
            processPointByValPayload.put("function_name", "process_point_by_val");
            processPointByValPayload.put("return_type", "int32");
            JSONArray processPointByValArgs = new JSONArray();
            JSONObject pointVal = new JSONObject().put("x", 10).put("y", 20);
            processPointByValArgs.put(new JSONObject().put("type", "Point").put("value", pointVal));
            processPointByValPayload.put("args", processPointByValArgs);
            processPointByValRequest.put("payload", processPointByValPayload);
            System.out.println("Calling function 'process_point_by_val' with args (Point {x=10, y=20})");
            JSONObject processPointByValResponse = sendAndReceive(out, in, processPointByValRequest);
            System.out.println("Response: " + processPointByValResponse);
            System.out.println("Result of process_point_by_val is: " + processPointByValResponse.getJSONObject("data").getInt("value"));

            // 6. Call function 'process_point_by_ptr'
            JSONObject processPointByPtrRequest = new JSONObject();
            processPointByPtrRequest.put("command", "call_function");
            processPointByPtrRequest.put("request_id", "req-6");
            JSONObject processPointByPtrPayload = new JSONObject();
            processPointByPtrPayload.put("library_id", libraryId);
            processPointByPtrPayload.put("function_name", "process_point_by_ptr");
            processPointByPtrPayload.put("return_type", "int32");
            JSONArray processPointByPtrArgs = new JSONArray();
            JSONObject pointPtrVal = new JSONObject().put("x", 5).put("y", 6);
            JSONObject pointPtrArg = new JSONObject().put("type", "pointer").put("value", pointPtrVal).put("target_type", "Point");
            processPointByPtrArgs.put(pointPtrArg);
            processPointByPtrPayload.put("args", processPointByPtrArgs);
            processPointByPtrRequest.put("payload", processPointByPtrPayload);
            System.out.println("Calling function 'process_point_by_ptr' with args (Point {x=5, y=6})");
            JSONObject processPointByPtrResponse = sendAndReceive(out, in, processPointByPtrRequest);
            System.out.println("Response: " + processPointByPtrResponse);
            System.out.println("Result of process_point_by_ptr is: " + processPointByPtrResponse.getJSONObject("data").getInt("value"));

            // 7. Call function 'create_point'
            JSONObject createPointRequest = new JSONObject();
            createPointRequest.put("command", "call_function");
            createPointRequest.put("request_id", "req-7");
            JSONObject createPointPayload = new JSONObject();
            createPointPayload.put("library_id", libraryId);
            createPointPayload.put("function_name", "create_point");
            createPointPayload.put("return_type", "Point");
            JSONArray createPointArgs = new JSONArray();
            createPointArgs.put(new JSONObject().put("type", "int32").put("value", 100));
            createPointArgs.put(new JSONObject().put("type", "int32").put("value", 200));
            createPointPayload.put("args", createPointArgs);
            createPointRequest.put("payload", createPointPayload);
            System.out.println("Calling function 'create_point' with args (100, 200)");
            JSONObject createPointResponse = sendAndReceive(out, in, createPointRequest);
            System.out.println("Response: " + createPointResponse);
            System.out.println("Result of create_point is: " + createPointResponse.getJSONObject("data").getJSONObject("value"));


        } catch (IOException e) {
            System.err.println("Error communicating with executor: " + e.getMessage());
            e.printStackTrace();
        }
        System.out.println("Connection closed.");
    }

    private static JSONObject sendAndReceive(DataOutputStream out, DataInputStream in, JSONObject request) throws IOException {
        String requestStr = request.toString();
        byte[] requestBytes = requestStr.getBytes("UTF-8");

                    // Send length prefix
                    ByteBuffer lengthBuffer = ByteBuffer.allocate(4);
                    lengthBuffer.order(ByteOrder.BIG_ENDIAN); // Executor expects network byte order (big-endian)
                    lengthBuffer.putInt(requestBytes.length);
                    out.write(lengthBuffer.array());
                    out.write(requestBytes);
                    out.flush();
        
                    // Read length prefix
                    byte[] lenBytes = new byte[4];
                    in.readFully(lenBytes);
                    ByteBuffer receivedLengthBuffer = ByteBuffer.wrap(lenBytes);
                    receivedLengthBuffer.order(ByteOrder.BIG_ENDIAN); // Executor sends network byte order (big-endian)
                    int responseLength = receivedLengthBuffer.getInt();
        // Read response
        byte[] responseBytes = new byte[responseLength];
        in.readFully(responseBytes);
        String responseStr = new String(responseBytes, "UTF-8");
        return new JSONObject(responseStr);
    }
}
