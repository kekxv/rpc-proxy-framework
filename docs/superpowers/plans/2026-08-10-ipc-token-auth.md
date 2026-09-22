# IPC Token 认证加固 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 executor 的 IPC 端点（Windows 命名管道 + Linux/Unix Socket）增加 token 认证握手：**配置了 token 时强制认证**（未认证/认证失败的连接一律关闭，fail-closed）；**未配置 token 时保留传统模式**（照常启动并接受连接，启动时打印警告）。Windows 与 Linux 使用完全相同的协议。

**Architecture:** 客户端连接后发送的第一帧必须是 `auth` 请求，executor 在会话线程入口处校验 token（常量时间比较），成功后才进入现有的命令分发循环；失败则回错误响应并立即断开。**兼容模式**：`auth_token_` 为空时跳过强制认证——首帧是 `auth` 则按通过处理（消费该帧），首帧是普通命令则直接进入命令处理（传统行为），并在启动时打印警告。Token 通过 `--token` / `--token-file` / 环境变量 `RPC_PROXY_TOKEN` 提供给 executor，客户端通过构造参数或同一环境变量获取；客户端始终发送 auth 帧（token 可为空串，由服务端决定是否放行）。`auth` 命令不进入 `COMMAND_DISPATCHER`，在 `handle_client_session` 中显式处理。

**Tech Stack:** C++17（executor 核心）、jsoncpp（JSON）、libffi（FFI）、Python 3（示例控制器）、Java 8+ / JNA（示例控制器）、CMake / ctest、GitHub Actions。

## Global Constraints

- **双平台统一协议**：Windows 命名管道与 Unix Socket 的认证协议完全相同：首帧 `{"command":"auth","request_id":<str>,"payload":{"token":<str>}}`，响应 `{"request_id":<echo>,"status":"success"}` 或 `{"request_id":<echo>,"status":"error","error_message":"Authentication failed"}`。
- **强制认证（配置了 token 时）**：连接首帧不是 `auth`、token 缺失/错误、首帧 JSON 无法解析 → 回错误响应并关闭连接（fail-closed）。
- **传统兼容模式（未配置 token 时）**：executor 照常启动并在 stderr 打印警告；不强制认证，首帧按普通命令处理；若首帧恰为 `auth` 则按通过处理（空 token 无从校验，放行）。
- **客户端始终发送 auth 帧**：token 为空时也发送（`payload.token` 为空串），由服务端根据自身配置决定放行/拒绝；客户端不得在 token 为空时跳过认证或报错退出。
- **响应必须回显 `request_id`**：所有客户端（Python/Java/C++）的请求-响应关联都依赖 `request_id`，auth 的成功/失败响应都必须回显客户端传入的 `request_id`，否则客户端会等待超时。
- **Token 比较必须常量时间**：禁止 `==`/`memcmp` 直接比较，用 XOR 累计比较，避免时序侧信道。
- **认证不进入 COMMAND_DISPATCHER**：`auth` 只在 `handle_client_session` 中处理（使用文件内静态辅助函数 `try_parse_auth_frame`）。
- **帧格式不变**：4 字节大端长度头 + JSON 正文；`kMaxIpcFrameSize = 64 MiB` 不变。
- **现有命令集不变**：`load_library`、`unload_library`、`register_struct`、`unregister_struct`、`register_callback`、`unregister_callback`、`call_function`、`register_cleanup`、`cancel_cleanup` 语义不变。
- **会话隔离设计不变**：每个连接独立 LibManager/StructManager/CallbackManager/cleanup 表。
- **`executor_test.cpp` 不需要改动**：它直接调用 `ffi_dispatcher`，不走 socket，与认证无关。
- 构建要求不变：C++17、CMake ≥ 3.15、MSVC（Win7 SP1 兼容，`WINVER=0x0601`）/ GCC / Clang。

---

### Task 1: Executor 核心 —— token 认证握手

**Files:**
- Modify: `src/executor.h` — `run()` 签名加 token 参数、新增 `auth_token_` 成员
- Modify: `src/executor.cpp` — `secure_equals` 常量时间比较、`try_parse_auth_frame` 静态辅助函数、`run()` 存储 token、`handle_client_session` 入口首帧处理（强制认证 / 传统兼容）
- Modify: `test/multi_client_test.cpp` — fixture 传 token、`SimplePipeClient` 增加 `authenticate()`、既有调用点补认证、新增两个负向测试

