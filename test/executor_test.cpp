#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <namedpipeapi.h>
// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <csignal>
#endif
#include "gtest/gtest.h"
#include "struct_manager.h"
#include "ffi_dispatcher.h"
#include "lib_manager.h"
#include "callback_manager.h"
#include "ipc_server.h"
#include "executor.h"
#include "utils/base64.h"

#include <memory>
#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <map>
#include <future>
#include <atomic>
#include <queue>
#include <vector>

using json = nlohmann::json;

// Forward declarations for test_lib functions
extern "C" {
int add(int a, int b);
char* greet(char* name);
}

// Dummy ClientConnection for testing CallbackManager in isolation
class DummyClientConnection : public ClientConnection
{
public:
  nlohmann::json last_event;

  std::string read() override { return ""; } // No-op
  bool write(const std::string& message) override { return true; } // No-op
  bool sendEvent(const nlohmann::json& event_json) override
  {
    last_event = event_json;
    return true;
  }

  bool isOpen() override { return true; } // Always open for test purposes
};

class ExecutorTest : public ::testing::Test
{
protected:
  StructManager struct_manager;
  DummyClientConnection dummy_connection; // Dummy connection for CallbackManager
  CallbackManager callback_manager;
  FfiDispatcher ffi_dispatcher;
  LibManager lib_manager;

  ExecutorTest() :
    struct_manager(),
    dummy_connection(),
    callback_manager(&dummy_connection, &struct_manager), // Pass dummy connection
    ffi_dispatcher(struct_manager, &callback_manager) // Pass CallbackManager to FfiDispatcher
  {
  }

  void SetUp() override
  {
    // Register Point struct
    json point_def = {
      {{"name", "x"}, {"type", "int32"}},
      {{"name", "y"}, {"type", "int32"}},
    };
    struct_manager.register_struct("Point", point_def);

    // Register Line struct (nested Point)
    json line_def = {
      {{"name", "p1"}, {"type", "Point"}},
      {{"name", "p2"}, {"type", "Point"}},
    };
    struct_manager.register_struct("Line", line_def);

    // Load the test library
#ifdef _WIN32
#ifdef CMAKE_BUILD_TYPE
    std::string lib_path = "../../test_lib/" CMAKE_BUILD_TYPE "/my_lib.dll";
#else
    std::string lib_path = "../test_lib/my_lib.dll";
#endif
#elif defined(__linux__)
    std::string lib_path = "build/test_lib/my_lib.so";
    if (access(lib_path.c_str(), F_OK) != 0)
    {
      lib_path = "../test_lib/my_lib.so";
    }
#else
    std::string lib_path = "../test_lib/my_lib.dylib";
#endif
    test_lib_id = lib_manager.load_library(lib_path);
    if (test_lib_id.empty())
    {
      lib_path = "cmake-build-debug/test_lib/my_lib.dylib";
      test_lib_id = lib_manager.load_library(lib_path);
    }
    ASSERT_FALSE(test_lib_id.empty()) << "Failed to load library from: " << lib_path;
  }

  void TearDown() override
  {
    lib_manager.unload_library(test_lib_id);
  }

  std::string test_lib_id;
};

TEST_F(ExecutorTest, BasicAddFunction)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "add"},
    {"return_type", "int32"},
    {
      "args", {
        {{"type", "int32"}, {"value", 10}},
        {{"type", "int32"}, {"value", 20}}
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "add"), payload);
  ASSERT_EQ(result["return"]["type"], "int32");
  ASSERT_EQ(result["return"]["value"], 30);
}

TEST_F(ExecutorTest, GreetFunction)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "greet"},
    {"return_type", "string"},
    {
      "args", {
        {{"type", "string"}, {"value", "World"}}
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "greet"), payload);
  ASSERT_EQ(result["return"]["type"], "string");
  ASSERT_EQ(result["return"]["value"], "Hello, World");
}

