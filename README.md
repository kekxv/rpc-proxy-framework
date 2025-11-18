# `rpc-proxy-framework` 项目设计文档

## 1. 项目概述 (Project Overview)

`rpc-proxy-framework` 是一个轻量级、跨平台的远程过程调用（RPC）代理框架。其核心是一个名为 `executor` 的独立可执行程序。`executor` 本身不包含任何业务逻辑，它作为一个通用的“代码执行宿主”，能够根据外部指令动态加载共享库（`.dll`, `.so`, `.dylib`），并调用其中的函数。

其他应用程序（称为 `controller`）通过平台原生的IPC管道（Windows上的命名管道，Linux/macOS上的Unix Domain Socket）向 `executor` 发送控制指令，实现了对原生代码的远程、动态调用。此框架旨在提供一个简单、高效且易于集成的原生代码执行环境。

## 2. 核心目标 (Core Goals)

*   **单一可执行文件**: `executor` 程序应被编译为单个可执行文件，不依赖外部的动态链接库（除系统库和libffi外）。
*   **跨平台支持**: 无缝支持 Windows、Linux 和 macOS。
*   **动态加载**: 能够根据RPC指令在运行时加载和卸载共享库。
*   **动态函数调用**: 使用 `libffi` 库，支持在运行时调用任意C语言签名的函数。
*   **IPC通信**: 使用平台原生的高效管道进行通信，避免网络套接字的复杂性和端口占用问题。
*   **多实例**: 支持同时启动和运行多个独立的 `executor` 实例。
*   **依赖最小化**: 除 C++ 标准库和 `libffi` 外，尽量减少第三方依赖。

## 3. 架构设计 (Architecture Design)

框架由两个主要部分组成：`Controller`（控制器）和 `Executor`（执行器）。

```text
+-----------------+      (IPC: Named Pipe / Unix Domain Socket)      +--------------------+      (libffi)      +----------------------+
|                 | <--------------------------------------------> |                    | <----------------> |                      |
| Controller      |        JSON-based Request/Response             | Executor Instance  |                    |  Dynamic Library     |
| (Any language)  |                                                | (Single Binary)    | --- dlopen/LoadLib-->| (.so, .dll, .dylib)  |
|                 |                                                |                    |                    |                      |
+-----------------+                                                +--------------------+                    |    + C Functions +   |
                                                                             |                                 |                      |
                                                                             | (Launched with unique pipe name)  +----------------------+
                                                                             |
                                                                     +--------------------+
                                                                     | Executor Instance  |
                                                                     | (Another process)  |
                                                                     +--------------------+
```

*   **Executor**: C++编写的后台进程。启动时会创建一个唯一的IPC管道端点并等待连接。它负责解析指令、管理动态库生命周期并通过`libffi`执行函数调用。
*   **Controller**: 任何能够读写管道的程序。它构建JSON格式的请求，发送给`Executor`，并接收`Executor`返回的JSON响应。

## 4. 技术选型 (Technology Stack)

*   **核心语言**: **C++17**。提供现代、高效且跨平台的编程能力。
*   **构建系统**: **CMake**。跨平台的标准构建工具，能很好地管理项目配置和依赖。
*   **动态调用**: **libffi**。这是实现动态函数调用的核心库。
*   **IPC 通信**:
    *   **Windows**: **命名管道 (Named Pipes)**。通过 `CreateNamedPipe`, `ConnectNamedPipe`, `ReadFile`, `WriteFile` 等Win32 API实现。
    *   **Linux/macOS**: **Unix Domain Sockets**。通过 `socket(AF_UNIX, ...)` 系列函数实现，行为类似文件IO。
*   **序列化协议**: **JSON**。简单、人类可读，且有轻量级的C++库（如 `nlohmann/json`，通常是单头文件库），符合依赖最小化的原则。

## 5. 核心组件详解 (`Executor`)

### 5.1. IPC通信模块 (IPC Communication Module)

