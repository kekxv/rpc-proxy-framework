package com.kekxv;

import com.kekxv.rpc.RpcClient;
import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

public class MultiClientApp {

    // --- 配置 ---
    private static final int NUM_CLIENTS = 5; // 要模拟的并发客户端数量

    // --- 用于彩色输出的ANSI转义码 ---
    private static final String RESET = "\033[0m";
    private static final String BOLD = "\033[1m";
    private static final String RED = "\033[91m";
    private static final String GREEN = "\033[92m";
    private static final String YELLOW = "\033[93m";
    private static final String BLUE = "\033[94m";
    private static final String MAGENTA = "\033[95m";
    private static final String CYAN = "\033[96m";


    /**
     * 同步的打印方法，确保多线程输出不会混乱。
     * @param message 要打印的消息
     */
    public static synchronized void safePrint(String message) {
        System.out.println(message);
    }

    /**
     * 模拟一个客户端的完整会话：连接，加载库，调用函数，然后断开。
     * @param clientId  客户端的唯一ID
     * @param pipeName  管道/套接字名称
     * @param libPath   要加载的动态库的绝对路径
     */
    public static void runClientSession(int clientId, String pipeName, String libPath) {
        safePrint(String.format("[Client %d] %sThread started.%s", clientId, YELLOW, RESET));
        
        String libraryId = null;
        // 使用 try-with-resources 确保 RpcClient 在结束时总是被关闭
        try (RpcClient client = new RpcClient(pipeName)) {
            client.connect();

            // 1. 加载库
            JSONObject loadReq = new JSONObject() 
                .put("command", "load_library") 
                .put("payload", new JSONObject().put("path", libPath));
            
            JSONObject loadRes = client.sendRequest(loadReq);
            if (!"success".equals(loadRes.optString("status"))) {
                throw new RuntimeException("Failed to load library. Response: " + loadRes);
            }
            libraryId = loadRes.getJSONObject("data").getString("library_id");
            safePrint(String.format("[Client %d] %sLibrary loaded with ID: %s...%s", clientId, GREEN, libraryId.substring(0, 8), RESET));

            // 2. 调用函数
            int a = clientId * 10;
            int b = 5;
            JSONObject callReq = new JSONObject() 
                .put("command", "call_function") 
                .put("payload", new JSONObject() 
                    .put("library_id", libraryId)
                    .put("function_name", "add")
                    .put("return_type", "int32")
                    .put("args", new JSONArray() 
                        .put(new JSONObject().put("type", "int32").put("value", a))
                        .put(new JSONObject().put("type", "int32").put("value", b))
                    )
                );

            JSONObject callRes = client.sendRequest(callReq);
            if (!"success".equals(callRes.optString("status"))) {
                throw new RuntimeException("Function call failed. Response: " + callRes);
            }

            // 3. 验证结果
            int result = callRes.getJSONObject("data").getJSONObject("return").getInt("value");
            int expected = a + b;
            if (result == expected) {
                safePrint(String.format("[Client %d] %s%sSUCCESS! add(%d, %d) => %d%s", clientId, BOLD, GREEN, a, b, result, RESET));
            } else {
                throw new IllegalStateException(String.format("Assertion failed: add(%d, %d) expected %d, but got %d", a, b, expected, result));
            }

        } catch (Throwable e) {
            safePrint(String.format("[Client %d] %s%sAn error occurred: %s%s", clientId, BOLD, RED, e.getMessage(), RESET));
        } finally {
            // RpcClient 的 close() 会在 try-with-resources 结束时自动调用，无需手动卸载库
            safePrint(String.format("[Client %d] %sSession finished.%s", clientId, CYAN, RESET));
        }
    }
    
    /**
     * 获取跨平台的测试库路径。
     * @return 动态库的绝对路径
     * @throws java.io.FileNotFoundException 如果找不到库文件
     */
    private static String getTestLibPath() throws java.io.FileNotFoundException {
        String libName = System.mapLibraryName("my_lib"); // 自动处理 .dll / .so / .dylib

        // 尝试多个可能的构建目录
        String[] possibleDirs = {
            "build/test_lib",
            "cmake-build-debug/test_lib",
            "../build/test_lib",
            "../cmake-build-debug/test_lib",
            "test_lib/build" // In-source build
        };
        
        for (String dir : possibleDirs) {
            File libFile = new File(dir, libName);
            if(libFile.exists()) {
                return libFile.getAbsolutePath();
            }
            // 有些系统（如macOS）可能会在 mapLibraryName 时添加 'lib' 前缀，但文件名本身没有
            File libFileWithoutPrefix = new File(dir, libName.replace("libmy_lib", "my_lib"));
             if(libFileWithoutPrefix.exists()) {
                return libFileWithoutPrefix.getAbsolutePath();
            }
        }
        
        throw new java.io.FileNotFoundException(String.format("Test library (my_lib) not found in common build directories. Searched for '%s'", libName));
    }


    /**
     * 主函数：启动多个客户端线程并发地与Executor通信。
     * @param args 命令行参数
     */
    public static void main(String[] args) {
        if (args.length < 1) {
            System.err.println(RED + "用法: java -jar <your-jar-file>.jar <pipe_name>" + RESET);
            return;
        }

        String pipeName = args[0];
        String libPath;

        try {
            libPath = getTestLibPath();
            safePrint(BOLD + "Found test library at: " + libPath + RESET);
        } catch (java.io.FileNotFoundException e) {
            safePrint(BOLD + RED + e.getMessage() + RESET);
            return;
        }

        safePrint(String.format("\n%s%s--- Starting %d Concurrent Clients ---%s", BOLD, MAGENTA, NUM_CLIENTS, RESET));
        safePrint(String.format("%s每个客户端将连接到管道 '%s', 加载库, 调用 'add' 函数, 然后断开。%s\n", YELLOW, pipeName, RESET));

        // 使用固定大小的线程池来管理客户端线程
        ExecutorService executorService = Executors.newFixedThreadPool(NUM_CLIENTS);

        for (int i = 0; i < NUM_CLIENTS; i++) {
            final int clientId = i;
            // 提交一个任务到线程池
            executorService.submit(() -> runClientSession(clientId, pipeName, libPath));
        }

        // 关闭线程池，不再接受新任务
        executorService.shutdown();
        try {
            // 等待所有任务完成，最多等待1分钟
            if (!executorService.awaitTermination(1, TimeUnit.MINUTES)) {
                System.err.println(RED + "并非所有客户端都在超时前完成！" + RESET);
                executorService.shutdownNow();
            }
        } catch (InterruptedException e) {
            System.err.println(RED + "线程池等待被打断！" + RESET);
            executorService.shutdownNow();
        }

        safePrint(String.format("\n%s%s--- All %d client sessions finished ---%s", BOLD, MAGENTA, NUM_CLIENTS, RESET));
    }
}