TEST_F(ExecutorTest, ProcessPointByVal)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "process_point_by_val"},
    {"return_type", "int32"},
    {
      "args", {
        {{"type", "Point"}, {"value", {{"x", 10}, {"y", 20}}}}
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "process_point_by_val"), payload);
  ASSERT_EQ(result["return"]["type"], "int32");
  ASSERT_EQ(result["return"]["value"], 30);
}

TEST_F(ExecutorTest, ProcessPointByPtr)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "process_point_by_ptr"},
    {"return_type", "int32"},
    {
      "args", {
        {
          {"type", "pointer"},
          {"target_type", "Point"},
          {"value", {{"x", 5}, {"y", 6}}}
        }
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "process_point_by_ptr"), payload);
  ASSERT_EQ(result["return"]["type"], "int32");
  ASSERT_EQ(result["return"]["value"], 11);
}

TEST_F(ExecutorTest, CreatePoint)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "create_point"},
    {"return_type", "Point"},
    {
      "args", {
        {{"type", "int32"}, {"value", 100}},
        {{"type", "int32"}, {"value", 200}}
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "create_point"), payload);
  ASSERT_EQ(result["return"]["type"], "Point");
  ASSERT_EQ(result["return"]["value"]["x"], 100);
  ASSERT_EQ(result["return"]["value"]["y"], 200);
}

TEST_F(ExecutorTest, GetLineLength)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "get_line_length"},
    {"return_type", "int32"},
    {
      "args", {
        {
          {"type", "Line"}, {
            "value", {
              {"p1", {{"x", 1}, {"y", 2}}},
              {"p2", {{"x", 3}, {"y", 4}}}
            }
          }
        }
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "get_line_length"), payload);
  ASSERT_EQ(result["return"]["type"], "int32");
  ASSERT_EQ(result["return"]["value"], 10);
}

TEST_F(ExecutorTest, SumPoints)
{
  int count = 3;
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "sum_points"},
    {"return_type", "int32"},
    {
      "args", {
        {
          {"type", "pointer"},
          {"target_type", "Point[]"},
          {
            "value", {
              {{"x", 1}, {"y", 1}},
              {{"x", 2}, {"y", 2}},
              {{"x", 3}, {"y", 3}}
            }
          }
        },
        {{"type", "int32"}, {"value", count}}
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "sum_points"), payload);
  ASSERT_EQ(result["return"]["type"], "int32");
  ASSERT_EQ(result["return"]["value"], 12);
}

TEST_F(ExecutorTest, CreateLine)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "create_line"},
    {"return_type", "Line"},
    {
      "args", {
        {{"type", "int32"}, {"value", 10}},
        {{"type", "int32"}, {"value", 20}},
        {{"type", "int32"}, {"value", 30}},
        {{"type", "int32"}, {"value", 40}}
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "create_line"), payload);
  ASSERT_EQ(result["return"]["type"], "Line");
  ASSERT_EQ(result["return"]["value"]["p1"]["x"], 10);
  ASSERT_EQ(result["return"]["value"]["p1"]["y"], 20);
  ASSERT_EQ(result["return"]["value"]["p2"]["x"], 30);
  ASSERT_EQ(result["return"]["value"]["p2"]["y"], 40);
}

TEST_F(ExecutorTest, CallbackFunction)
{
  std::string callback_id = callback_manager.registerCallback("void", {"string", "int32"});
  const char* test_message = "Hello from C++ unit test!";
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "call_my_callback"},
    {"return_type", "void"},
    {
      "args", {
        {{"type", "callback"}, {"value", callback_id}},
        {{"type", "string"}, {"value", test_message}}
      }
    }
  };

  ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "call_my_callback"), payload);

  ASSERT_FALSE(dummy_connection.last_event.is_null());
  ASSERT_EQ(dummy_connection.last_event["event"], "invoke_callback");
  ASSERT_EQ(dummy_connection.last_event["payload"]["callback_id"], callback_id);
  ASSERT_EQ(dummy_connection.last_event["payload"]["args"][0]["value"], test_message);
}