**Interfaces:**
- Consumes: 现有 `ClientConnection::read()/write()/close()/isOpen()`（不变）
- Produces:
  - `void Executor::run(const std::string& pipe_name, const std::string& auth_token)` —— 替代旧签名 `run(pipe_name)`；`auth_token` 为空 ⇒ 传统兼容模式（不强制认证）
  - 文件内静态函数 `static bool try_parse_auth_frame(const std::string& frame, std::string& request_id, std::string& token)` —— 解析首帧；是合法 JSON 且 `command == "auth"` 时返回 true，并输出 `request_id` 与 `payload.token`；否则返回 false
  - `test/multi_client_test.cpp` 中：`static const std::string kTestToken = "integration-test-token-9f3a7c";`
  - `test/multi_client_test.cpp` 中：`bool SimplePipeClient::authenticate()` —— 发送 auth 帧并返回 `status == "success"`

- [ ] **Step 1: 先加 token 参数与测试侧改动（不改认证逻辑，保持全绿）**

修改 `src/executor.h`：

```cpp
class Executor
{
public:
  Executor();
  ~Executor();

  void run(const std::string& pipe_name, const std::string& auth_token);
  void stop();

private:
  std::unique_ptr<IpcServer> server;
  std::atomic<bool> is_running_{false};
  std::string auth_token_;
  std::mutex sessions_mutex_;
  std::set<ClientConnection*> active_connections_;
  std::vector<std::thread> session_threads_;

  void handle_client_session(std::unique_ptr<ClientConnection> connection);
  void close_active_connections();
  void join_session_threads();
};
```

修改 `src/executor.cpp` 的 `run()`（此步只存 token，不强制校验）：

```cpp
void Executor::run(const std::string& pipe_name, const std::string& auth_token)
{
  auth_token_ = auth_token;
  is_running_ = true;
  server->listen(pipe_name);
  // ... 以下循环体保持不变
}
```

修改 `test/multi_client_test.cpp`：
1. 文件顶部（`static std::string g_pipe_name;` 附近）加：

```cpp
// Shared test token used by all integration tests.
static const std::string kTestToken = "integration-test-token-9f3a7c";
```

2. `SimplePipeClient` 构造函数改为 `SimplePipeClient(int client_id, const std::string& token = kTestToken)`，新增成员 `std::string token_;`，并在构造函数初始化列表里加 `token_(token)`。
3. `SimplePipeClient` 新增方法：

```cpp
  bool authenticate()
  {
    json auth_req;
    auth_req["command"] = "auth";
    auth_req["request_id"] = "req-auth-" + std::to_string(client_id_);
    auth_req["payload"]["token"] = token_;
    if (!send_request(json_dump(auth_req))) return false;
    json resp = json_parse(receive_response());
    return resp["status"].asString() == "success";
  }
```

4. Fixture `SetUp` 中 `executor_->run(g_pipe_name);` 改为 `executor_->run(g_pipe_name, kTestToken);`。
5. 各测试在 `client.connect(g_pipe_name)` 成功后补认证（`run_client_session` 内改成失败即返回 false）：

```cpp
  // run_client_session: 在 connect 成功检查之后
  if (!client.authenticate())
  {
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cerr << "[Client " << client_id << "] Authentication failed." << std::endl;
    return false;
  }
```

```cpp
  // TransfersFiveMiBBufferAndKeepsFramingAligned
  ASSERT_TRUE(client.connect(g_pipe_name));
  ASSERT_TRUE(client.authenticate());
```

```cpp
  // HandlesFragmentedHeaderAndBody
  ASSERT_TRUE(client.connect(g_pipe_name));
  ASSERT_TRUE(client.authenticate());
```

```cpp
  // RejectsOversizedFrameWithoutStoppingServer —— invalid_client 不需要认证
  //（超大帧在 read() 层就被拒绝）；healthy_client 需要：
  ASSERT_TRUE(healthy_client.connect(g_pipe_name));
  ASSERT_TRUE(healthy_client.authenticate());
```

```cpp
  // TransfersFiveMiBCallbackEvent
  ASSERT_TRUE(client.connect(g_pipe_name));
  ASSERT_TRUE(client.authenticate());
```

```cpp
  // ClientDisconnectDuringLargeResponseDoesNotStopServer
  ASSERT_TRUE(client.connect(g_pipe_name));
  ASSERT_TRUE(client.authenticate());
```

> `StopUnblocksIdleClientSession` 故意不认证（连接后立即 stop，验证 stop() 能解除阻塞在首帧读取上的会话），保持原样。

- [ ] **Step 2: 新增两个负向测试（此时应 FAIL —— 服务器尚未强制认证）**