*   **职责**: 建立和管理与 `Controller` 的IPC通信。
*   **实现**:
    1.  `Executor` 启动时，必须通过命令行参数接收一个唯一的管道名称，例如: `./executor --pipe /tmp/my_executor_1` 或 `executor.exe --pipe \\.\pipe\my_executor_1`。
    2.  根据操作系统创建并监听指定的管道/Socket。
    3.  进入一个循环：等待 `Controller` 连接 -> 读取一个完整的JSON请求 -> 交给协议解析器处理 -> 将处理结果（JSON响应）写回管道 -> 断开连接（或保持长连接，视设计而定，短连接更简单）。
    4.  需要一个简单的协议来处理数据分包问题，例如，在发送每条JSON消息前，先发送一个4字节的长度头（Header）。

### 5.2. 动态库管理器 (Dynamic Library Manager)

*   **职责**: 负责加载、卸载和查找动态库及其中的函数。
*   **实现**:
    *   内部维护一个 `std::map<std::string, LibraryHandle>`，其中 `key` 是用户指定的 `library_id`，`value` 是平台相关的库句柄 (`HMODULE` 或 `void*`)。
    *   **Load(path)**: 调用平台API (`LoadLibraryW` / `dlopen`) 加载库。成功后，生成一个唯一的 `library_id`（如UUID），存入map并返回给`Controller`。
    *   **Unload(library_id)**: 查找 `library_id` 对应的句柄，调用 `FreeLibrary` / `dlclose` 卸载，并从map中移除。
    *   **GetFunction(library_id, func_name)**: 查找库句柄，然后调用 `GetProcAddress` / `dlsym` 获取函数指针。

### 5.3. FFI调用分发器 (FFI Call Dispatcher)

*   **职责**: 解析 `call_function` 请求，使用 `libffi` 构造并执行函数调用。
*   **实现**:
    1.  接收函数指针、参数列表（类型和值）以及期望的返回类型。
    2.  **类型映射**: 将JSON中描述的类型（如 "int32", "double", "string"）映射到 `libffi` 的 `ffi_type` 结构（如 `ffi_type_sint32`, `ffi_type_double`, `ffi_type_pointer`）。
    3.  **参数准备**:
        *   为每个参数在内存中分配实际空间（例如，将JSON number `123` 存入一个 `int32_t` 变量）。
        *   创建一个 `void*` 数组，每个元素指向一个参数的内存地址。
    4.  **构建调用接口 (CIF)**: 调用 `ffi_prep_cif`，传入返回类型、参数数量和参数类型数组，初始化一个 `ffi_cif` 结构。
    5.  **执行调用**: 调用 `ffi_call`，传入 `ffi_cif`、函数指针、返回值缓冲区地址和参数值指针数组。
    6.  **结果封装**: 从返回值缓冲区中读取结果，并将其转换为JSON格式返回。

## 6. 通信协议设计 (JSON-RPC)

所有通信都基于简单的JSON请求/响应模式。

### 请求 (Request)
```json5
{
  "command": "load_library | unload_library | register_struct | call_function",
  "request_id": "unique_id_for_tracking",
  "payload": {
    // ... command-specific data
  }
}
```

#### `load_library` 示例
```json
{
  "command": "load_library",
  "request_id": "req-001",
  "payload": {
    "path": "/path/to/my_lib.so"
  }
}
```

#### `register_struct` 示例
```json
{
  "command": "register_struct",
  "request_id": "req-002",
  "payload": {
    "struct_name": "Point",
    "definition": [
      {"name": "x", "type": "int32"},
      {"name": "y", "type": "int32"}
    ]
  }
}
```
> **定义说明**: `definition` 是一个数组，每个元素描述结构体的一个成员，包含 `name` (成员名) 和 `type` (成员类型)。成员类型可以是基本类型或已注册的其他结构体类型。

#### `call_function` 示例
```json
{
  "command": "call_function",
  "request_id": "req-003",
  "payload": {
    "library_id": "lib-uuid-123",
    "function_name": "add",
    "return_type": "int32",
    "args": [
      { "type": "int32", "value": 10 },
      { "type": "int32", "value": 20 }
    ]
  }
}
```
> **类型说明**: `type` 字段可以是 `void`, `int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float`, `double`, `string` (对应 `char*`), `pointer`。**此外，`type` 字段也可以是任何已通过 `register_struct` 命令注册的结构体名称。**
>
> **结构体参数示例**: 
> ```json
> {
>   "type": "Point",
>   "value": {"x": 10, "y": 20}
> }
> ```
> **结构体指针参数示例**: 
> 当需要传递结构体指针时，`type` 字段应为 `"pointer"`，而 `value` 字段则包含一个描述实际结构体数据的对象，其中 `type` 字段指定了指针指向的结构体类型。
> ```json
> {
>   "type": "pointer",
>   "value": {
>     "type": "Point",
>     "value": {"x": 5, "y": 6}
>   }
> }
> ```