TEST_F(ExecutorTest, ProcessBufferInout)
{
  const int buffer_capacity = 64;
  const std::string input_base64 = "BQ==";

  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "process_buffer_inout"},
    {"return_type", "int32"},
    {
      "args", {
        {
          {"type", "buffer"},
          {"direction", "inout"},
          {"size", buffer_capacity},
          {"value", input_base64}
        },
        {
          {"type", "pointer"},
          {"target_type", "int32"},
          {"direction", "inout"},
          {"value", buffer_capacity}
        }
      }
    }
  };

  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "process_buffer_inout"), payload);

  ASSERT_EQ(result["return"]["type"], "int32");
  ASSERT_EQ(result["return"]["value"], 0);

  const auto& out_params = result["out_params"];
  ASSERT_EQ(out_params.size(), 2);

  json buffer_param = out_params[0]["index"] == 0 ? out_params[0] : out_params[1];

  const std::string expected_raw_data = "\xAA\x06\xDE\xAD";
  std::string decoded_output_buffer = base64_decode(buffer_param["value"].get<std::string>());
  ASSERT_EQ(decoded_output_buffer.substr(0, 4), expected_raw_data);
}

// ---------------------------------------------------------
// 优化后的 RpcTestClient
// ---------------------------------------------------------
class RpcTestClient
{
public:
#ifdef _WIN32
  using SocketType = HANDLE;
  const SocketType INVALID_SOCKET_HANDLE = INVALID_HANDLE_VALUE;
#else
  using SocketType = int;
  const SocketType INVALID_SOCKET_HANDLE = -1;
#endif

  RpcTestClient(const std::string& pipe_name_in) : sock_(INVALID_SOCKET_HANDLE), request_id_counter_(0), running_(false)
  {
#ifdef _WIN32
    pipe_name_ = "\\\\.\\pipe\\" + pipe_name_in;
#else
    pipe_name_ = "/tmp/" + pipe_name_in;
#endif
  }

  ~RpcTestClient()
  {
    disconnect();
  }

  // quick_wake_mode: 用于唤醒服务端的模式，只尝试少量次数，间隔短，不抛异常
  void connect(bool quick_wake_mode = false)
  {
    // 轮询间隔 10ms
    int interval_ms = 10;
    // 正常: 300 * 10ms = 3s; 唤醒: 10 * 10ms = 0.1s
    int max_retries = quick_wake_mode ? 10 : 300;
    bool connected = false;

    while (max_retries-- > 0)
    {
#ifdef _WIN32
      sock_ = CreateFileA(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
      if (sock_ != INVALID_SOCKET_HANDLE) { connected = true; break; }
      if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeA(pipe_name_.c_str(), 1);
#else
      sock_ = socket(AF_UNIX, SOCK_STREAM, 0);
      if (sock_ != INVALID_SOCKET_HANDLE)
      {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, pipe_name_.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) == 0) { connected = true; break; }
        else { close(sock_); sock_ = INVALID_SOCKET_HANDLE; }
      }
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    if (!connected)
    {
      if (quick_wake_mode) return; // 唤醒模式失败是正常的
      throw std::runtime_error("Failed to connect to: " + pipe_name_);
    }

    if (!quick_wake_mode) {
        running_ = true;
        receiver_thread_ = std::thread(&RpcTestClient::receive_messages, this);
    }
  }

  void disconnect()
  {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        // 如果不是运行状态（可能是 quick_wake），只清理 socket 句柄
        if (sock_ != INVALID_SOCKET_HANDLE) {
#ifdef _WIN32
            CloseHandle(sock_);
#else
            close(sock_);
#endif
            sock_ = INVALID_SOCKET_HANDLE;
        }
        return;
    }

