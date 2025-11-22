#include "gtest/gtest.h"
#include "executor.h"
#include <json/json.h>
#include <thread>
#include <vector>
#include <string>
#include <memory>
#include <future>
#include <chrono>
#include <iostream>
#include <atomic>

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
std::string json_dump(const json& j) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, j);
}

json json_parse(const std::string& s) {
    json j;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    if (!reader->parse(s.data(), s.data() + s.size(), &j, &errs)) {
        return json(); 
    }
    return j;
}

// --- Test Configuration ---
const std::string PIPE_NAME = "multi_client_test_pipe";
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
    std::string full_pipe_name = "\\.\pipe\" + name;
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
    std::cerr << "[Client " << client_id_ << "] Connection attempt timed out after 3 seconds."
;    return false;
#else
    socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd_ < 0) return false;

    struct sockaddr_un addr;
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
    uint32_t len = htonl(request.length());
#ifdef _WIN32
    DWORD bytesWritten;
    return WriteFile(pipe_handle_, &len, sizeof(len), &bytesWritten, NULL) &&
      WriteFile(pipe_handle_, request.c_str(), request.length(), &bytesWritten, NULL);
#else
    return send(socket_fd_, &len, sizeof(len), 0) != -1 &&
      send(socket_fd_, request.c_str(), request.length(), 0) != -1;
#endif
  }

  std::string receive_response()
  {
    uint32_t net_msg_len;
#ifdef _WIN32
    DWORD bytesRead;
    if (!ReadFile(pipe_handle_, &net_msg_len, sizeof(net_msg_len), &bytesRead, NULL) || bytesRead == 0) return "";
#else
    if (recv(socket_fd_, &net_msg_len, sizeof(net_msg_len), 0) <= 0) return "";
#endif

    uint32_t msg_len = ntohl(net_msg_len);
    if (msg_len == 0 || msg_len > 4096) return "";

    std::vector<char> buffer(msg_len);
#ifdef _WIN32
    if (!ReadFile(pipe_handle_, buffer.data(), msg_len, &bytesRead, NULL) || bytesRead != msg_len) return "";
#else
    ssize_t total_read = 0;
    while (total_read < (ssize_t)msg_len)
    {
      ssize_t current_read = recv(socket_fd_, buffer.data() + total_read, msg_len - total_read, 0);
      if (current_read <= 0) return "";
      total_read += current_read;
    }
#endif
    return std::string(buffer.begin(), buffer.end());
  }

private:
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
    {
      std::lock_guard<std::mutex> lock(g_test_log_mutex);
      std::cout << "[Test Main] SetUp: Starting executor thread..." << std::endl;
    }
    executor_ = std::make_unique<Executor>();
    executor_thread_ = std::thread([this]()
    {
      {
        std::lock_guard<std::mutex> lock(g_test_log_mutex);
        std::cout << "[Test Main] Executor thread " << std::this_thread::get_id() << " started. Calling run()."
;
      }
      executor_->run(PIPE_NAME);
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
  if (!client.connect(PIPE_NAME))
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
  { json arg; arg["type"] = "int32"; arg["value"] = a; args.append(arg); }
  { json arg; arg["type"] = "int32"; arg["value"] = b; args.append(arg); }
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
    std::cerr << "[Client " << client_id << "] Function call failed or returned wrong value: " << json_dump(call_resp) <<
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
  lib_path = "../../test_lib/" CMAKE_BUILD_TYPE "/my_lib.dll";
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