#include "executor.h"
#include "ipc_server.h"
#include "lib_manager.h"
#include "struct_manager.h"
#include "callback_manager.h"
#include "ffi_dispatcher.h"
#include <json/json.h>

#include <iostream>
#include <stdexcept>
#include <memory>
#include <thread>
#include <vector>
#include <functional>
#include <map>
#include <mutex>
#include <atomic>

using json = Json::Value;

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
  if (!parsed.isObject()) return false; // 顶层非对象（字符串/数组等）：不是合法 auth 帧
  // 全程类型检查：此 jsoncpp 构建（JSON_USE_EXCEPTION=1）下，非字符串值调用 asString()
  // （以及 Value::get() 内部 find() 对非对象值）都会抛 Json::LogicError。因此只对 isString()
  // 的值取字符串；容器/数字/缺失一律视为字段无效 => 保持 fail-closed（"" 永远不等于已配置 token）。
  if (!parsed["command"].isString() || parsed["command"].asString() != "auth") return false;
  if (parsed["request_id"].isString())
  {
    request_id = parsed["request_id"].asString();
  }
  if (parsed["payload"].isObject())
  {
    const json& token_value = parsed["payload"]["token"];
    if (token_value.isString())
    {
      token = token_value.asString();
    }
  }
  return true;
}

// 全局日志锁，防止多线程打印乱码
std::mutex g_log_mutex;

// -----------------------------------------------------------------------------
// Command Dispatcher Logic
// -----------------------------------------------------------------------------

// 定义命令处理函数的签名
using CommandHandler = std::function<void(
  const json& request_payload,
  json& response_json,
  LibManager& lib_mgr,
  StructManager& struct_mgr,
  CallbackManager& cb_mgr,
  FfiDispatcher& ffi_disp,
  std::map<std::string, json>& cleanup_tasks,
  int& cleanup_counter
)>;

// 注册所有支持的命令
static const std::map<std::string, CommandHandler> COMMAND_DISPATCHER = {
  {
    "load_library", [](const json& payload, json& resp, LibManager& lib, StructManager&, CallbackManager&,
                       FfiDispatcher&, std::map<std::string, json>&, int&)
    {
      std::string path = payload["path"].asString();
      std::string lib_id = lib.load_library(path);
      resp["status"] = "success";
      resp["data"]["library_id"] = lib_id;
    }
  },
  {
    "unload_library", [](const json& payload, json& resp, LibManager& lib, StructManager&, CallbackManager&,
                         FfiDispatcher&, std::map<std::string, json>&, int&)
    {
      std::string lib_id = payload["library_id"].asString();
      lib.unload_library(lib_id);
      resp["status"] = "success";
    }
  },
  {
    "register_struct", [](const json& payload, json& resp, LibManager&, StructManager& sm, CallbackManager&,
                          FfiDispatcher&, std::map<std::string, json>&, int&)
    {
      std::string name = payload["struct_name"].asString();
      sm.register_struct(name, payload["definition"]);
      resp["status"] = "success";
    }
  },
  {
    "unregister_struct", [](const json& payload, json& resp, LibManager&, StructManager& sm, CallbackManager&,
                            FfiDispatcher&, std::map<std::string, json>&, int&)
    {
      std::string name = payload["struct_name"].asString();
      sm.unregister_struct(name);
      resp["status"] = "success";
    }
  },
  {
    "register_callback", [](const json& payload, json& resp, LibManager&, StructManager&, CallbackManager& cm,
                            FfiDispatcher&, std::map<std::string, json>&, int&)
    {
      std::string ret_type = payload["return_type"].asString();
      // Pass the args_type JSON value directly to allow complex definitions
      const auto& args_json = payload["args_type"];
      std::string cb_id = cm.registerCallback(ret_type, args_json);
      resp["status"] = "success";
      resp["data"]["callback_id"] = cb_id;
    }
  },
  {
    "unregister_callback", [](const json& payload, json& resp, LibManager&, StructManager&, CallbackManager& cm,
                              FfiDispatcher&, std::map<std::string, json>&, int&)
    {
      std::string cb_id = payload["callback_id"].asString();
      cm.unregisterCallback(cb_id);
      resp["status"] = "success";
    }
  },
  {
    "call_function", [](const json& payload, json& resp, LibManager& lib, StructManager&, CallbackManager&,
                        FfiDispatcher& ffi, std::map<std::string, json>&, int&)
    {
      std::string lib_id = payload["library_id"].asString();
      std::string func_name = payload["function_name"].asString();
      void* func_ptr = lib.get_function(lib_id, func_name);
      json result = ffi.call_function(func_ptr, payload);
      resp["status"] = "success";
      resp["data"] = result;
    }
  },
  {
    "register_cleanup", [](const json& payload, json& resp, LibManager&, StructManager&, CallbackManager&,
                           FfiDispatcher&, std::map<std::string, json>& cleanup_tasks, int& cleanup_counter)
    {
      // 注册一个清理任务，参数格式与 call_function 类似
      if (cleanup_tasks.size() >= 1024) {
        throw std::runtime_error("Too many cleanup tasks");
      }
      cleanup_counter++;
      std::string cleanup_id = "cleanup-" + std::to_string(cleanup_counter);
      cleanup_tasks[cleanup_id] = payload;
      
      resp["status"] = "success";
      resp["data"]["cleanup_id"] = cleanup_id;
    }
  },
  {
    "cancel_cleanup", [](const json& payload, json& resp, LibManager&, StructManager&, CallbackManager&,
                           FfiDispatcher&, std::map<std::string, json>& cleanup_tasks, int&)
    {
      std::string cleanup_id = payload["cleanup_id"].asString();
      if (cleanup_tasks.erase(cleanup_id) > 0) {
        resp["status"] = "success";
      } else {
        resp["status"] = "error";
        resp["error_message"] = "Cleanup task not found: " + cleanup_id;
      }
    }
  }
};