    if (sock_ != INVALID_SOCKET_HANDLE)
    {
#ifdef _WIN32
      CancelIoEx(sock_, NULL); // 强制打断阻塞的 ReadFile
      CloseHandle(sock_);
#else
      shutdown(sock_, SHUT_RDWR); // 唤醒阻塞的 recv
      close(sock_);
#endif
      sock_ = INVALID_SOCKET_HANDLE;
    }
    if (receiver_thread_.joinable())
    {
      receiver_thread_.join();
    }
  }

  json send_request(const json& request_payload)
  {
    if (!running_) throw std::runtime_error("Client not connected");

    std::string req_id = "req-" + std::to_string(++request_id_counter_);
    json request = request_payload;
    request["request_id"] = req_id;

    auto promise = std::make_shared<std::promise<json>>();
    auto future = promise->get_future();
    {
      std::lock_guard<std::mutex> lock(pending_requests_mutex_);
      pending_requests_[req_id] = promise;
    }

    std::string request_str = request.dump();
    uint32_t len_net = htonl(static_cast<uint32_t>(request_str.length()));

    std::lock_guard<std::mutex> send_lock(send_mutex_);
#ifdef _WIN32
    DWORD written;
    if (!WriteFile(sock_, &len_net, 4, &written, NULL) ||
        !WriteFile(sock_, request_str.c_str(), request_str.length(), &written, NULL))
#else
    if (send(sock_, &len_net, 4, MSG_NOSIGNAL) < 0 ||
        send(sock_, request_str.c_str(), request_str.length(), MSG_NOSIGNAL) < 0)
#endif
    {
      std::lock_guard<std::mutex> lock(pending_requests_mutex_);
      pending_requests_.erase(req_id);
      throw std::runtime_error("Write failed");
    }

    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::timeout)
    {
      std::lock_guard<std::mutex> lock(pending_requests_mutex_);
      pending_requests_.erase(req_id);
      throw std::runtime_error("Timeout waiting for response " + req_id);
    }
    return future.get();
  }

private:
  void receive_messages()
  {
    while (running_)
    {
      uint32_t len_net;
#ifdef _WIN32
      DWORD bytes_read;
      if (!ReadFile(sock_, &len_net, 4, &bytes_read, NULL) || bytes_read == 0) break;
#else
      ssize_t r = recv(sock_, &len_net, 4, 0);
      if (r <= 0) break;
#endif
      uint32_t len = ntohl(len_net);
      if (len > 10 * 1024 * 1024) break; // Sanity check

      std::vector<char> buf(len);
      char* ptr = buf.data();
      uint32_t remain = len;
      bool err = false;

      while (remain > 0) {
#ifdef _WIN32
          if (!ReadFile(sock_, ptr, remain, &bytes_read, NULL) || bytes_read == 0) { err = true; break; }
          remain -= bytes_read; ptr += bytes_read;
#else
          ssize_t r2 = recv(sock_, ptr, remain, 0);
          if (r2 <= 0) { err = true; break; }
          remain -= r2; ptr += r2;
#endif
      }
      if (err) break;

      try {
        json resp = json::parse(std::string(buf.begin(), buf.end()));
        if (resp.contains("request_id")) {
          std::string id = resp["request_id"];
          std::lock_guard<std::mutex> lock(pending_requests_mutex_);
          if (pending_requests_.count(id)) {
            pending_requests_[id]->set_value(resp);
            pending_requests_.erase(id);
          }
        }
      } catch(...) {}
    }
    running_ = false;
  }

  std::string pipe_name_;
  SocketType sock_;
  std::atomic<int> request_id_counter_;
  std::thread receiver_thread_;
  std::atomic<bool> running_;
  std::mutex send_mutex_;
  std::map<std::string, std::shared_ptr<std::promise<json>>> pending_requests_;
  std::mutex pending_requests_mutex_;
};

