#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <namedpipeapi.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
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
    // std::cout << "DummyClientConnection received event: " << event_json.dump() << std::endl;
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

    // Load the test library. The path is relative to the project root,
    // where the test executable is run from.
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
      // Fallback for different build configurations if needed
      lib_path = "cmake-build-debug/test_lib/my_lib.dylib";
      test_lib_id = lib_manager.load_library(lib_path);
    }
    ASSERT_FALSE(test_lib_id.empty()) << "Failed to load library from: " << lib_path;
  }

  void TearDown() override
  {
    // Unload the test library
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
  ASSERT_EQ(result["return"]["value"], 10); // 1+2+3+4 = 10
}

TEST_F(ExecutorTest, SumPoints)
{
  typedef struct
  {
    int32_t x;
    int32_t y;
  } Point;
  Point points_array[] = {{1, 1}, {2, 2}, {3, 3}};
  int count = sizeof(points_array) / sizeof(Point);

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
  ASSERT_EQ(result["return"]["value"], 12); // (1+1) + (2+2) + (3+3) = 2 + 4 + 6 = 12
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
  // 1. Register a callback signature
  std::string callback_id = callback_manager.registerCallback("void", {"string", "int32"});
  ASSERT_FALSE(callback_id.empty());

  // 2. Prepare to call the C function that expects a callback
  const char* test_message = "Hello from C++ unit test!";
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "call_my_callback"},
    {"return_type", "void"},
    {
      "args", {
        {
          {"type", "callback"},
          {"value", callback_id}
        },
        {
          {"type", "string"},
          {"value", test_message}
        }
      }
    }
  };

  // 3. Call the function
  ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "call_my_callback"), payload);

  // 4. Verify that the dummy connection received the event
  ASSERT_FALSE(dummy_connection.last_event.is_null());
  ASSERT_EQ(dummy_connection.last_event["event"], "invoke_callback");

  const auto& event_payload = dummy_connection.last_event["payload"];
  ASSERT_EQ(event_payload["callback_id"], callback_id);

  const auto& event_args = event_payload["args"];
  ASSERT_EQ(event_args.size(), 2);
  ASSERT_EQ(event_args[0]["type"], "string");
  ASSERT_EQ(event_args[0]["value"], test_message);
  ASSERT_EQ(event_args[1]["type"], "int32");
  ASSERT_EQ(event_args[1]["value"], 123);
}

TEST_F(ExecutorTest, ProcessBufferInout)
{
  const int buffer_capacity = 64;
  // Input data: a single byte 0x05, base64 encoded is "BQ=="
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

  // 1. Check direct return value
  ASSERT_EQ(result["return"]["type"], "int32");
  ASSERT_EQ(result["return"]["value"], 0); // Success code

  // 2. Check out_params
  const auto& out_params = result["out_params"];
  ASSERT_EQ(out_params.size(), 2);

  json buffer_param;
  json size_param;
  for (const auto& param : out_params)
  {
    if (param["index"] == 0)
    {
      buffer_param = param;
    }
    else if (param["index"] == 1)
    {
      size_param = param;
    }
  }

  ASSERT_FALSE(buffer_param.is_null());
  ASSERT_FALSE(size_param.is_null());

  // The C function reads 0x05, and writes {0xAA, 0x06, 0xDE, 0xAD} into the buffer.
  // The rest of the 64-byte buffer is zeros.
  // The expected base64 is for the entire buffer.
  const std::string expected_base64 =
    "qgberQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==";
  const int expected_size = 4;

  ASSERT_EQ(buffer_param["type"], "buffer");
  ASSERT_EQ(buffer_param["value"], expected_base64);

  // Verify the decoded content of the buffer
  const std::string expected_raw_data = "\xAA\x06\xDE\xAD";
  std::string decoded_output_buffer = base64_decode(buffer_param["value"].get<std::string>());

  // We only care about the first 4 bytes for comparison, as the rest are zeros written by C function.
  ASSERT_EQ(decoded_output_buffer.substr(0, 4), expected_raw_data);

  ASSERT_EQ(size_param["type"], "int32");
  ASSERT_EQ(size_param["value"], expected_size);
}


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

  void connect()
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
#ifdef _WIN32
    sock_ = CreateFileA(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (sock_ == INVALID_SOCKET_HANDLE)
    {
      throw std::runtime_error("Failed to connect to named pipe: " + std::to_string(GetLastError()));
    }
#else
    sock_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_ == INVALID_SOCKET_HANDLE)
    {
      throw std::runtime_error("Failed to create socket");
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, pipe_name_.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) == -1)
    {
      close(sock_);
      sock_ = INVALID_SOCKET_HANDLE;
      throw std::runtime_error("Failed to connect to Unix domain socket: " + pipe_name_);
    }