std::string handle_session_request(
  const std::string& request_json_str,
  LibManager& lib_manager,
  StructManager& struct_manager,
  CallbackManager& callback_manager,
  FfiDispatcher& ffi_dispatcher,
  std::map<std::string, json>& cleanup_tasks,
  int& cleanup_counter)
{
  json response_json;
  std::string req_id = "";
  Json::StreamWriterBuilder writer;
  writer["indentation"] = ""; // Compact JSON

  try
  {
    // 1. Parse Request
    json request_json;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    if (!reader->parse(request_json_str.data(), request_json_str.data() + request_json_str.size(), &request_json,
                       &errs))
    {
      throw std::runtime_error("Parse error: " + errs);
    }

    req_id = request_json.get("request_id", "").asString();
    response_json["request_id"] = req_id;

    // 2. Extract Command
    if (!request_json.isMember("command"))
    {
      throw std::runtime_error("Missing 'command' field in request");
    }
    std::string command = request_json["command"].asString();

    // 3. Dispatch Command
    auto it = COMMAND_DISPATCHER.find(command);
    if (it != COMMAND_DISPATCHER.end())
    {
      // 只有确认是支持的命令后，才尝试获取 payload
      if (!request_json.isMember("payload"))
      {
        // 如果缺 payload，下面的 operator[] 会创建 null 或返回 null，这在后续 logic 可能会报错
        // 为了保持原有逻辑，我们尝试获取它。
      }

      const auto& payload = request_json["payload"];
      it->second(payload, response_json, lib_manager, struct_manager, callback_manager, ffi_dispatcher, cleanup_tasks, cleanup_counter);
    }
    else
    {
      // 4. Handle Unknown Command (这就是测试用例期待的路径)
      throw std::runtime_error("Unknown command: " + command);
    }
  }
  catch (const std::exception& e)
  {
    response_json["status"] = "error";
    response_json["error_message"] = e.what();
    if (!req_id.empty())
    {
      response_json["request_id"] = req_id;
    }
  }

  return Json::writeString(writer, response_json);
}

// -----------------------------------------------------------------------------
// Executor Implementation
// -----------------------------------------------------------------------------

Executor::Executor() : server(IpcServer::create())
{
}

Executor::~Executor()
{
  stop();
}

void Executor::stop()
{
  is_running_ = false;
  if (server)
  {
    server->stop();
  }
  close_active_connections();
  join_session_threads();
}

void Executor::close_active_connections()
{
  std::vector<std::shared_ptr<ClientConnection>> connections;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    connections.assign(active_connections_.begin(), active_connections_.end());
  }
  for (const auto& connection : connections)
  {
    connection->close();
  }
}

void Executor::join_session_threads()
{
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    threads.swap(session_threads_);
  }
  for (auto& thread : threads)
  {
    if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) thread.join();
  }
}

