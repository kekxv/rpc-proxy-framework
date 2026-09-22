# Security Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复执行器中除 IPC 文件权限外的可证实安全问题，并用回归测试锁定行为。

**Architecture:** 在协议处理层增加资源配额与响应大小检查，在 FFI 层增加算术溢出和输出限制；在生命周期层阻止仍被引用的结构体注销，并让回调在连接关闭后失效。移除进程内崩溃自动重启，交由外部 supervisor 处理。

**Tech Stack:** C++17、JsonCpp、libffi、GoogleTest、CMake。

**Spec:** `README.md` 的 RPC/FFI 协议与现有执行器实现。

## Global Constraints

- 不修改 Unix socket/Windows named pipe 文件权限策略。
- 保持现有合法客户端协议和现有测试行为兼容。
- 单帧上限继续为 64 MiB。
- 所有新增限制必须返回可读的 RPC error，而不是让进程崩溃。

### Task 1: FFI 输入与输出边界

**Files:** `src/ffi_dispatcher.cpp`, `test/executor_test.cpp`

- [x] 为数组乘法溢出、过大输出和参数数量写失败测试。
- [x] 增加 checked multiplication、最大参数数和响应序列化大小检查。
- [x] 运行 FFI 单测和全量测试。

### Task 2: 结构体与回调生命周期

**Files:** `src/struct_manager.h`, `src/struct_manager.cpp`, `src/callback_manager.h`, `src/callback_manager.cpp`, `src/executor.cpp`, `test/executor_test.cpp`

- [x] 写测试证明被回调引用的结构体不能注销，连接关闭后回调事件不会访问失效连接。
- [x] 为结构体布局增加引用计数/占用检查；回调管理器在关闭时使连接指针失效。
- [x] 运行相关回调测试。

### Task 3: 崩溃处理与会话资源配额

**Files:** `src/main.cpp`, `src/executor.cpp`, `src/executor.h`, `test/cli_args_test.cmake`, `test/multi_client_test.cpp`

- [x] 写测试确认普通启动不会安装自动重启行为，并限制每会话 cleanup/回调/库数量。
- [x] 删除进程内 execv/CreateProcess 崩溃重启逻辑，加入会话级资源上限和错误响应。
- [x] 运行 CLI、集成和全量测试。

### Task 4: 验证

- [x] 使用 Debug 构建运行 `ctest --test-dir build --output-on-failure`。
- [x] 检查 `git diff`，确认未修改权限相关代码。