在 `test/multi_client_test.cpp` 末尾（`ClientDisconnectDuringLargeResponseDoesNotStopServer` 测试之后）追加：

```cpp
TEST_F(MultiClientIntegrationTest, RejectsClientWithoutAuthHandshake)
{
  SimplePipeClient client(200);
  ASSERT_TRUE(client.connect(g_pipe_name));
  json request;
  request["command"] = "load_library";
  request["request_id"] = "no-auth";
  request["payload"]["path"] = get_test_library_path();
  ASSERT_TRUE(client.send_request(json_dump(request)));
  json response = json_parse(client.receive_response());
  EXPECT_EQ(response["status"].asString(), "error");
  // 服务器必须在握手失败后关闭连接
  EXPECT_TRUE(client.receive_response().empty());
}

TEST_F(MultiClientIntegrationTest, RejectsClientWithWrongToken)
{
  SimplePipeClient client(201, "wrong-token");
  ASSERT_TRUE(client.connect(g_pipe_name));
  EXPECT_FALSE(client.authenticate());
  // 服务器必须在认证失败后关闭连接
  EXPECT_TRUE(client.receive_response().empty());
}
```

- [ ] **Step 3: 运行测试，确认新测试失败、旧测试仍通过**

Run: `cmake --build build --config Release && ctest --test-dir build --output-on-failure --timeout 55`
Expected: `RejectsClientWithoutAuthHandshake` FAIL（服务器目前接受无认证请求并返回 success/正常响应）、`RejectsClientWithWrongToken` FAIL（`authenticate()` 意外返回 true）；其余既有测试 PASS。

- [ ] **Step 4: 实现认证强制逻辑**

在 `src/executor.cpp` 顶部（`using json = Json::Value;` 之后）加常量时间比较与首帧解析辅助函数：

```cpp
// 常量时间字符串比较，避免时序侧信道泄露 token 内容。
static bool secure_equals(const std::string& a, const std::string& b)
{
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i)
  {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

// 尝试把首帧解析为 auth 请求。成功返回 true 并输出 request_id 与 payload.token。
static bool try_parse_auth_frame(const std::string& frame, std::string& request_id, std::string& token)
{
  if (frame.empty()) return false;
  json parsed;
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string errs;
  if (!reader->parse(frame.data(), frame.data() + frame.size(), &parsed, &errs)) return false;
  if (parsed.get("command", "").asString() != "auth") return false;
  request_id = parsed.get("request_id", "").asString();
  token = parsed["payload"]["token"].asString();
  return true;
}
```

在 `handle_client_session` 中，创建 Managers 之后、进入请求循环之前插入首帧处理：

```cpp
  std::map<std::string, json> cleanup_tasks;
  int cleanup_counter = 0;

  // 首帧处理：
  //  - 配置了 token：首帧必须是 auth 且 token 匹配（fail-closed），否则回错误并断开；
  //  - 未配置 token（传统模式）：不强制认证，首帧是普通命令则直接处理，首帧是 auth 则按通过处理。
  std::string first_frame = connection->read();
  bool session_open = false;
  if (!first_frame.empty())
  {
    std::string auth_req_id, auth_token;
    bool is_auth_frame = try_parse_auth_frame(first_frame, auth_req_id, auth_token);
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";

    if (auth_token_.empty())
    {
      // 传统兼容模式：无 token 不强制认证
      if (is_auth_frame)
      {
        // 客户端带了 token 但 executor 未配置：视为通过（空 token 无从校验）
        json ok;
        ok["request_id"] = auth_req_id;
        ok["status"] = "success";
        session_open = connection->write(Json::writeString(writer, ok));
      }
      else
      {
        // 首帧是普通命令：按传统行为直接处理
        std::string response_str = handle_session_request(
          first_frame, lib_manager, struct_manager, callback_manager, ffi_dispatcher, cleanup_tasks, cleanup_counter);
        session_open = connection->write(response_str);
      }
    }
    else
    {
      // 强制认证模式
      if (is_auth_frame && secure_equals(auth_token, auth_token_))
      {
        json ok;
        ok["request_id"] = auth_req_id;
        ok["status"] = "success";
        session_open = connection->write(Json::writeString(writer, ok));
      }
      else
      {
        json err;
        err["request_id"] = auth_req_id;
        err["status"] = "error";
        err["error_message"] = "Authentication failed";
        connection->write(Json::writeString(writer, err));
        connection->close();
      }
    }
  }

  while (session_open && is_running_ && connection->isOpen())
  {
    // ... 原有请求循环体保持不变
  }
```