### 响应 (Response)
```json5
{
  "request_id": "same_as_request",
  "status": "success | error",
  "data": {
    // ... on success
  },
  "error_message": "..." // on error
}
```

#### `load_library` 成功响应
```json
{
  "request_id": "req-001",
  "status": "success",
  "data": {
    "library_id": "lib-uuid-123"
  }
}
```

#### `register_struct` 成功响应
```json
{
  "request_id": "req-002",
  "status": "success",
  "data": {}
}
```

#### `call_function` 成功响应
```json
{
  "request_id": "req-003",
  "status": "success",
  "data": {
    "type": "int32",
    "value": 30
  }
}
```
> **结构体返回值示例**: 
> ```json
> {
>   "request_id": "req-007",
>   "status": "success",
>   "data": {
>     "type": "Point",
>     "value": {"x": 100, "y": 200}
>   }
> }
> ```

## 7. 跨平台实现要点

建议创建小的抽象层来封装平台差异。

*   **`platform/ipc.h`**:
    *   提供 `IpcServer` 类。
    *   `#ifdef _WIN32` 内部实现使用 `CreateNamedPipe` 等。
    *   `#else` 内部实现使用 Unix Domain Sockets。
*   **`platform/dynlib.h`**:
    *   提供 `DynamicLibrary` 类。
    *   `#ifdef _WIN32` 内部使用 `HMODULE`, `LoadLibraryW`, `GetProcAddress`。
    *   `#else` 内部使用 `void*`, `dlopen`, `dlsym`。

## 8. 构建系统 (CMake)

一个简化的 `CMakeLists.txt` 结构：

```cmake
cmake_minimum_required(VERSION 3.15)
project(rpc-proxy-framework CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 查找 libffi ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFI REQUIRED libffi)

# --- 添加 JSON 库 (例如 nlohmann/json) ---
# 建议使用 FetchContent 或 Git Submodule
include(FetchContent)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.2
)
FetchContent_MakeAvailable(json)

# --- 定义可执行文件 ---
add_executable(executor
  src/main.cpp
  src/executor.cpp
  # ... 其他源文件
)

# --- 包含头文件路径 ---
target_include_directories(executor PUBLIC
  ${FFI_INCLUDE_DIRS}
  ${json_SOURCE_DIR}/include
)

# --- 链接库 ---
target_link_libraries(executor PRIVATE
  ${FFI_LIBRARIES}
)

# --- 平台特定的链接 ---
if(UNIX AND NOT APPLE)
  # Linux 需要链接 dl
  target_link_libraries(executor PRIVATE dl)
endif()
```

## 9. 项目文件结构 (Project File Structure)

```
rpc-proxy-framework/
├── .github/
│   └── workflows/
│       └── test_examples.yml  # GitHub Actions workflow for testing examples
├── CMakeLists.txt
├── src/
│   ├── main.cpp             # 程序入口, 解析命令行参数
│   ├── executor.h/.cpp      # Executor 核心逻辑类
│   ├── ipc_server.h/.cpp    # IPC 服务端实现 (跨平台抽象)
│   ├── lib_manager.h/.cpp   # 动态库管理器
│   ├── ffi_dispatcher.h/.cpp  # FFI 调用分发器
│   └── struct_manager.h/.cpp  # 结构体管理器
├── platform/                  # (可选) 存放平台特定代码的头文件
│   ├── common.h
│   └── ...
├── examples/                  # 示例控制器
│   ├── python_controller/
│   │   └── controller.py      # Python 示例控制器
│   ├── java_controller/
│   │   ├── pom.xml            # Java 示例 Maven 配置
│   │   └── src/
│   │       └── main/
│   │           └── java/
│   │               └── com/
│   │                   └── example/
│   │                       └── App.java # Java 示例控制器代码
│   └── cpp_controller/
│       ├── CMakeLists.txt     # C++ 示例 CMake 配置
│       └── cpp_controller_example.cpp # C++ 示例控制器代码
├── third_party/               # 存放第三方库, 如 libffi 或 json
└── ...
```

