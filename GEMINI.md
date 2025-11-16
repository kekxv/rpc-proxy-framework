使用中文回复！

---

# **`rpc-proxy-framework` 项目设计文档**

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
```json
{
  "command": "load_library | unload_library | call_function",
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

#### `call_function` 示例
```json
{
  "command": "call_function",
  "request_id": "req-002",
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
> **类型说明**: `type` 字段可以是 `void`, `int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float`, `double`, `string` (对应 `char*`), `pointer`。

### 响应 (Response)
```json
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

#### `call_function` 成功响应
```json
{
  "request_id": "req-002",
  "status": "success",
  "data": {
    "type": "int32",
    "value": 30
  }
}
```

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
├── CMakeLists.txt
├── src/
│   ├── main.cpp             # 程序入口, 解析命令行参数
│   ├── executor.h/.cpp      # Executor 核心逻辑类
│   ├── ipc_server.h/.cpp    # IPC 服务端实现 (跨平台抽象)
│   ├── lib_manager.h/.cpp   # 动态库管理器
│   └── ffi_dispatcher.h/.cpp  # FFI 调用分发器
├── platform/                  # (可选) 存放平台特定代码的头文件
│   ├── common.h
│   └── ...
├── third_party/               # 存放第三方库, 如 libffi 或 json
└── ...
```

## 10. 工作流程示例

1.  **启动 Executor**: 在一个终端中运行 `executor --pipe /tmp/my_executor_1`。程序将阻塞，等待连接。
2.  **Controller 连接**: 另一个进程（如Python脚本）打开并连接到 `/tmp/my_executor_1` 这个文件/管道。
3.  **发送请求**: Controller 写入一个 `load_library` 的JSON请求字符串（前面附加4字节长度）。
4.  **接收响应**: Executor 处理请求，加载库，并返回一个包含 `library_id` 的JSON响应。
5.  **调用函数**: Controller 使用返回的 `library_id` 构造一个 `call_function` 请求并发送。
6.  **获得结果**: Executor 使用`libffi`调用函数，并将结果封装在JSON中返回。
7.  **结束**: Controller 发送 `unload_library` 请求，或直接关闭管道连接。Executor 检测到断开后，可以设计为自动清理该连接加载的所有库，然后等待下一个连接。

## 11. 待办与未来改进 (TODO & Future Improvements)

*   **错误处理**: 完善错误处理机制，特别是捕获被调用函数中的段错误（Segmentation Fault）等致命异常，防止 `executor` 崩溃。
*   **安全性**: 当前模型下，`executor` 可以执行任何代码，存在巨大安全风险。应在受信任的环境中使用，或考虑使用容器/沙箱技术隔离`executor`进程。
*   **复杂类型支持**: 扩展协议以支持传递 C 结构体（struct）或回调函数（callback），这将显著增加复杂性。
*   **异步处理**: 当前设计为同步请求/响应，可升级为异步模型以提高吞吐量。

## 12. 使用说明 (Usage Instructions)

本节将指导您如何构建和运行 `executor` 程序，并使用提供的 `controller.py` 脚本与其交互。

### 12.1. 环境准备 (Prerequisites)

在开始之前，请确保您的系统已安装以下软件：

*   **C++ 编译器**: 支持 C++17 的编译器 (如 GCC, Clang, MSVC)。
*   **CMake**: 版本 3.15 或更高。
*   **libffi**: 动态函数调用库。
    *   **macOS**: `brew install libffi`
    *   **Debian/Ubuntu**: `sudo apt-get install libffi-dev`
    *   **CentOS/RHEL**: `sudo yum install libffi-devel`
*   **Python 3**: 用于运行 `controller.py` 脚本。

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
成功后，`my_lib.so` (或 `.dylib`, `.dll`) 文件会出现在项目的 `build` 目录下，以便 `executor` 和 `controller` 找到它。

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

**注意**: 在非Windows系统上，这会在 `build` 目录下创建一个名为 `my_pipe` 的socket文件。

#### 终端 2: 运行 Controller

在第二个终端中，保持在项目 **根目录**，然后运行 `controller.py` 脚本，并传入相同的管道名称。

```bash
# 确保当前在项目的根目录下
cd /path/to/rpc-proxy-framework

# 运行 Python 控制器
python3 controller.py my_pipe
```

`controller.py` 脚本将自动执行以下操作：
1.  连接到 `executor`。
2.  请求加载 `build/my_lib.so` 动态库。
3.  请求调用库中的 `add(10, 20)` 函数。
4.  请求调用库中的 `greet("World")` 函数。
5.  打印出每次操作的请求、响应和最终结果。

您应该能看到类似以下的输出：
```
Connecting to /path/to/rpc-proxy-framework/build/my_pipe...
Connected.

Loading library: /path/to/rpc-proxy-framework/build/my_lib.dylib
Response: {'data': {'library_id': 'lib-a1b2c3d4-...'}, 'request_id': 'req-1', 'status': 'success'}
Library loaded with ID: lib-a1b2c3d4-...

Calling function 'add' with args (10, 20)
Response: {'data': {'type': 'int32', 'value': 30}, 'request_id': 'req-2', 'status': 'success'}
Result of add(10, 20) is: 30

Calling function 'greet' with arg ('World')
Response: {'data': {'type': 'string', 'value': 'Hello, World'}, 'request_id': 'req-3', 'status': 'success'}
Result of greet('World') is: 'Hello, World'

Connection closed.
```

当 `controller.py` 运行结束后，`executor` 进程会继续运行，等待下一次连接。您可以按 `Ctrl+C` 来终止它。