- [ ] **Step 5: 运行全部测试，确认全绿**

Run: `cmake --build build --config Release && ctest --test-dir build --output-on-failure --timeout 55`
Expected: 全部 PASS，包括 `RejectsClientWithoutAuthHandshake`、`RejectsClientWithWrongToken` 及所有既有测试。

- [ ] **Step 6: Commit**

```bash
git add src/executor.h src/executor.cpp test/multi_client_test.cpp
git commit -m "feat: token auth handshake on IPC connections (enforced when token configured)"
```

---

### Task 2: main.cpp —— token 参数解析（--token / --token-file / 环境变量）

**Files:**
- Modify: `src/main.cpp` — 通用参数解析、`read_token_file`、无 token 时传统模式警告

**Interfaces:**
- Consumes: `Executor::run(pipe_name, auth_token)`（Task 1）
- Produces: 命令行协议：`executor --pipe <pipe_name> [--token <value> | --token-file <path>]`；token 解析优先级 `--token` > `--token-file` > 环境变量 `RPC_PROXY_TOKEN`；**三者为空时不退出，打印警告并以传统模式启动**（不强制认证）
- 备注：`--token` 会出现在 `ps` 输出中，仅用于开发/CI；生产建议 `--token-file`（文件权限 0600）或环境变量

- [ ] **Step 1: 重写 `main.cpp` 的参数解析**

修改 `src/main.cpp`：顶部 include 增加 `<fstream>`、`<cstdlib>`；删除原 `argc != 3` 的用法检查；新增：

```cpp
// 读取 token 文件：取第一行并去除行尾空白（兼容 CRLF）。
static std::string read_token_file(const std::string& path)
{
  std::ifstream in(path);
  if (!in.is_open())
  {
    throw std::runtime_error("Cannot open token file: " + path);
  }
  std::string token;
  std::getline(in, token);
  while (!token.empty() &&
         (token.back() == '\n' || token.back() == '\r' || token.back() == ' ' || token.back() == '\t'))
  {
    token.pop_back();
  }
  if (token.empty())
  {
    throw std::runtime_error("Token file is empty: " + path);
  }
  return token;
}
```

`main()` 改为：

```cpp
int main(int argc, char* argv[]) {
    g_argv = argv;
    setup_crash_handler();

    std::string pipe_name;
    std::string token;

    for (int i = 1; i < argc; ++i)
    {
      std::string arg = argv[i];
      if (arg == "--pipe" && i + 1 < argc)
      {
        pipe_name = argv[++i];
      }
      else if (arg == "--token" && i + 1 < argc)
      {
        token = argv[++i];
      }
      else if (arg == "--token-file" && i + 1 < argc)
      {
        token = read_token_file(argv[++i]);
      }
      else
      {
        std::cerr << "Unknown or incomplete argument: " << arg << std::endl;
        return 1;
      }
    }

    if (pipe_name.empty())
    {
      std::cerr << "Usage: " << argv[0]
                << " --pipe <pipe_name> [--token <token> | --token-file <path>]" << std::endl;
      std::cerr << "Token can also be provided via the RPC_PROXY_TOKEN environment variable." << std::endl;
      return 1;
    }

    // Token 解析优先级: --token > --token-file > RPC_PROXY_TOKEN 环境变量
    // 三者都未提供时：保留传统模式（不强制认证），打印警告后继续启动
    if (token.empty())
    {
      const char* env_token = std::getenv("RPC_PROXY_TOKEN");
      if (env_token != nullptr) token = env_token;
    }
    if (token.empty())
    {
      std::cerr << "WARNING: no authentication token configured (--token/--token-file/RPC_PROXY_TOKEN). "
                   "Running in legacy mode: connections will NOT be authenticated." << std::endl;
    }

    try {
        Executor executor;
        executor.run(pipe_name, token);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
```

- [ ] **Step 2: 手动验证四种启动方式**

```bash
cmake --build build --config Release
# 1. 无 token → 传统模式启动（打印警告，不退出）
build/executor --pipe my_pipe &
sleep 1; kill %1

# 2. --token 正常启动（后台，验证后 kill）
build/executor --pipe my_pipe --token dev-token-123 &
sleep 1; kill %1

# 3. 环境变量正常启动
RPC_PROXY_TOKEN=dev-token-123 build/executor --pipe my_pipe &
sleep 1; kill %1

# 4. --token-file 正常启动
printf 'file-token-456\n' > /tmp/rpc.token && chmod 600 /tmp/rpc.token
build/executor --pipe my_pipe --token-file /tmp/rpc.token &
sleep 1; kill %1; rm -f /tmp/rpc.token /tmp/my_pipe
```