class TestController
{
public:
  explicit TestController(std::string pipe_name) : pipe_name_(std::move(pipe_name))
  {
  }

  void run_test()
  {
    try
    {
      RpcTestClient client(pipe_name_);
      client.connect(); // Use normal connect
      json request = {{"command", "ping"}};
      json response = client.send_request(request);
      ASSERT_EQ(response["status"], "error");
      ASSERT_TRUE(response["error_message"].get<std::string>().find("Unknown command") != std::string::npos);
      client.disconnect();
    }
    catch (const std::exception& e)
    {
      FAIL() << "TestController on pipe '" << pipe_name_ << "' threw an exception: " << e.what();
    }
  }

private:
  std::string pipe_name_;
};

class ExecutorEndToEndTest : public ::testing::Test
{
protected:
  std::unique_ptr<Executor> executor;
  std::thread executor_thread;

  void SetUp() override {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); // Prevent Linux crash on broken pipe write
#endif
  }

  // Helper to wake up server if it is stuck in accept(), fast fail mode.
  void try_wake_server(const std::string& pipe_name) {
      try {
          RpcTestClient waker(pipe_name);
          waker.connect(true); // quick_wake_mode = true
      } catch(...) {}
  }

  void TearDown() override
  {
    if (executor)
    {
      executor->stop();
      // Only try to wake the single channel pipe here as fallback
      try_wake_server("single_channel_pipe");
    }
    if (executor_thread.joinable())
    {
      executor_thread.join();
    }
#ifndef _WIN32
    unlink("/tmp/single_channel_pipe");
#endif
  }
};

TEST_F(ExecutorEndToEndTest, SingleChannel)
{
  executor = std::make_unique<Executor>();
  executor_thread = std::thread([this]()
  {
    executor->run("single_channel_pipe");
  });

  TestController controller("single_channel_pipe");
  controller.run_test();
}

TEST_F(ExecutorEndToEndTest, MultiChannel)
{
  const int NUM_CHANNELS = 5;
  std::vector<std::unique_ptr<Executor>> executors;
  std::vector<std::thread> executor_threads;
  std::vector<std::unique_ptr<TestController>> controllers;
  std::vector<std::thread> controller_threads;

  // 1. Start Executors
  for (int i = 0; i < NUM_CHANNELS; ++i)
  {
    std::string pipe_name = "multi_pipe_" + std::to_string(i);
    executors.push_back(std::make_unique<Executor>());
    executor_threads.emplace_back([&executors, i, pipe_name]()
    {
      executors[i]->run(pipe_name);
    });
  }

  // 2. Run Tests
  for (int i = 0; i < NUM_CHANNELS; ++i)
  {
    std::string pipe_name = "multi_pipe_" + std::to_string(i);
    controllers.push_back(std::make_unique<TestController>(pipe_name));
    controller_threads.emplace_back([&controllers, i]()
    {
      controllers[i]->run_test();
    });
  }

  for (auto& t : controller_threads)
  {
    t.join();
  }

  // 3. Stop Executors
  for (auto& exec : executors)
  {
    exec->stop();
  }

  // 4. Parallel Wake-up (Optimization)
  // Wake up all servers concurrently so they exit their accept() loops
  std::vector<std::future<void>> wake_futures;
  for (int i = 0; i < NUM_CHANNELS; ++i)
  {
      std::string name = "multi_pipe_" + std::to_string(i);
      wake_futures.push_back(std::async(std::launch::async, [this, name]() {
          try_wake_server(name);
      }));
  }
  // Wait for wake attempts to finish (they are fast due to quick_wake_mode)
  for(auto& f : wake_futures) f.get();

  // 5. Join Server threads
  for (auto& t : executor_threads)
  {
    t.join();
  }

  // 6. Cleanup files
#ifndef _WIN32
  for(int i=0; i<NUM_CHANNELS; ++i) unlink(("/tmp/multi_pipe_" + std::to_string(i)).c_str());
#endif
}