## 10. 工作流程示例

1.  **启动 Executor**: 在一个终端中运行 `executor --pipe /tmp/my_executor_1`。程序将阻塞，等待连接。
2.  **Controller 连接**: 另一个进程（如Python脚本）打开并连接到 `/tmp/my_executor_1` 这个文件/管道。
3.  **注册结构体**: Controller 写入一个 `register_struct` 的JSON请求字符串。
4.  **发送请求**: Controller 写入一个 `load_library` 的JSON请求字符串（前面附加4字节长度）。
5.  **接收响应**: Executor 处理请求，加载库，并返回一个包含 `library_id` 的JSON响应。
6.  **调用函数**: Controller 使用返回的 `library_id` 构造一个 `call_function` 请求并发送，其中可以包含结构体参数。
7.  **获得结果**: Executor 使用`libffi`调用函数，并将结果（可能包含结构体）封装在JSON中返回。
8.  **结束**: Controller 发送 `unload_library` 请求，或直接关闭管道连接。Executor 检测到断开后，可以设计为自动清理该连接加载的所有库，然后等待下一个连接。

## 11. 待办与未来改进 (TODO & Future Improvements)

*   **错误处理**: 完善错误处理机制，特别是捕获被调用函数中的段错误（Segmentation Fault）等致命异常，防止 `executor` 崩溃。
*   **安全性**: 当前模型下，`executor` 可以执行任何代码，存在巨大安全风险。应在受信任的环境中使用，或考虑使用容器/沙箱技术隔离`executor`进程。
*   **回调函数支持**: 当前设计为同步请求/响应，回调函数需要异步、双向通信，这将是架构上的重大改变。
*   **复杂类型支持**: 扩展协议以支持传递 C 结构体（struct）或回调函数（callback），这将显著增加复杂性。
*   **异步处理**: 当前设计为同步请求/响应，可升级为异步模型以提高吞吐量。

## 12. 使用说明 (Usage Instructions)

本节将指导您如何构建和运行 `executor` 程序，并使用不同语言的 `controller` 脚本与其交互。

### 12.1. 环境准备 (Prerequisites)

在开始之前，请确保您的系统已安装以下软件：

*   **C++ 编译器**: 支持 C++17 的编译器 (如 GCC, Clang, MSVC)。
*   **CMake**: 版本 3.15 或更高。
*   **libffi**: 动态函数调用库。
    *   **macOS**: `brew install libffi`
    *   **Debian/Ubuntu**: `sudo apt-get install libffi-dev`
    *   **CentOS/RHEL**: `sudo yum install libffi-devel`
*   **Python 3**: 用于运行 `controller.py` 脚本。
*   **Java Development Kit (JDK)**: 版本 8 或更高。
*   **Maven 或 Gradle**: 用于构建 Java 示例。

### 12.2. 构建项目 (Building the Project)

整个项目包含主程序 `executor` 和一个用于测试的动态库 `my_lib`。

#### 1. 构建 Executor

首先，构建核心的 `executor` 程序。

```bash
# 在项目根目录
mkdir build
cd build
cmake ..
make
```
成功后，您应该能在 `build` 目录下找到 `executor` 可执行文件。

#### 2. 构建测试动态库

为了演示 `executor` 的功能，我们提供了一个简单的动态库 `my_lib`。

```bash
# 在项目根目录
cd test_lib
cmake .
make
```
成功后，`my_lib.so` (或 `.dylib`, `.dll`) 文件会出现在 `test_lib/build` 目录下。

### 12.3. 运行与交互 (Running and Interacting)

请打开 **两个** 终端窗口来执行以下步骤。

#### 终端 1: 启动 Executor

在第一个终端中，进入 `build` 目录并启动 `executor`。它将创建一个名为 `my_pipe` 的IPC通道并等待连接。