Expected: 1 打印 `WARNING: no authentication token configured ... legacy mode` 且不退出（输出 `Executor service listening on: my_pipe`）；2/3/4 均正常启动，且不打印 WARNING。

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "feat: executor token configuration via --token/--token-file/RPC_PROXY_TOKEN"
```

---

### Task 3: Python 控制器支持认证

**Files:**
- Modify: `examples/python_controller/controller.py` — `RpcProxyClient`、`SimpleClient`、`run_client_session`、`main`

**Interfaces:**
- Consumes: Task 1 的 auth 协议（首帧 `auth` 请求，响应回显 `request_id`，`status == "success"` 为通过）
- Produces: `RpcProxyClient(pipe_name, token=None)`、`SimpleClient(pipe_name, client_id, token=None)`；token 为空时回退 `os.environ.get("RPC_PROXY_TOKEN", "")`；`main()` 支持可选第二参数 `python3 controller.py <pipe_name> [<token>]`；`connect()` 成功后自动完成认证，失败抛异常
- 关键约束：`RpcProxyClient._send_request` 依赖响应中的 `request_id` 关联 future，所以 auth 响应必须回显（Task 1 已实现）；认证必须放在 receiver 线程启动之后、返回 `connect()` 之前

- [ ] **Step 1: 修改 `RpcProxyClient`**

```python
  def __init__(self, pipe_name, token=None):
    self.pipe_name = pipe_name
    self.token = token if token is not None else os.environ.get("RPC_PROXY_TOKEN", "")
    # ... 其余初始化保持不变
```

在 `connect()` 中 receiver 线程启动之后、`print(f"{Colors.BRIGHT_GREEN}Connected.{Colors.RESET}")` 之后加：

```python
      self.authenticate()
```

新增方法（放在 `connect()` 之后）：

```python
  def authenticate(self):
    """发送认证握手；失败抛出异常。token 可为空串，由服务端决定是否放行。"""
    request = {
      "command": "auth",
      "request_id": self._get_next_request_id(),
      "payload": {"token": self.token}
    }
    response = self._send_request(request)
    if response.get("status") != "success":
      raise ConnectionError(f"Authentication failed: {response.get('error_message', 'unknown error')}")
```

- [ ] **Step 2: 修改 `SimpleClient` 与 `run_client_session`**

```python
  def __init__(self, pipe_name, client_id, token=None):
    self.pipe_name = pipe_name
    self.client_id = client_id
    self.token = token if token is not None else os.environ.get("RPC_PROXY_TOKEN", "")
    # ... 其余初始化保持不变

  def authenticate(self):
    """发送认证握手；失败抛出异常。token 可为空串，由服务端决定是否放行。"""
    response = self.call("auth", {"token": self.token})
    if not response or response.get("status") != "success":
      raise ConnectionError(f"Authentication failed: {response.get('error_message', 'unknown error')}")
```

`run_client_session` 签名改为 `def run_client_session(client_id, pipe_name, lib_path, token):`，`connect()` 成功后加：

```python
    if not client.connect():
      return

    # 认证握手
    try:
      client.authenticate()
    except Exception as e:
      safe_print(f"[Client {client_id}] {Colors.RED}Authentication failed: {e}{Colors.RESET}")
      return
```

- [ ] **Step 3: 修改 `main()`**

```python
def main():
  if len(sys.argv) < 2:
    print(f"{Colors.BRIGHT_RED}Usage: python {sys.argv[0]} <pipe_name> [<token>]{Colors.RESET}")
    sys.exit(1)

  pipe_name = sys.argv[1]
  token = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("RPC_PROXY_TOKEN", "")
  # ... lib_path 查找逻辑保持不变
  client = RpcProxyClient(pipe_name, token)
  # ... 其余保持不变
```

线程创建处改为传 token：

```python
    thread = threading.Thread(target=run_client_session, args=(i, pipe_name, lib_path, token))
