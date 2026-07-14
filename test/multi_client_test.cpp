#include "gtest/gtest.h"
#include "executor.h"
#include "ipc_server.h"
#include "utils/base64.h"
#include <json/json.h>
#include <thread>
#include <vector>
#include <string>
#include <memory>
#include <future>
#include <chrono>
#include <iostream>
#include <atomic>
#include <cstring>
#include <cerrno>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

using json = Json::Value;

static std::mutex g_test_log_mutex;

// --- Helper Functions for JSON ---
std::string json_dump(const json& j)
{
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return Json::writeString(writer, j);
}

json json_parse(const std::string& s)
{
  json j;
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string errs;
  if (!reader->parse(s.data(), s.data() + s.size(), &j, &errs))
  {
    return json();
  }
  return j;
}

// --- Test Configuration ---
static std::string g_pipe_name;
static std::atomic<unsigned int> g_pipe_counter{0};
const int NUM_CLIENTS = 10; // Number of concurrent clients to simulate

/**
 * @brief A helper class to manage a simple client connection over IPC.
 * This abstracts the platform differences for connecting, writing, and reading.
 */
class SimplePipeClient
{
public:
  SimplePipeClient(int client_id) : client_id_(client_id)
  {
  };

  ~SimplePipeClient()
  {
    disconnect();
  }

  bool connect(const std::string& name)
  {
#ifdef _WIN32
    std::string full_pipe_name = "\\\\.\\pipe\\" + name;
    {
      std::lock_guard<std::mutex> lock(g_test_log_mutex);
      std::cout << "[Client " << client_id_ << "] Connecting to " << full_pipe_name << "..." << std::endl;
    }

    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(3)) // Retry for up to 3 seconds
    {
      pipe_handle_ = CreateFileA(
        full_pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL);

      if (pipe_handle_ != INVALID_HANDLE_VALUE)
      {
        // Success!
        std::lock_guard<std::mutex> lock(g_test_log_mutex);
        std::cout << "[Client " << client_id_ << "] CreateFileA succeeded. Handle: " << pipe_handle_ << std::endl;
        return true;
      }

      DWORD last_error = GetLastError();
      if (last_error == ERROR_PIPE_BUSY)
      {
        // This is expected when multiple clients race for a new pipe instance.
        // Wait a bit and retry.
        Sleep(50);
        continue;
      }

      // For other errors, log it and fail.
      std::lock_guard<std::mutex> lock(g_test_log_mutex);
      std::cerr << "[Client " << client_id_ << "] CreateFileA failed with unrecoverable error: " << last_error <<
        std::endl;
      return false;
    }