```bash
# 确保当前在项目的 build 目录下
cd /path/to/rpc-proxy-framework/build

# 启动 executor 并指定管道名称
./executor --pipe my_pipe
```
您将看到类似以下的输出，表示 `executor` 正在监听：
`Executor listening on pipe: my_pipe`

**注意**: 在非Windows系统上，这会在 `/tmp/` 目录下创建一个名为 `my_pipe` 的socket文件。

#### 终端 2: 运行 Controller (Python)

在第二个终端中，进入 `examples/python_controller` 目录，然后运行 `controller.py` 脚本，并传入相同的管道名称。

```bash
# 确保当前在项目的根目录下
cd examples/python_controller

# 运行 Python 控制器
python3 controller.py my_pipe
```

`controller.py` 脚本将自动执行以下操作：
1.  连接到 `executor`。
2.  **注册 `Point` 结构体**。
3.  请求加载 `test_lib/build/my_lib.so` 动态库。
4.  请求调用库中的 `add(10, 20)` 函数。
5.  请求调用库中的 `greet("World")` 函数。
6.  **请求调用库中的 `process_point_by_val(Point {x=10, y=20})` 函数。**
7.  **请求调用库中的 `process_point_by_ptr(Point {x=5, y=6})` 函数。**
8.  **请求调用库中的 `create_point(100, 200)` 函数，并接收返回的 `Point` 结构体。**
9.  打印出每次操作的请求、响应和最终结果。

您应该能看到类似以下的输出：
```
Connecting to /tmp/my_pipe...
Connected.

Registering struct 'Point'
Response: {'request_id': 'req-1', 'status': 'success'}

Loading library: /path/to/rpc-proxy-framework/test_lib/build/my_lib.dylib
Response: {'data': {'library_id': 'lib-a1b2c3d4-...'}, 'request_id': 'req-2', 'status': 'success'}
Library loaded with ID: lib-a1b2c3d4-...

Calling function 'add' with args (10, 20)
Response: {'data': {'type': 'int32', 'value': 30}, 'request_id': 'req-3', 'status': 'success'}
Result of add(10, 20) is: 30

Calling function 'greet' with arg ("World")
Response: {'data': {'type': 'string', 'value': 'Hello, World'}, 'request_id': 'req-4', 'status': 'success'}
Result of greet('World') is: 'Hello, World'

Calling function 'process_point_by_val' with args (Point {x=10, y=20})
Response: {'data': {'type': 'int32', 'value': 30}, 'request_id': 'req-5', 'status': 'success'}
Result of process_point_by_val is: 30

Calling function 'process_point_by_ptr' with args (Point {x=5, y=6})
Response: {'data': {'type': 'int32', 'value': 30}, 'request_id': 'req-6', 'status': 'success'}
Result of process_point_by_ptr is: 30

Calling function 'create_point' with args (100, 200)
Response: {'data': {'type': 'Point', 'value': {'x': 100, 'y': 200}}, 'request_id': 'req-7', 'status': 'success'}
Result of create_point is: {'x': 100, 'y': 200}

Connection closed.
```

#### 终端 2: 运行 Controller (Java)

为了运行 Java 示例，您需要一个 Java 开发环境（JDK 8+）和一个构建工具（如 Maven）。

1.  **创建 Maven 项目**:
    ```bash
    # 在项目根目录
    mkdir examples/java_controller
    cd examples/java_controller
    # 创建 pom.xml 和 src 目录结构
    # (这里省略了 mvn archetype:generate 命令，因为我们手动创建了文件)
    ```

2.  **添加依赖**:
    编辑 `examples/java_controller/pom.xml`，在 `<dependencies>` 标签中添加 `org.json` 依赖。
    ```xml
    <dependency>
        <groupId>org.json</groupId>
        <artifactId>json</artifactId>
        <version>20231013</version>
    </dependency>
    ```

3.  **编写 Java 代码**:
    在 `examples/java_controller/src/main/java/com/example/App.java` 中写入以下内容。
    ```java
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
        private static final String LIBRARY_PATH = Paths.get("..", "..", "test_lib", "build", System.mapLibraryName("my_lib")).toAbsolutePath().toString();

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
                JSONObject pointPtrArg = new JSONObject().put("type", "pointer").put("value", new JSONObject().put("type", "Point").put("value", pointPtrVal));
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
    ```