#endif
    running_ = true;
    receiver_thread_ = std::thread(&RpcTestClient::receive_messages, this);
  }

  void disconnect()
  {
    running_ = false;
    if (sock_ != INVALID_SOCKET_HANDLE)
    {
#ifdef _WIN32
      CloseHandle(sock_);
#else
      shutdown(sock_, SHUT_RDWR);
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
    std::string req_id = "req-" + std::to_string(++request_id_counter_);
    json request = request_payload;
    request["request_id"] = req_id;

    auto promise = std::make_shared<std::promise<json>>();
    std::future<json> future = promise->get_future();
    {
      std::lock_guard<std::mutex> lock(pending_requests_mutex_);
      pending_requests_[req_id] = promise;
    }

    std::string request_str = request.dump();
    uint32_t length = request_str.length();
    uint32_t network_order_length = htonl(length);

    std::lock_guard<std::mutex> send_lock(send_mutex_);
#ifdef _WIN32
    DWORD bytes_written;
    if (!WriteFile(sock_, &network_order_length, 4, &bytes_written, NULL) || !WriteFile(
      sock_, request_str.c_str(), length, &bytes_written, NULL))
    {
      throw std::runtime_error("Failed to write to pipe.");
    }
#else
    if (write(sock_, &network_order_length, 4) < 0 || write(sock_, request_str.c_str(), length) < 0)
    {
      throw std::runtime_error("Failed to write to socket.");
    }
#endif

    if (future.wait_for(std::chrono::seconds(10)) == std::future_status::timeout)
    {
      throw std::runtime_error("Timeout waiting for response for request ID " + req_id);
    }
    return future.get();
  }

private:
  void receive_messages()
  {
    while (running_)
    {
      uint32_t network_order_response_length;
#ifdef _WIN32
      DWORD bytes_read;
      if (!ReadFile(sock_, &network_order_response_length, 4, &bytes_read, NULL) || bytes_read == 0)
      {
        if (running_)
        {
          /* std::cerr << "Executor disconnected." << std::endl; */
        }
        break;
      }
#else
      ssize_t read_bytes = recv(sock_, &network_order_response_length, 4, 0);
      if (read_bytes <= 0)
      {
        if (running_)
        {
          /* std::cerr << "Executor disconnected." << std::endl; */
        }
        break;
      }
#endif
      uint32_t response_length = ntohl(network_order_response_length);
      if (response_length > 10 * 1024 * 1024)
      {
        // 10MB sanity check
        if (running_) std::cerr << "Excessive response length received: " << response_length << std::endl;
        break;
      }
      std::vector<char> response_buffer(response_length);
#ifdef _WIN32
      if (!ReadFile(sock_, response_buffer.data(), response_length, &bytes_read, NULL) || bytes_read == 0)
      {
        if (running_)
        {
          /* std::cerr << "Executor disconnected during read." << std::endl; */
        }
        break;
      }
#else
      ssize_t total_read = 0;
      while (total_read < response_length)
      {
        ssize_t read_bytes = recv(sock_, response_buffer.data() + total_read, response_length - total_read, 0);
        if (read_bytes <= 0)
        {
          if (running_)
          {
            /* std::cerr << "Executor disconnected during read." << std::endl; */
          }
          goto end_loop;
        }
        total_read += read_bytes;
      }
#endif
      std::string response_str(response_buffer.begin(), response_buffer.end());
      json response = json::parse(response_str);

      if (response.contains("request_id"))
      {
        std::string req_id = response["request_id"];
        std::shared_ptr<std::promise<json>> promise;
        {
          std::lock_guard<std::mutex> lock(pending_requests_mutex_);
          auto it = pending_requests_.find(req_id);
          if (it != pending_requests_.end())
          {
            promise = it->second;
            pending_requests_.erase(it);
          }
        }
        if (promise)
        {
          promise->set_value(response);
        }
      }
    }
  end_loop:;
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
      client.connect();
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

  void TearDown() override
  {
    if (executor)
    {
      executor->stop();
    }
    if (executor_thread.joinable())
    {
      executor_thread.join();
    }
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

  for (int i = 0; i < NUM_CHANNELS; ++i)
  {
    std::string pipe_name = "multi_pipe_" + std::to_string(i);
    executors.push_back(std::make_unique<Executor>());
    executor_threads.emplace_back([&executors, i, pipe_name]()
    {
      executors[i]->run(pipe_name);
    });
  }

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

  for (auto& exec : executors)
  {
    exec->stop();
  }

  for (auto& t : executor_threads)
  {
    t.join();
  }
}