// 处理单个客户端会话的逻辑（在独立线程中运行）
void Executor::handle_client_session(std::shared_ptr<ClientConnection> connection)
{
  ClientConnection* connection_ptr = connection.get();

  // 资源隔离：每个线程/会话拥有独立的 Managers
  StructManager struct_manager;
  CallbackManager callback_manager(connection.get(), &struct_manager);
  LibManager lib_manager;
  FfiDispatcher ffi_dispatcher(struct_manager, &callback_manager);
  std::map<std::string, json> cleanup_tasks;
  int cleanup_counter = 0;

  // 首帧处理：
  //  - 配置了 token：首帧必须是 auth 且 token 匹配（fail-closed），否则回错误并断开；
  //  - 未配置 token（传统模式）：不强制认证，首帧是普通命令则直接处理，首帧是 auth 则按通过处理。
  std::string first_frame = connection->read();
  bool session_open = false;
  if (!first_frame.empty())
  {
    try
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
    catch (const std::exception& e)
    {
      // 纵深防御：首帧处理中任何异常都不能崩溃进程 —— 一律按认证失败拒绝并断开连接（fail-closed）。
      json err;
      err["request_id"] = "";
      err["status"] = "error";
      err["error_message"] = "Authentication failed";
      Json::StreamWriterBuilder writer;
      writer["indentation"] = "";
      connection->write(Json::writeString(writer, err));
      connection->close();
    }
  }

  while (session_open && is_running_ && connection->isOpen()) // Also check is_running_ here
  {
    std::string request_str = connection->read();
    if (request_str.empty())
    {
      break; // Client disconnected or server is stopping
    }

    std::string response_str;
    try
    {
      response_str = handle_session_request(
        request_str, lib_manager, struct_manager, callback_manager, ffi_dispatcher, cleanup_tasks, cleanup_counter);
    }
    catch (const std::exception& e)
    {
      // 如果处理过程彻底崩溃（极少见），构建一个兜底错误
      json err;
      err["status"] = "error";
      err["error_message"] = std::string("Critical internal error: ") + e.what();
      Json::StreamWriterBuilder writer;
      writer["indentation"] = "";
      response_str = Json::writeString(writer, err);
    }

    if (!connection->write(response_str))
    {
      std::lock_guard<std::mutex> lock(g_log_mutex);
      std::cerr << "[Executor] Failed to write response. Connection lost." << std::endl;
      break;
    }
  }

  // 执行清理任务
  callback_manager.invalidateConnection(connection_ptr);
  for (const auto& pair : cleanup_tasks)
  {
    const json& task = pair.second;
    try
    {
      std::string lib_id = task["library_id"].asString();
      std::string func_name = task["function_name"].asString();
      
      // 尝试获取函数并调用
      // 注意：如果库已经被 unload_library 卸载，这里可能会抛出异常，这是预期的
      void* func_ptr = lib_manager.get_function(lib_id, func_name);
      ffi_dispatcher.call_function(func_ptr, task);
    }
    catch (const std::exception& e)
    {
      std::lock_guard<std::mutex> lock(g_log_mutex);
      std::cerr << "[Executor] Cleanup task failed: " << e.what() << std::endl;
    }
  }

  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    active_connections_.erase(connection);
  }
}

void Executor::run(const std::string& pipe_name, const std::string& auth_token)
{
  auth_token_ = auth_token;
  if (auth_token_.empty())
  {
    std::cerr << "WARNING: no auth token configured; running in legacy mode (connections will NOT be authenticated)" << std::endl;
  }
  is_running_ = true;
  server->listen(pipe_name);

  std::cout << "Executor service listening on: " << pipe_name << std::endl;

  while (is_running_) // Use the atomic flag as the loop condition
  {
    // 1. Accept a new connection (Blocking)
    std::unique_ptr<ClientConnection> connection = server->accept();

    if (!is_running_ || !connection)
    {
      // If the loop should stop or accept failed, break out
      std::cout << "Executor run loop is stopping..." << std::endl;
      break;
    }

    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      if (!is_running_)
      {
        connection->close();
        break;
      }
      std::shared_ptr<ClientConnection> shared_connection(std::move(connection));
      active_connections_.insert(shared_connection);
      std::thread session_thread([this, conn = std::move(shared_connection)]() mutable
      {
        this->handle_client_session(std::move(conn));
      });
      session_threads_.push_back(std::move(session_thread));
    }
  }
}