4.  **构建并运行**:
    ```bash
    # 在 examples/java_controller 目录下
    mvn clean compile assembly:single
    java -jar target/java-controller-1.0-SNAPSHOT-jar-with-dependencies.jar my_pipe
    ```
    **注意**: 上述 Java 示例中的 `new Socket("localhost", 12345)` 是一个占位符。在实际使用中，您需要根据操作系统和 `executor` 的 IPC 实现来调整连接方式。
    *   **对于 Unix-like 系统 (Linux/macOS)**，`executor` 使用 Unix Domain Sockets。Java 16+ 可以使用 `java.net.UnixDomainSocketAddress` 和 `java.nio.channels.SocketChannel` 进行连接。
        ```java
        // 替换 Socket 连接部分
        Path socketFile = Paths.get(socketPath);
        UnixDomainSocketAddress address = UnixDomainSocketAddress.of(socketFile);
        SocketChannel channel = SocketChannel.open(StandardProtocolFamily.UNIX);
        channel.connect(address);
        DataOutputStream out = new DataOutputStream(Channels.newOutputStream(channel));
        DataInputStream in = new DataInputStream(Channels.newInputStream(channel));
        ```
    *   **对于 Windows**，`executor` 使用命名管道。Java 标准库不直接支持命名管道，您可能需要使用 JNA (Java Native Access) 等库来调用 Win32 API。

#### 终端 2: 运行 Controller (C++)

为了运行 C++ 示例，您需要一个 C++ 编译器和 CMake。