    // If we exit the loop, it means we timed out.
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cerr << "[Client " << client_id_ << "] Connection attempt timed out after 3 seconds."; return false;
#else
    socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd_ < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string path = "/tmp/" + name;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(2))
    {
      if (::connect(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) == 0)
      {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
#endif
  }

  void disconnect()
  {
#ifdef _WIN32
    if (pipe_handle_ != INVALID_HANDLE_VALUE)
    {
      {
        std::lock_guard<std::mutex> lock(g_test_log_mutex);
        std::cout << "[Client " << client_id_ << "] Disconnecting handle: " << pipe_handle_ << std::endl;
      }
      CloseHandle(pipe_handle_);
      pipe_handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (socket_fd_ != -1)
    {
      close(socket_fd_);
      socket_fd_ = -1;
    }
#endif
  }

  bool send_request(const std::string& request)
  {
    if (request.empty() || request.size() > kMaxIpcFrameSize) return false;
    uint32_t len = htonl(static_cast<uint32_t>(request.size()));
#ifdef _WIN32
    return write_all(&len, sizeof(len)) && write_all(request.data(), request.size());
#else
    return write_all(&len, sizeof(len)) && write_all(request.data(), request.size());
#endif
  }

  bool send_request_fragmented(const std::string& request, size_t chunk_size)
  {
    uint32_t len = htonl(static_cast<uint32_t>(request.size()));
    std::string frame(reinterpret_cast<const char*>(&len), sizeof(len));
    frame += request;
    for (size_t offset = 0; offset < frame.size(); offset += chunk_size)
    {
      size_t count = std::min(chunk_size, frame.size() - offset);
      if (!write_all(frame.data() + offset, count)) return false;
    }
    return true;
  }

  bool send_length_only(uint32_t host_length)
  {
    uint32_t len = htonl(host_length);
    return write_all(&len, sizeof(len));
  }

  std::string receive_response()
  {
    uint32_t net_msg_len;
#ifdef _WIN32
    if (!read_exact(&net_msg_len, sizeof(net_msg_len))) return "";
#else
    if (!read_exact(&net_msg_len, sizeof(net_msg_len))) return "";
#endif

    uint32_t msg_len = ntohl(net_msg_len);
    if (msg_len == 0 || msg_len > kMaxIpcFrameSize) return "";

    std::vector<char> buffer(msg_len);
    if (!read_exact(buffer.data(), buffer.size())) return "";
    return std::string(buffer.begin(), buffer.end());
  }

private:
  bool write_all(const void* data, size_t size)
  {
    const char* bytes = static_cast<const char*>(data);
    size_t total = 0;
    while (total < size)
    {
#ifdef _WIN32
      DWORD written = 0;
      if (!WriteFile(pipe_handle_, bytes + total, static_cast<DWORD>(size - total), &written, NULL) || written == 0)
        return false;
      total += written;
#else
#ifdef MSG_NOSIGNAL
      ssize_t written = send(socket_fd_, bytes + total, size - total, MSG_NOSIGNAL);
#else
      ssize_t written = send(socket_fd_, bytes + total, size - total, 0);
#endif
      if (written > 0) total += static_cast<size_t>(written);
      else if (written < 0 && errno == EINTR) continue;
      else return false;
#endif
    }
    return true;
  }

  bool read_exact(void* data, size_t size)
  {
    char* bytes = static_cast<char*>(data);
    size_t total = 0;
    while (total < size)
    {
#ifdef _WIN32
      DWORD count = 0;
      if (!ReadFile(pipe_handle_, bytes + total, static_cast<DWORD>(size - total), &count, NULL) || count == 0)
        return false;
      total += count;
#else
      ssize_t count = recv(socket_fd_, bytes + total, size - total, 0);
      if (count > 0) total += static_cast<size_t>(count);
      else if (count < 0 && errno == EINTR) continue;
      else return false;
#endif
    }
    return true;
  }

  int client_id_;
#ifdef _WIN32
  HANDLE pipe_handle_ = INVALID_HANDLE_VALUE;
#else
  int socket_fd_ = -1;
#endif
};

// --- Test Fixture ---

class MultiClientIntegrationTest : public ::testing::Test
{
protected:
  std::unique_ptr<Executor> executor_;
  std::thread executor_thread_;

  void SetUp() override
  {
    unsigned long process_id;
#ifdef _WIN32
    process_id = GetCurrentProcessId();
#else
    process_id = static_cast<unsigned long>(getpid());
#endif
    g_pipe_name = "rpc_test_" + std::to_string(process_id) + "_" + std::to_string(++g_pipe_counter);
    {
      std::lock_guard<std::mutex> lock(g_test_log_mutex);
      std::cout << "[Test Main] SetUp: Starting executor thread..." << std::endl;
    }
    executor_ = std::make_unique<Executor>();
    executor_thread_ = std::thread([this]()
    {
      {
        std::lock_guard<std::mutex> lock(g_test_log_mutex);
        std::cout << "[Test Main] Executor thread " << std::this_thread::get_id() << " started. Calling run().";
      }
      executor_->run(g_pipe_name);
      {
        std::lock_guard<std::mutex> lock(g_test_log_mutex);
        std::cout << "[Test Main] Executor thread " << std::this_thread::get_id() << " finished run()." << std::endl;
      }
    });

    {
      std::lock_guard<std::mutex> lock(g_test_log_mutex);
      std::cout << "[Test Main] SetUp: Waiting for server to be ready..." << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  void TearDown() override
  {
    {
      std::lock_guard<std::mutex> lock(g_test_log_mutex);
      std::cout << "[Test Main] TearDown: Calling executor->stop()." << std::endl;
    }
    executor_->stop();
    if (executor_thread_.joinable())
    {
      {
        std::lock_guard<std::mutex> lock(g_test_log_mutex);
        std::cout << "[Test Main] TearDown: Joining executor thread..." << std::endl;
      }
      executor_thread_.join();
      {
        std::lock_guard<std::mutex> lock(g_test_log_mutex);
        std::cout << "[Test Main] TearDown: Executor thread joined." << std::endl;
      }
    }
  }
};

// --- The Test Case ---

/**
 * @brief This function simulates a single client's actions.
 * It connects, loads a library, calls a function, and checks the result.
 * @param client_id A unique ID for this client to make requests unique.
 * @return True on success, false on failure.
 */
bool run_client_session(int client_id, const std::string& lib_path)
{
  {
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cout << "[Client " << client_id << "][TID:" << std::this_thread::get_id() << "] Starting session." <<
      std::endl;
  }
  SimplePipeClient client(client_id);
  if (!client.connect(g_pipe_name))
  {
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cerr << "[Client " << client_id << "] Failed to connect." << std::endl;
    return false;
  }

  // 1. Load Library
  json load_req;
  load_req["command"] = "load_library";
  load_req["request_id"] = "req-load-" + std::to_string(client_id);
  load_req["payload"]["path"] = lib_path;

  if (!client.send_request(json_dump(load_req)))
  {
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cerr << "[Client " << client_id << "] Failed to send load request." << std::endl;
    return false;
  }
  std::string response_str = client.receive_response();
  if (response_str.empty())
  {
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cerr << "[Client " << client_id << "] Received empty response for load request." << std::endl;
    return false;
  }
  json load_resp = json_parse(response_str);
  if (load_resp["status"].asString() != "success")
  {
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cerr << "[Client " << client_id << "] Library load failed: " << json_dump(load_resp) << std::endl;
    return false;
  }
  std::string lib_id = load_resp["data"]["library_id"].asString();

  // 2. Call Function 'add'
  int a = client_id * 10;
  int b = client_id + 1;
  json call_req;
  call_req["command"] = "call_function";
  call_req["request_id"] = "req-call-" + std::to_string(client_id);
  call_req["payload"]["library_id"] = lib_id;
  call_req["payload"]["function_name"] = "add";
  call_req["payload"]["return_type"] = "int32";

  json args(Json::arrayValue);
  {
    json arg;
    arg["type"] = "int32";
    arg["value"] = a;
    args.append(arg);
  }
  {
    json arg;
    arg["type"] = "int32";
    arg["value"] = b;
    args.append(arg);
  }
  call_req["payload"]["args"] = args;

  if (!client.send_request(json_dump(call_req)))
  {
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cerr << "[Client " << client_id << "] Failed to send call request." << std::endl;
    return false;
  }
  response_str = client.receive_response();
  if (response_str.empty())
  {
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cerr << "[Client " << client_id << "] Received empty response for call request." << std::endl;
    return false;
  }
  json call_resp = json_parse(response_str);
  if (call_resp["status"].asString() != "success" || call_resp["data"]["return"]["value"].asInt() != (a + b))
  {
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cerr << "[Client " << client_id << "] Function call failed or returned wrong value: " << json_dump(call_resp)
      <<
      std::endl;
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cout << "[Client " << client_id << "] Success!" << std::endl;
  }
  return true;
}

TEST_F(MultiClientIntegrationTest, HandleMultipleClientsConcurrently)
{
  {
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cout << "[Test Main] Starting " << NUM_CLIENTS << " client threads..." << std::endl;
  }
  std::string lib_path;
#ifdef _WIN32
#ifdef CMAKE_BUILD_TYPE
  lib_path = "../test_lib/" CMAKE_BUILD_TYPE "/my_lib.dll";
#else
  lib_path = "../test_lib/my_lib.dll";
#endif
#elif defined(__linux__)
  lib_path = "build/test_lib/my_lib.so";
  if (access(lib_path.c_str(), F_OK) != 0) lib_path = "../test_lib/my_lib.so";
  if (access(lib_path.c_str(), F_OK) != 0) lib_path = "my_lib.so";
#else // macOS
  lib_path = "../test_lib/my_lib.dylib";
  if (access(lib_path.c_str(), F_OK) != 0) lib_path = "build/test_lib/my_lib.dylib";
  if (access(lib_path.c_str(), F_OK) != 0) lib_path = "my_lib.dylib";
#endif

  std::vector<std::future<bool>> client_futures;

  for (int i = 0; i < NUM_CLIENTS; ++i)
  {
    client_futures.push_back(
      std::async(std::launch::async, run_client_session, i, lib_path)
    );
  }

  bool all_clients_succeeded = true;
  for (int i = 0; i < NUM_CLIENTS; ++i)
  {
    if (!client_futures[i].get())
    {
      all_clients_succeeded = false;
      std::lock_guard<std::mutex> lock(g_test_log_mutex);
      std::cerr << "[Test Main] Client " << i << " reported failure." << std::endl;
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_test_log_mutex);
    std::cout << "[Test Main] All client threads finished." << std::endl;
  }

  ASSERT_TRUE(all_clients_succeeded) << "One or more client threads failed.";
}

static std::string get_test_library_path()
{
#ifdef _WIN32
#ifdef CMAKE_BUILD_TYPE
  return "../test_lib/" CMAKE_BUILD_TYPE "/my_lib.dll";
#else
  return "../test_lib/my_lib.dll";
#endif
#elif defined(__linux__)
  const std::vector<std::string> paths = {"build/test_lib/my_lib.so", "../test_lib/my_lib.so", "my_lib.so"};
#else
  const std::vector<std::string> paths = {"../test_lib/my_lib.dylib", "build/test_lib/my_lib.dylib", "my_lib.dylib"};
#endif
#ifndef _WIN32
  for (const auto& path : paths) if (access(path.c_str(), F_OK) == 0) return path;
  return paths.front();
#endif
}

static std::string load_test_library(SimplePipeClient& client)
{
  json request;
  request["command"] = "load_library";
  request["request_id"] = "load-large";
  request["payload"]["path"] = get_test_library_path();
  if (!client.send_request(json_dump(request))) return "";
  json response = json_parse(client.receive_response());
  if (response["status"].asString() != "success") return "";
  return response["data"]["library_id"].asString();
}

TEST_F(MultiClientIntegrationTest, TransfersFiveMiBBufferAndKeepsFramingAligned)
{
  SimplePipeClient client(100);
  ASSERT_TRUE(client.connect(g_pipe_name));
  const std::string library_id = load_test_library(client);
  ASSERT_FALSE(library_id.empty());

  constexpr size_t data_size = 5U * 1024U * 1024U;
  std::string input(data_size, 'A');
  json request;
  request["command"] = "call_function";
  request["request_id"] = "large-buffer";
  request["payload"]["library_id"] = library_id;
  request["payload"]["function_name"] = "process_buffer_inout";
  request["payload"]["return_type"] = "int32";
  json args(Json::arrayValue);
  json buffer_arg;
  buffer_arg["type"] = "buffer";
  buffer_arg["direction"] = "inout";
  buffer_arg["size"] = static_cast<Json::UInt64>(data_size);
  buffer_arg["value"] = base64_encode(input);
  args.append(buffer_arg);
  json size_arg;
  size_arg["type"] = "pointer";
  size_arg["target_type"] = "int32";
  size_arg["direction"] = "inout";
  size_arg["value"] = static_cast<Json::Int>(data_size);
  args.append(size_arg);
  request["payload"]["args"] = args;

  ASSERT_TRUE(client.send_request(json_dump(request)));
  json response = json_parse(client.receive_response());
  ASSERT_EQ(response["status"].asString(), "success") << json_dump(response);
  std::string output = base64_decode(response["data"]["out_params"][0]["value"].asString());
  ASSERT_EQ(output.size(), data_size);
  EXPECT_EQ(static_cast<unsigned char>(output[0]), 0xAA);
  EXPECT_EQ(output[1], 'B');
  EXPECT_EQ(static_cast<unsigned char>(output[2]), 0xDE);
  EXPECT_EQ(static_cast<unsigned char>(output[3]), 0xAD);

  json follow_up;
  follow_up["command"] = "call_function";
  follow_up["request_id"] = "after-large";
  follow_up["payload"]["library_id"] = library_id;
  follow_up["payload"]["function_name"] = "add";
  follow_up["payload"]["return_type"] = "int32";
  json add_args(Json::arrayValue);
  json a; a["type"] = "int32"; a["value"] = 7; add_args.append(a);
  json b; b["type"] = "int32"; b["value"] = 8; add_args.append(b);
  follow_up["payload"]["args"] = add_args;
  ASSERT_TRUE(client.send_request(json_dump(follow_up)));
  json follow_response = json_parse(client.receive_response());
  EXPECT_EQ(follow_response["data"]["return"]["value"].asInt(), 15);
}

TEST_F(MultiClientIntegrationTest, HandlesFragmentedHeaderAndBody)
{
  SimplePipeClient client(101);
  ASSERT_TRUE(client.connect(g_pipe_name));
  json request;
  request["command"] = "unknown_fragmented_command";
  request["request_id"] = "fragmented";
  request["payload"]["padding"] = std::string(8192, 'x');
  ASSERT_TRUE(client.send_request_fragmented(json_dump(request), 1));
  json response = json_parse(client.receive_response());
  EXPECT_EQ(response["request_id"].asString(), "fragmented");
  EXPECT_EQ(response["status"].asString(), "error");
}

TEST_F(MultiClientIntegrationTest, RejectsOversizedFrameWithoutStoppingServer)
{
  {
    SimplePipeClient invalid_client(102);
    ASSERT_TRUE(invalid_client.connect(g_pipe_name));
    ASSERT_TRUE(invalid_client.send_length_only(kMaxIpcFrameSize + 1));
    EXPECT_TRUE(invalid_client.receive_response().empty());
  }

  SimplePipeClient healthy_client(103);
  ASSERT_TRUE(healthy_client.connect(g_pipe_name));
  json request;
  request["command"] = "unknown_after_oversize";
  request["request_id"] = "healthy";
  request["payload"] = Json::objectValue;
  ASSERT_TRUE(healthy_client.send_request(json_dump(request)));
  json response = json_parse(healthy_client.receive_response());
  EXPECT_EQ(response["request_id"].asString(), "healthy");
  EXPECT_EQ(response["status"].asString(), "error");
}

TEST_F(MultiClientIntegrationTest, TransfersFiveMiBCallbackEvent)
{
  SimplePipeClient client(104);
  ASSERT_TRUE(client.connect(g_pipe_name));
  const std::string library_id = load_test_library(client);
  ASSERT_FALSE(library_id.empty());

  json register_request;
  register_request["command"] = "register_callback";
  register_request["request_id"] = "register-large-callback";
  register_request["payload"]["return_type"] = "void";
  json callback_args(Json::arrayValue);
  callback_args.append("int32");
  json callback_buffer;
  callback_buffer["type"] = "buffer_ptr";
  callback_buffer["size_arg_index"] = 2;
  callback_args.append(callback_buffer);
  callback_args.append("int32");
  callback_args.append("pointer");
  register_request["payload"]["args_type"] = callback_args;
  ASSERT_TRUE(client.send_request(json_dump(register_request)));
  json register_response = json_parse(client.receive_response());
  const std::string callback_id = register_response["data"]["callback_id"].asString();
  ASSERT_FALSE(callback_id.empty());

  constexpr size_t data_size = 5U * 1024U * 1024U;
  std::string input(data_size, '\x5A');
  json call_request;
  call_request["command"] = "call_function";
  call_request["request_id"] = "large-callback";
  call_request["payload"]["library_id"] = library_id;
  call_request["payload"]["function_name"] = "trigger_buffer_callback";
  call_request["payload"]["return_type"] = "void";
  json call_args(Json::arrayValue);
  json cb; cb["type"] = "callback"; cb["value"] = callback_id; call_args.append(cb);
  json type; type["type"] = "int32"; type["value"] = 77; call_args.append(type);
  json data; data["type"] = "buffer"; data["direction"] = "in";
  data["size"] = static_cast<Json::UInt64>(data_size); data["value"] = base64_encode(input); call_args.append(data);
  json size; size["type"] = "int32"; size["value"] = static_cast<Json::Int>(data_size); call_args.append(size);
  json context; context["type"] = "pointer"; context["value"] = 1234; call_args.append(context);
  call_request["payload"]["args"] = call_args;
  ASSERT_TRUE(client.send_request(json_dump(call_request)));

  json event = json_parse(client.receive_response());
  ASSERT_EQ(event["event"].asString(), "invoke_callback") << json_dump(event);
  EXPECT_EQ(event["payload"]["args"][0]["value"].asInt(), 77);
  EXPECT_EQ(base64_decode(event["payload"]["args"][1]["value"].asString()), input);
  json response = json_parse(client.receive_response());
  EXPECT_EQ(response["request_id"].asString(), "large-callback");
  EXPECT_EQ(response["status"].asString(), "success");
}

TEST_F(MultiClientIntegrationTest, StopUnblocksIdleClientSession)
{
  SimplePipeClient client(105);
  ASSERT_TRUE(client.connect(g_pipe_name));
  const auto start = std::chrono::steady_clock::now();
  executor_->stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

TEST_F(MultiClientIntegrationTest, ClientDisconnectDuringLargeResponseDoesNotStopServer)
{
  {
    SimplePipeClient client(106);
    ASSERT_TRUE(client.connect(g_pipe_name));
    const std::string library_id = load_test_library(client);
    ASSERT_FALSE(library_id.empty());

    constexpr size_t data_size = 5U * 1024U * 1024U;
    json request;
    request["command"] = "call_function";
    request["request_id"] = "disconnect-large-response";
    request["payload"]["library_id"] = library_id;
    request["payload"]["function_name"] = "process_buffer_inout";
    request["payload"]["return_type"] = "int32";
    json args(Json::arrayValue);
    json buffer_arg;
    buffer_arg["type"] = "buffer";
    buffer_arg["direction"] = "out";
    buffer_arg["size"] = static_cast<Json::UInt64>(data_size);
    args.append(buffer_arg);
    json size_arg;
    size_arg["type"] = "pointer";
    size_arg["target_type"] = "int32";
    size_arg["direction"] = "inout";
    size_arg["value"] = static_cast<Json::Int>(data_size);
    args.append(size_arg);
    request["payload"]["args"] = args;
    ASSERT_TRUE(client.send_request(json_dump(request)));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  SimplePipeClient healthy_client(107);
  ASSERT_TRUE(healthy_client.connect(g_pipe_name));
  json request;
  request["command"] = "still-alive";
  request["request_id"] = "after-disconnect";
  request["payload"] = Json::objectValue;
  ASSERT_TRUE(healthy_client.send_request(json_dump(request)));
  json response = json_parse(healthy_client.receive_response());
  EXPECT_EQ(response["request_id"].asString(), "after-disconnect");
}
