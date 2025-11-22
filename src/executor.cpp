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

// 全局日志锁，防止多线程打印乱码
static std::mutex g_log_mutex;

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
  FfiDispatcher& ffi_disp
)>;

// 注册所有支持的命令
static const std::map<std::string, CommandHandler> COMMAND_DISPATCHER = {
  {
    "load_library", [](const json& payload, json& resp, LibManager& lib, StructManager&, CallbackManager&,
                       FfiDispatcher&)
    {
      std::string path = payload["path"].asString();
      std::string lib_id = lib.load_library(path);
      resp["status"] = "success";
      resp["data"]["library_id"] = lib_id;
    }
  },
  {
    "unload_library", [](const json& payload, json& resp, LibManager& lib, StructManager&, CallbackManager&,
                         FfiDispatcher&)
    {
      std::string lib_id = payload["library_id"].asString();
      lib.unload_library(lib_id);
      resp["status"] = "success";
    }
  },
  {
    "register_struct", [](const json& payload, json& resp, LibManager&, StructManager& sm, CallbackManager&,
                          FfiDispatcher&)
    {
      std::string name = payload["struct_name"].asString();
      sm.register_struct(name, payload["definition"]);
      resp["status"] = "success";
    }
  },
  {
    "unregister_struct", [](const json& payload, json& resp, LibManager&, StructManager& sm, CallbackManager&,
                            FfiDispatcher&)
    {
      std::string name = payload["struct_name"].asString();
      sm.unregister_struct(name);
      resp["status"] = "success";
    }
  },
  {
    "register_callback", [](const json& payload, json& resp, LibManager&, StructManager&, CallbackManager& cm,
                            FfiDispatcher&)
    {
      std::string ret_type = payload["return_type"].asString();
      std::vector<std::string> args;
      const auto& args_json = payload["args_type"];
      if (args_json.isArray()) {
          for (const auto& arg : args_json) {
              args.push_back(arg.asString());
          }
      }
      std::string cb_id = cm.registerCallback(ret_type, args);
      resp["status"] = "success";
      resp["data"]["callback_id"] = cb_id;
    }
  },
  {
    "unregister_callback", [](const json& payload, json& resp, LibManager&, StructManager&, CallbackManager& cm,
                              FfiDispatcher&)
    {
      std::string cb_id = payload["callback_id"].asString();
      cm.unregisterCallback(cb_id);
      resp["status"] = "success";
    }
  },
  {
    "call_function", [](const json& payload, json& resp, LibManager& lib, StructManager&, CallbackManager&,
                        FfiDispatcher& ffi)
    {
      std::string lib_id = payload["library_id"].asString();
      std::string func_name = payload["function_name"].asString();
      void* func_ptr = lib.get_function(lib_id, func_name);
      json result = ffi.call_function(func_ptr, payload);
      resp["status"] = "success";
      resp["data"] = result;
    }
  }
};

std::string handle_session_request(
  const std::string& request_json_str,
  LibManager& lib_manager,
  StructManager& struct_manager,
  CallbackManager& callback_manager,
  FfiDispatcher& ffi_dispatcher)
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
    if (!reader->parse(request_json_str.data(), request_json_str.data() + request_json_str.size(), &request_json, &errs)) {
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
      it->second(payload, response_json, lib_manager, struct_manager, callback_manager, ffi_dispatcher);
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
  is_running_ = false; // Set the flag to signal the run loop to stop
  if (server)
  {
    server->stop(); // Interrupt any blocking accept() call
  }
}

// 处理单个客户端会话的逻辑（在独立线程中运行）
void Executor::handle_client_session(std::unique_ptr<ClientConnection> connection)
{
  // 资源隔离：每个线程/会话拥有独立的 Managers
  StructManager struct_manager;
  CallbackManager callback_manager(connection.get(), &struct_manager);
  LibManager lib_manager;
  FfiDispatcher ffi_dispatcher(struct_manager, &callback_manager);

  while (is_running_ && connection->isOpen()) // Also check is_running_ here
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
        request_str, lib_manager, struct_manager, callback_manager, ffi_dispatcher);
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
}

void Executor::run(const std::string& pipe_name)
{
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

    // 2. Spawn a new thread to handle this client
    std::thread([this, conn = std::move(connection)]() mutable
    {
      this->handle_client_session(std::move(conn));
    }).detach();
  }
}