```

- [ ] **Step 4: 端到端验证（先起带 token 的 executor，再跑控制器）**

```bash
cmake --build build --config Release
cmake -B test_lib/build test_lib && cmake --build test_lib/build
RPC_PROXY_TOKEN=py-test-token build/executor --pipe py_pipe &
sleep 2
RPC_PROXY_TOKEN=py-test-token python3 examples/python_controller/controller.py py_pipe
kill %1; rm -f /tmp/py_pipe
```

Expected: 全部测试 PASS；最后 5 个并发客户端会话全部 SUCCESS。再验证错误 token 会被拒绝：改用一个错误 token 跑一次，期望第一个测试抛 `Authentication failed`。

- [ ] **Step 5: Commit**

```bash
git add examples/python_controller/controller.py
git commit -m "feat: python controller auth handshake"
```

---

### Task 4: Java 控制器支持认证

**Files:**
- Modify: `examples/java_controller/src/main/java/com/kekxv/rpc/RpcClient.java` — 双构造器 + `authenticate()`
- App.java / MultiClientApp.java 无需改动（单参构造器自动读 `RPC_PROXY_TOKEN` 环境变量）

**Interfaces:**
- Consumes: Task 1 的 auth 协议
- Produces: `RpcClient(String pipeName)`（token 取 `System.getenv("RPC_PROXY_TOKEN")`）、`RpcClient(String pipeName, String token)`；`connect()` 在接收线程启动后自动认证，失败抛 `IOException`；token 为空时仍发送 auth 帧（空 token 由服务端决定放行）

- [ ] **Step 1: 增加 token 字段与双构造器**

```java
  private final String pipeName;
  private final String token;
```

```java
  public RpcClient(String pipeName) {
    this(pipeName, System.getenv("RPC_PROXY_TOKEN"));
  }

  public RpcClient(String pipeName, String token) {
    this.pipeName = pipeName;
    this.token = token;
  }
```

（删除旧单构造器 `public RpcClient(String pipeName) { this.pipeName = pipeName; }`。）

- [ ] **Step 2: `connect()` 启动接收线程后调用认证**

```java
    // 启动接收线程
    receiveThread = new Thread(this::receiveLoop, "Rpc-Receiver-" + pipeName);
    receiveThread.setDaemon(true);
    receiveThread.start();

    authenticate();
```

新增方法（放在 `receiveLoop()` 之前）：

```java
  /**
   * 认证握手：连接后第一条消息必须是 auth。失败抛 IOException。
   * token 可为空串，由服务端决定是否放行（无 token 的 executor 为传统模式）。
   * 注意：sendRequest 会自行分配 request_id，响应回显后由 pendingRequests 关联。
   */
  private void authenticate() throws IOException {
    try {
      JSONObject request = new JSONObject()
        .put("command", "auth")
        .put("payload", new JSONObject().put("token", token == null ? "" : token));
      JSONObject response = sendRequest(request);
      if (!"success".equals(response.optString("status"))) {
        throw new IOException("Authentication failed: " + response.optString("error_message"));
      }
    } catch (IOException e) {
      throw e;
    } catch (Exception e) {
      throw new IOException("Authentication failed", e);
    }
  }
```

- [ ] **Step 3: 端到端验证**

```bash
cmake --build build --config Release
cmake -B test_lib/build test_lib && cmake --build test_lib/build
RPC_PROXY_TOKEN=java-test-token build/executor --pipe java_pipe &
sleep 2
RPC_PROXY_TOKEN=java-test-token java -jar examples/java_controller/target/java-controller-1.0-SNAPSHOT-jar-with-dependencies.jar java_pipe
RPC_PROXY_TOKEN=java-test-token java -cp examples/java_controller/target/java-controller-1.0-SNAPSHOT-jar-with-dependencies.jar com.kekxv.MultiClientApp java_pipe
kill %1; rm -f /tmp/java_pipe
```

Expected: 两个 Java 程序全部测试 PASS。构建 jar：`cd examples/java_controller && mvn clean install && cd ../..`（如尚未构建）。

- [ ] **Step 4: Commit**

```bash
git add examples/java_controller/src/main/java/com/kekxv/rpc/RpcClient.java
git commit -m "feat: java controller auth handshake"
```

---

### Task 5: C++ 示例控制器支持认证

**Files:**
- Modify: `examples/cpp_controller/cpp_controller_example.cpp` — `RpcClient` 构造器加 token、`connect()` 内认证、`main()` 解析 token

**Interfaces:**
- Consumes: Task 1 的 auth 协议
- Produces: `RpcClient(pipe_name, token)`；用法 `cpp_controller_example <pipe_name> [<token>]`，缺省回退 `RPC_PROXY_TOKEN` 环境变量，两者皆空则打印提示并以空 token 运行（由服务端决定是否放行）；`connect()` 内自动认证，失败抛 `std::runtime_error`
- 关键约束：认证必须在 receiver 线程启动、`running_ = true` 之后（`send_request` 依赖 `running_` 与 receiver 线程）

- [ ] **Step 1: `RpcClient` 构造器加 token 并认证**

```cpp
  RpcClient(const std::string& pipe_name, const std::string& token)
    : pipe_name_(pipe_name), token_(token), sock_(RPC_INVALID_SOCKET), request_id_counter_(0), running_(false)
  {
  }