1.  **创建 C++ 源文件**:
    在 `examples/cpp_controller/cpp_controller_example.cpp` 中写入以下内容。

    ```cpp
    #include <iostream>
    #include <string>
    #include <vector>
    #include <chrono>
    #include <thread>
    #include <filesystem>

    #include "nlohmann/json.hpp" // Make sure nlohmann/json is available

    #ifdef _WIN32
    #include <windows.h>
    #include <namedpipeapi.h>
    #else
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <unistd.h>
    #endif

    namespace fs = std::filesystem;
    using json = nlohmann::json;

    // Helper function to send JSON request with length prefix
    json send_request(
    #ifdef _WIN32
        HANDLE pipe,
    #else
        int sock,
    #endif
        const json& request) {
        std::string request_str = request.dump();
        uint32_t length = request_str.length();

        // Send length prefix (little-endian)
        char len_bytes[4];
        len_bytes[0] = (length >> 0) & 0xFF;
        len_bytes[1] = (length >> 8) & 0xFF;
        len_bytes[2] = (length >> 16) & 0xFF;
        len_bytes[3] = (length >> 24) & 0xFF;

    #ifdef _WIN32
        DWORD bytes_written;
        WriteFile(pipe, len_bytes, 4, &bytes_written, NULL);
        WriteFile(pipe, request_str.c_str(), length, &bytes_written, NULL);
        FlushFileBuffers(pipe);
    #else
        write(sock, len_bytes, 4);
        write(sock, request_str.c_str(), length);
    #endif

        // Read length prefix of response
        char response_len_bytes[4];
    #ifdef _WIN32
        DWORD bytes_read;
        ReadFile(pipe, response_len_bytes, 4, &bytes_read, NULL);
    #else
        read(sock, response_len_bytes, 4);
    #endif
        uint32_t response_length = (uint8_t)response_len_bytes[0] |
                                   ((uint8_t)response_len_bytes[1] << 8) |
                                   ((uint8_t)response_len_bytes[2] << 16) |
                                   ((uint8_t)response_len_bytes[3] << 24);

        // Read response
        std::vector<char> response_buffer(response_length);
    #ifdef _WIN32
        ReadFile(pipe, response_buffer.data(), response_length, &bytes_read, NULL);
    #else
        read(sock, response_buffer.data(), response_length);
    #endif
        std::string response_str(response_buffer.begin(), response_buffer.end());
        return json::parse(response_str);
    }

    int main(int argc, char* argv[]) {
        if (argc < 2) {
            std::cerr << "Usage: " << argv[0] << " <pipe_name>" << std::endl;
            return 1;
        }
        std::string pipe_name = argv[1];

    #ifdef _WIN32
        std::string pipe_path = "\\\\.\\pipe\\" + pipe_name;
        HANDLE pipe = CreateFileA(pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::cerr << "Failed to connect to named pipe: " << GetLastError() << std::endl;
            return 1;
        }
        std::cout << "Connected to named pipe: " << pipe_path << std::endl;
    #else
        std::string socket_path = "/tmp/" + pipe_name;
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock == -1) {
            std::cerr << "Failed to create socket" << std::endl;
            return 1;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            std::cerr << "Failed to connect to Unix domain socket: " << socket_path << std::endl;
            close(sock);
            return 1;
        }
        std::cout << "Connected to Unix domain socket: " << socket_path << std::endl;
    #endif

        fs::path current_path = fs::current_path();
        fs::path lib_path = current_path / "test_lib" / "build";
    #ifdef _WIN32
        lib_path /= "my_lib.dll";
    #elif __APPLE__
        lib_path /= "libmy_lib.dylib";
    #else
        lib_path /= "libmy_lib.so";
    #endif
        std::string library_full_path = lib_path.string();

        try {
            // 1. Register Struct "Point"
            json register_struct_request = {
                {"command", "register_struct"},
                {"request_id", "req-1"},
                {"payload", {
                    {"struct_name", "Point"},
                    {"definition", {
                        {{"name", "x"}, {"type", "int32"}},
                        {{"name", "y"}, {"type", "int32"}}
                    }}
                }}
            };
            std::cout << "Registering struct 'Point'" << std::endl;
            json register_struct_response = send_request(
            #ifdef _WIN32
                pipe,
            #else
                sock,
            #endif
                register_struct_request);
            std::cout << "Response: " << register_struct_response.dump() << std::endl;

            // 2. Load Library
            json load_library_request = {
                {"command", "load_library"},
                {"request_id", "req-2"},
                {"payload", {{"path", library_full_path}}}
            };
            std::cout << "Loading library: " << library_full_path << std::endl;
            json load_library_response = send_request(
            #ifdef _WIN32
                pipe,
            #else
                sock,
            #endif
                load_library_request);
            std::cout << "Response: " << load_library_response.dump() << std::endl;
            std::string library_id = load_library_response["data"]["library_id"].get<std::string>();
            std::cout << "Library loaded with ID: " << library_id << std::endl;

            // 3. Call function 'add'
            json add_request = {
                {"command", "call_function"},
                {"request_id", "req-3"},
                {"payload", {
                    {"library_id", library_id},
                    {"function_name", "add"},
                    {"return_type", "int32"},
                    {"args", {
                        {{"type", "int32"}, {"value", 10}},
                        {{"type", "int32"}, {"value", 20}}
                    }}
                }}
            };
            std::cout << "Calling function 'add' with args (10, 20)" << std::endl;
            json add_response = send_request(
            #ifdef _WIN32
                pipe,
            #else
                sock,
            #endif
                add_request);
            std::cout << "Response: " << add_response.dump() << std::endl;
            std::cout << "Result of add(10, 20) is: " << add_response["data"]["value"].get<int>() << std::endl;

            // 4. Call function 'greet'
            json greet_request = {
                {"command", "call_function"},
                {"request_id", "req-4"},
                {"payload", {
                    {"library_id", library_id},
                    {"function_name", "greet"},
                    {"return_type", "string"},
                    {"args", {
                        {{"type", "string"}, {"value", "C++ World"}}
                    }}
                }}
            };
            std::cout << "Calling function 'greet' with arg ('C++ World')" << std::endl;
            json greet_response = send_request(
            #ifdef _WIN32
                pipe,
            #else
                sock,
            #endif
                greet_request);
            std::cout << "Response: " << greet_response.dump() << std::endl;
            std::cout << "Result of greet('C++ World') is: '" << greet_response["data"]["value"].get<std::string>() << "'" << std::endl;

            // 5. Call function 'process_point_by_val'
            json process_point_by_val_request = {
                {"command", "call_function"},
                {"request_id", "req-5"},
                {"payload", {
                    {"library_id", library_id},
                    {"function_name", "process_point_by_val"},
                    {"return_type", "int32"},
                    {"args", {
                        {{"type", "Point"}, {"value", {{"x", 10}, {"y", 20}}}}
                    }}
                }}
            };
            std::cout << "Calling function 'process_point_by_val' with args (Point {x=10, y=20})" << std::endl;
            json process_point_by_val_response = send_request(
            #ifdef _WIN32
                pipe,
            #else
                sock,
            #endif
                process_point_by_val_request);
            std::cout << "Response: " << process_point_by_val_response.dump() << std::endl;
            std::cout << "Result of process_point_by_val is: " << process_point_by_val_response["data"]["value"].get<int>() << std::endl;

            // 6. Call function 'process_point_by_ptr'
            json process_point_by_ptr_request = {
                {"command", "call_function"},
                {"request_id", "req-6"},
                {"payload", {
                    {"library_id", library_id},
                    {"function_name", "process_point_by_ptr"},
                    {"return_type", "int32"},
                    {"args", {
                        {{"type", "pointer"}, {"value", {{"type", "Point"}, {"value", {{"x", 5}, {"y", 6}}}}}}
                    }}
                }}
            };
            std::cout << "Calling function 'process_point_by_ptr' with args (Point {x=5, y=6})" << std::endl;
            json process_point_by_ptr_response = send_request(
            #ifdef _WIN32
                pipe,
            #else
                sock,
            #endif
                process_point_by_ptr_request);
            std::cout << "Response: " << process_point_by_ptr_response.dump() << std::endl;
            std::cout << "Result of process_point_by_ptr is: " << process_point_by_ptr_response["data"]["value"].get<int>() << std::endl;

            // 7. Call function 'create_point'
            json create_point_request = {
                {"command", "call_function"},
                {"request_id", "req-7"},
                {"payload", {
                    {"library_id", library_id},
                    {"function_name", "create_point"},
                    {"return_type", "Point"},
                    {"args", {
                        {{"type", "int32"}, {"value", 100}},
                        {{"type", "int32"}, {"value", 200}}
                    }}
                }}
            };
            std::cout << "Calling function 'create_point' with args (100, 200)" << std::endl;
            json create_point_response = send_request(
            #ifdef _WIN32
                pipe,
            #else
                sock,
            #endif
                create_point_request);
            std::cout << "Response: " << create_point_response.dump() << std::endl;
            std::cout << "Result of create_point is: " << create_point_response["data"]["value"].dump() << std::endl;

        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }

    #ifdef _WIN32
        CloseHandle(pipe);
    #else
        close(sock);
    #endif
        std::cout << "Connection closed." << std::endl;

        return 0;
    }
    ```