```

成员区加 `std::string token_;`（放在 `std::string pipe_name_;` 之后）。

`connect()` 中 receiver 线程启动之后、`std::cout << "Connected to " << pipe_name_ << std::endl;` 之前加：

```cpp
    // 认证握手：连接后第一条消息必须是 auth
    json auth_req;
    auth_req["command"] = "auth";
    auth_req["payload"]["token"] = token_;
    json auth_res = send_request(auth_req);
    if (auth_res["status"].asString() != "success")
    {
      throw std::runtime_error("Authentication failed: " + auth_res.get("error_message", "").asString());
    }
```

`main()` 改为：

```cpp
int main(int argc, char* argv[])
{
  if (argc < 2)
  {
    std::cerr << "Usage: " << argv[0] << " <pipe_name> [<token>]" << std::endl;
    std::cerr << "Token can also be provided via the RPC_PROXY_TOKEN environment variable." << std::endl;
    return 1;
  }

  std::string token;
  if (argc > 2)
  {
    token = argv[2];
  }
  else
  {
    const char* env_token = std::getenv("RPC_PROXY_TOKEN");
    if (env_token != nullptr) token = env_token;
  }
  if (token.empty())
  {
    std::cerr << "Warning: no authentication token configured; running in legacy mode "
                 "(the executor decides whether to accept)." << std::endl;
  }

  try
  {
    RpcClient client(argv[1], token);
    client.connect();
    // ... 其余测试逻辑保持不变
```

`#include <cstdlib>` 需加在文件头部 include 区。

- [ ] **Step 2: 端到端验证**

```bash
cmake --build build --config Release
cmake -B test_lib/build test_lib && cmake --build test_lib/build
cmake -B examples/cpp_controller/build -DCMAKE_BUILD_TYPE=Release examples/cpp_controller
cmake --build examples/cpp_controller/build
RPC_PROXY_TOKEN=cpp-test-token build/executor --pipe cpp_pipe &
sleep 2
RPC_PROXY_TOKEN=cpp-test-token examples/cpp_controller/build/cpp_controller_example cpp_pipe
kill %1; rm -f /tmp/cpp_pipe
```

Expected: 全部测试 PASS。

- [ ] **Step 3: Commit**

```bash
git add examples/cpp_controller/cpp_controller_example.cpp
git commit -m "feat: cpp example controller auth handshake"
```

---

### Task 6: CI 工作流与文档

**Files:**
- Modify: `.github/workflows/build_test_and_examples.yml` — 三个示例运行步骤传 token
- Modify: `README.md` — 用法、安全性章节更新
- Modify: `GEMINI.md` — 通信协议章节补 `auth` 命令

**Interfaces:**
- Consumes: Task 1/2 的启动参数与 auth 协议；Task 3/4/5 的客户端 token 参数
- Produces: 可复现的 CI 验证；README 中 executor 启动用法（`--token` / `--token-file` / `RPC_PROXY_TOKEN`）、客户端用法（第二参数或环境变量）、安全模型说明

- [ ] **Step 1: CI 三个示例步骤加 token（executor 用 `--token`，客户端用环境变量）**

Python 步骤改为：

```yaml
    - name: Run Python Example
      timeout-minutes: 1
      run: |
        build/executor --pipe my_pipe --token ci-test-token &
        EXECUTOR_PID=$!
        sleep 2
        RPC_PROXY_TOKEN=ci-test-token python3 examples/python_controller/controller.py my_pipe
        kill $EXECUTOR_PID
        rm /tmp/my_pipe # Clean up Unix domain socket
```

Java 步骤的 run 块改为：

```yaml
      run: |
        build/executor --pipe my_pipe --token ci-test-token &
        EXECUTOR_PID=$!
        sleep 3
        RPC_PROXY_TOKEN=ci-test-token java -jar examples/java_controller/target/java-controller-1.0-SNAPSHOT-jar-with-dependencies.jar my_pipe
        RPC_PROXY_TOKEN=ci-test-token java -cp  examples/java_controller/target/java-controller-1.0-SNAPSHOT-jar-with-dependencies.jar com.kekxv.MultiClientApp my_pipe
        kill $EXECUTOR_PID
        rm /tmp/my_pipe # Clean up Unix domain socket
```

C++ 步骤的 run 块改为：

```yaml
      run: |
        build/executor --pipe my_pipe --token ci-test-token &
        EXECUTOR_PID=$!
        sleep 2
        RPC_PROXY_TOKEN=ci-test-token examples/cpp_controller/build/cpp_controller_example my_pipe
        kill $EXECUTOR_PID
        rm /tmp/my_pipe # Clean up Unix domain socket
```

- [ ] **Step 2: 更新 README.md**

1. 第 10 节"工作流程示例"步骤 1 改为：

```markdown
1.  **启动 Executor**: 在一个终端中运行 `executor --pipe my_executor_1 --token <token>`（或 `--token-file <path>`、`RPC_PROXY_TOKEN` 环境变量）。程序将阻塞，等待连接。
```

并在步骤 2 后插入新步骤：

```markdown
2.  **Controller 连接并认证**: Controller 打开并连接到 `/tmp/my_executor_1`，发送的第一条消息必须是 `auth` 请求：`{"command":"auth","request_id":"req-1","payload":{"token":"<token>"}}`。认证失败时 executor 会关闭连接。
```

（后续步骤号顺延，最终步骤改为"结束"不变。）

2. 第 11 节安全性条目更新为：

```markdown
*   **安全性**: 自 v2 起，配置 token 时 IPC 端点强制认证（`--token` / `--token-file` / `RPC_PROXY_TOKEN`），未认证连接一律关闭；未配置 token 时以传统模式运行（启动时打印警告）。但 token 只解决"谁能连"；一旦连接建立，客户端仍可加载任意 DLL 并调用任意导出函数。因此 executor 仍必须以最低权限账号运行，并建议在容器/沙箱中隔离。
```

3. 第 12 节使用说明：`./executor --pipe my_pipe` 处（约 481 行与 488 行）改为带 token 的示例，并增加一段说明：

```markdown
Token 提供方式（按优先级）：
1.  `--token <token>` —— 注意会出现在 `ps` 输出中，仅建议开发/CI 使用；
2.  `--token-file <path>` —— 推荐生产使用，token 文件建议 `chmod 600`；
3.  环境变量 `RPC_PROXY_TOKEN`。
配置了 token 时 IPC 强制认证；三者都未设置时 executor 以传统模式启动（打印警告，不强制认证）。客户端（Python/Java/C++ 示例）通过第二命令行参数或 `RPC_PROXY_TOKEN` 环境变量传入同一 token。
```

- [ ] **Step 3: 更新 GEMINI.md 通信协议章节**

在第 6 节命令列表（`"command": "load_library | ... | call_function"` 处）加入 `auth`，并在该节补充：

```markdown
**认证（auth）**：连接建立后，客户端发送的第一条消息必须是 `auth` 请求：

```json
{ "command": "auth", "request_id": "req-1", "payload": { "token": "<token>" } }
```

executor 校验 token（常量时间比较）后响应 `{ "request_id": "req-1", "status": "success" }`；失败则响应 `{ "request_id": "req-1", "status": "error", "error_message": "Authentication failed" }` 并关闭连接。token 由 `--token` / `--token-file` / `RPC_PROXY_TOKEN` 提供；未配置 token 时 executor 以传统模式运行（不强制认证，启动时打印警告）。
```

- [ ] **Step 4: 本地验证全部测试 + 示例**

```bash
cmake --build build --config Release
ctest --test-dir build --output-on-failure --timeout 55
# Python / Java / C++ 三个示例各按 Task 3/4/5 的验证命令跑一遍
```

Expected: ctest 全绿，三个示例全 PASS。

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/build_test_and_examples.yml README.md GEMINI.md
git commit -m "docs: document and CI-verify token auth"
```

---

## 完成标准（Definition of Done）

- [ ] `executor --pipe <name>`（无 token）以传统模式启动（打印 WARNING，不退出，不强制认证）
- [ ] 配置 token 时：未认证/错误 token/首帧非 `auth` 的连接被拒绝并关闭（`RejectsClientWithoutAuthHandshake`、`RejectsClientWithWrongToken` 测试通过）
- [ ] 正确 token 下，全部既有命令与回调功能不受影响（ctest 全绿；Python/Java/C++ 三个示例全 PASS，Windows 与 Linux 同一协议）
- [ ] token 比较为常量时间；`--token` / `--token-file` / `RPC_PROXY_TOKEN` 三种方式均可启动
- [ ] README/GEMINI 文档与实现一致