2.  **创建 `CMakeLists.txt`**:
    在 `examples/cpp_controller/CMakeLists.txt` 中写入以下内容。

    ```cmake
    cmake_minimum_required(VERSION 3.15)
    project(cpp_controller_example CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    # 查找 nlohmann/json (假设已通过 FetchContent 或其他方式引入到主项目中)
    # 如果没有，您可能需要手动下载或使用 FetchContent
    find_package(nlohmann_json CONFIG REQUIRED)

    add_executable(cpp_controller_example cpp_controller_example.cpp)

    target_link_libraries(cpp_controller_example PRIVATE nlohmann_json::nlohmann_json)

    # 平台特定的链接
    if(UNIX AND NOT APPLE)
      # Linux 需要链接 dl
      target_link_libraries(cpp_controller_example PRIVATE dl)
    endif()
    ```

3.  **构建并运行**:
    ```bash
    # 在项目根目录
    mkdir examples/cpp_controller/build
    cd examples/cpp_controller/build
    cmake ../..
    make
    ./cpp_controller_example my_pipe
    ```
    **注意**: 上述 `CMakeLists.txt` 假设 `nlohmann/json` 已经以某种方式（例如通过主项目的 `FetchContent`）在您的构建环境中可用。如果不是，您可能需要调整 `find_package` 或手动添加 `nlohmann/json` 的头文件路径。

当 `controller` 运行结束后，`executor` 进程会继续运行，等待下一次连接。您可以按 `Ctrl+C` 来终止它。

当 `controller` 运行结束后，`executor` 进程会继续运行，等待下一次连接。您可以按 `Ctrl+C` 来终止它。

