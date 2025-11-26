#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <future>
#include <atomic>
#include <filesystem>

#include "json/json.h"
#include "utils/base64.h" // Include Base64 utilities

#ifdef _WIN32
#include <windows.h>
#include <namedpipeapi.h>
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <queue>
#endif

namespace fs = std::filesystem;
using json = Json::Value;

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

class RpcClient
{
public:
#ifdef _WIN32
  using SocketType = HANDLE;
  const SocketType INVALID_SOCKET = INVALID_HANDLE_VALUE;
#else
  using SocketType = int;
  const SocketType INVALID_SOCKET = -1;
#endif

  RpcClient(const std::string& pipe_name) : pipe_name_(pipe_name), sock_(INVALID_SOCKET), request_id_counter_(0),
                                            running_(false)
  {
  }

  ~RpcClient()
  {
    disconnect();
  }

  void connect()
  {
#ifdef _WIN32
    std::string pipe_path = "\\\\.\\pipe\\" + pipe_name_;
    sock_ = CreateFileA(pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (sock_ == INVALID_SOCKET)
    {
      throw std::runtime_error("Failed to connect to named pipe: " + std::to_string(GetLastError()));
    }
#else
    std::string socket_path = "/tmp/" + pipe_name_;
    sock_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_ == INVALID_SOCKET)
    {
      throw std::runtime_error("Failed to create socket");
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) == -1)
    {
      close(sock_);
      sock_ = INVALID_SOCKET;
      throw std::runtime_error("Failed to connect to Unix domain socket: " + socket_path);
    }
#endif
    running_ = true;
    receiver_thread_ = std::thread(&RpcClient::receive_messages, this);
    std::cout << "Connected to " << pipe_name_ << std::endl;
  }

  void disconnect()
  {
    running_ = false;
    if (sock_ != INVALID_SOCKET)
    {
#ifdef _WIN32
      CloseHandle(sock_);
#else
      shutdown(sock_, SHUT_RDWR);
      close(sock_);
#endif
      sock_ = INVALID_SOCKET;
    }
    if (receiver_thread_.joinable())
    {
      receiver_thread_.join();
    }
    // Notify any waiting get_event calls that the client has disconnected
    event_cond_.notify_all();
    std::cout << "Connection closed." << std::endl;
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

    std::string request_str = json_dump(request);
    uint32_t length = request_str.length();
    uint32_t network_order_length = htonl(length);

    std::lock_guard<std::mutex> send_lock(send_mutex_);
#ifdef _WIN32
    DWORD bytes_written;
    WriteFile(sock_, &network_order_length, 4, &bytes_written, NULL);
    WriteFile(sock_, request_str.c_str(), length, &bytes_written, NULL);
#else
    write(sock_, &network_order_length, 4);
    write(sock_, request_str.c_str(), length);
#endif

    std::cout << "--> Sending Request [" << request["command"].asString() << "] id=" << req_id << std::endl;

    // Wait for the response
    if (future.wait_for(std::chrono::seconds(10)) == std::future_status::timeout)
    {
      throw std::runtime_error("Timeout waiting for response for request ID " + req_id);
    }

    json response = future.get();
    std::cout << "<-- Received Response for id=" << req_id << ": " << json_dump(response) << std::endl;
    return response;
  }

  json get_event(std::chrono::seconds timeout = std::chrono::seconds(5))
  {
    std::unique_lock<std::mutex> lock(event_queue_mutex_);
    if (event_cond_.wait_for(lock, timeout, [this] { return !event_queue_.empty() || !running_; }))
    {
      if (!running_ && event_queue_.empty())
      {
        throw std::runtime_error("Client disconnected while waiting for event.");
      }
      json event = event_queue_.front();
      event_queue_.pop();
      return event;
    }
    throw std::runtime_error("Timeout waiting for event.");
  }

  bool has_events()
  {
    std::lock_guard<std::mutex> lock(event_queue_mutex_);
    return !event_queue_.empty();
  }

  void clear_events()
  {
    std::lock_guard<std::mutex> lock(event_queue_mutex_);
    while (!event_queue_.empty())
    {
      event_queue_.pop();
    }
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
        if (running_) std::cerr << "Executor disconnected." << std::endl;
        break;
      }
#else
      ssize_t read_bytes = read(sock_, &network_order_response_length, 4);
      if (read_bytes <= 0)
      {
        if (running_) std::cerr << "Executor disconnected." << std::endl;
        break;
      }
#endif
      uint32_t response_length = ntohl(network_order_response_length);
      std::vector<char> response_buffer(response_length);
#ifdef _WIN32
      if (!ReadFile(sock_, response_buffer.data(), response_length, &bytes_read, NULL) || bytes_read == 0)
      {
        if (running_) std::cerr << "Executor disconnected during read." << std::endl;
        break;
      }
#else
      read_bytes = read(sock_, response_buffer.data(), response_length);
      if (read_bytes <= 0)
      {
        if (running_) std::cerr << "Executor disconnected during read." << std::endl;
        break;
      }
#endif
      std::string response_str(response_buffer.begin(), response_buffer.end());
      json response = json_parse(response_str);

      if (response.isMember("request_id"))
      {
        std::string req_id = response["request_id"].asString();
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
      else if (response.isMember("event"))
      {
        std::cout << "<-- Received Event [" << response["event"].asString() << "]: " << json_dump(response) <<
          std::endl;
        {
          std::lock_guard<std::mutex> lock(event_queue_mutex_);
          event_queue_.push(response);
        }
        event_cond_.notify_one();
      }
    }
    // Notify any waiting get_event calls that the client has disconnected
    event_cond_.notify_all();
  }

  std::string pipe_name_;
  SocketType sock_;
  std::atomic<int> request_id_counter_;
  std::thread receiver_thread_;
  std::atomic<bool> running_;
  std::mutex send_mutex_;

  std::map<std::string, std::shared_ptr<std::promise<json>>> pending_requests_;
  std::mutex pending_requests_mutex_;

  std::queue<json> event_queue_;
  std::mutex event_queue_mutex_;
  std::condition_variable event_cond_;
};

void run_test(const std::string& name, const std::function<void(RpcClient&)>& test_func, RpcClient& client)
{
  std::cout << "\n--- Running Test: " << name << " ---" << std::endl;
  try
  {
    test_func(client);
    std::cout << "--- Test '" << name << "' PASSED ---" << std::endl;
  }
  catch (const std::exception& e)
  {
    std::cerr << "--- Test '" << name << "' FAILED: " << e.what() << " ---" << std::endl;
    throw;
  }
}

int main(int argc, char* argv[])
{
  if (argc < 2)
  {
    std::cerr << "Usage: " << argv[0] << " <pipe_name>" << std::endl;
    return 1;
  }

  try
  {
    RpcClient client(argv[1]);
    client.connect();

    std::string library_id;

    run_test("Register Point Struct", [&](RpcClient& c)
    {
      json req;
      req["command"] = "register_struct";

      json p1, p2;
      p1["name"] = "x";
      p1["type"] = "int32";
      p2["name"] = "y";
      p2["type"] = "int32";
      json def(Json::arrayValue);
      def.append(p1);
      def.append(p2);

      req["payload"]["struct_name"] = "Point";
      req["payload"]["definition"] = def;

      json res = c.send_request(req);
      if (res["status"].asString() != "success") throw std::runtime_error("Failed to register struct");
    }, client);

    run_test("Load Library", [&](RpcClient& c)
    {
      fs::path lib_path = fs::current_path() / "build" / "test_lib";
#ifdef _WIN32
      lib_path /= "my_lib.dll";
#elif __APPLE__
      lib_path /= "my_lib.dylib";
#else
      lib_path /= "my_lib.so";
#endif
      json req;
      req["command"] = "load_library";
      req["payload"]["path"] = lib_path.string();

      json res = c.send_request(req);
      if (res["status"].asString() != "success") throw std::runtime_error("Failed to load library");
      library_id = res["data"]["library_id"].asString();
    }, client);

    run_test("Add Function", [&](RpcClient& c)
    {
      json req;
      req["command"] = "call_function";
      req["payload"]["library_id"] = library_id;
      req["payload"]["function_name"] = "add";
      req["payload"]["return_type"] = "int32";

      json args(Json::arrayValue);
      json a1;
      a1["type"] = "int32";
      a1["value"] = 10;
      args.append(a1);
      json a2;
      a2["type"] = "int32";
      a2["value"] = 20;
      args.append(a2);
      req["payload"]["args"] = args;

      json res = c.send_request(req);
      if (res["data"]["return"]["value"].asInt() != 30) throw std::runtime_error("Add function failed");
    }, client);

    run_test("Callback Functionality", [&](RpcClient& c)
    {
      c.clear_events();

      json reg_req;
      reg_req["command"] = "register_callback";
      reg_req["payload"]["return_type"] = "void";
      json args_type(Json::arrayValue);
      args_type.append("string");
      args_type.append("int32");
      reg_req["payload"]["args_type"] = args_type;

      json reg_res = c.send_request(reg_req);
      std::string callback_id = reg_res["data"]["callback_id"].asString();
      std::cout << "Callback registered with ID: " << callback_id << std::endl;

      json call_req;
      call_req["command"] = "call_function";
      call_req["payload"]["library_id"] = library_id;
      call_req["payload"]["function_name"] = "call_my_callback";
      call_req["payload"]["return_type"] = "void";

      json args(Json::arrayValue);
      json a1;
      a1["type"] = "callback";
      a1["value"] = callback_id;
      args.append(a1);
      json a2;
      a2["type"] = "string";
      a2["value"] = "Hello from C++!";
      args.append(a2);
      call_req["payload"]["args"] = args;

      c.send_request(call_req);
      std::cout << "call_my_callback returned successfully, expecting one event..." << std::endl;

      json event = c.get_event();
      if (event["event"].asString() != "invoke_callback" || event["payload"]["callback_id"].asString() != callback_id)
      {
        throw std::runtime_error("Did not receive expected callback event.");
      }
      if (event["payload"]["args"][0]["value"].asString() != "Hello from C++!" || event["payload"]["args"][1]["value"].
        asInt() != 123)
      {
        throw std::runtime_error("Callback event arguments mismatch.");
      }
      std::cout << "Successfully received and verified invoke_callback event." << std::endl;

      json unreg_req;
      unreg_req["command"] = "unregister_callback";
      unreg_req["payload"]["callback_id"] = callback_id;
      json unreg_res = c.send_request(unreg_req);
      if (unreg_res["status"].asString() != "success") throw std::runtime_error("Failed to unregister callback");
      std::cout << "Callback unregistered successfully." << std::endl;
    }, client);

    run_test("Multi-Callback Functionality", [&](RpcClient& c)
    {
      c.clear_events(); // Clear any stale events

      // 1. 构造 register_callback 请求
      Json::Value reg_req;
      reg_req["command"] = "register_callback";
      reg_req["payload"]["return_type"] = "void";

      Json::Value args_type(Json::arrayValue);
      args_type.append("string");
      args_type.append("int32");
      reg_req["payload"]["args_type"] = args_type;

      // 发送请求并解析 ID
      Json::Value reg_res = c.send_request(reg_req);
      std::string multi_callback_id = reg_res["data"]["callback_id"].asString();
      std::cout << "Multi-Callback registered with ID: " << multi_callback_id << std::endl;

      int num_calls = 3;

      // 2. 构造 call_function 请求
      Json::Value call_req;
      call_req["command"] = "call_function";

      Json::Value payload;
      payload["library_id"] = library_id;
      payload["function_name"] = "call_multi_callbacks";
      payload["return_type"] = "void";

      Json::Value arg1;
      arg1["type"] = "callback";
      arg1["value"] = multi_callback_id;

      Json::Value arg2;
      arg2["type"] = "int32";
      arg2["value"] = num_calls;

      Json::Value args(Json::arrayValue);
      args.append(arg1);
      args.append(arg2);

      payload["args"] = args;
      call_req["payload"] = payload;

      c.send_request(call_req);
      std::cout << "call_multi_callbacks returned successfully, expecting " << num_calls << " events..." << std::endl;

      // 3. 循环接收并验证事件
      for (int i = 0; i < num_calls; ++i)
      {
        Json::Value event = c.get_event();

        // 验证事件类型和ID
        if (event["event"].asString() != "invoke_callback" || event["payload"]["callback_id"].asString() !=
          multi_callback_id)
        {
          throw std::runtime_error("Did not receive expected multi-callback event for call " + std::to_string(i + 1));
        }

        std::string expected_message = "Message from native code, call " + std::to_string(i + 1);
        int expected_value = i + 1;

        // 验证参数值 (注意：使用索引 [0] 和 [1])
        if (event["payload"]["args"][0]["value"].asString() != expected_message ||
          event["payload"]["args"][1]["value"].asInt() != expected_value)
        {
          throw std::runtime_error("Multi-callback event arguments mismatch for call " + std::to_string(i + 1));
        }

        std::cout << "  Received and verified multi-callback event " << i + 1 << "/" << num_calls
          << ": msg='" << event["payload"]["args"][0]["value"].asString()
          << "', val=" << event["payload"]["args"][1]["value"].asInt() << std::endl;
      }

      // 4. 构造 unregister_callback 请求
      Json::Value unreg_req;
      unreg_req["command"] = "unregister_callback";
      unreg_req["payload"]["callback_id"] = multi_callback_id;

      Json::Value unreg_res = c.send_request(unreg_req);
      if (unreg_res["status"].asString() != "success") throw std::runtime_error("Failed to unregister multi-callback");
      std::cout << "Multi-Callback unregistered successfully." << std::endl;
    }, client);

    run_test("Process Buffer Inout Functionality", [&](RpcClient& c)
    {
      int buffer_capacity = 64;
      std::string input_raw_data_str = "\x05"; // Input byte 0x05
      std::string input_base64 = base64_encode(reinterpret_cast<const unsigned char*>(input_raw_data_str.data()),
                                               input_raw_data_str.length());

      std::string expected_raw_output_prefix_str = "\xAA\x06\xDE\xAD"; // Expected output prefix

      json req;
      req["command"] = "call_function";
      req["payload"]["library_id"] = library_id;
      req["payload"]["function_name"] = "process_buffer_inout";
      req["payload"]["return_type"] = "int32";

      json args(Json::arrayValue);
      json a1;
      a1["type"] = "buffer";
      a1["direction"] = "inout";
      a1["size"] = buffer_capacity;
      a1["value"] = input_base64;
      args.append(a1);
      json a2;
      a2["type"] = "pointer";
      a2["target_type"] = "int32";
      a2["direction"] = "inout";
      a2["value"] = buffer_capacity;
      args.append(a2);
      req["payload"]["args"] = args;

      json res = c.send_request(req);
      if (res["status"].asString() != "success") throw std::runtime_error("process_buffer_inout call failed");

      int return_code = res["data"]["return"]["value"].asInt();
      if (return_code != 0) throw std::runtime_error("process_buffer_inout returned non-zero status");

      json out_params = res["data"]["out_params"];
      std::string output_base64_value;
      int updated_size = -1;
      for (const auto& param : out_params)
      {
        if (param["index"].asInt() == 0) output_base64_value = param["value"].asString();
        if (param["index"].asInt() == 1) updated_size = param["value"].asInt();
      }

      if (output_base64_value.empty()) throw std::runtime_error("Output buffer not received or empty");
      if (updated_size == -1) throw std::runtime_error("Updated size not received");

      // Decode the base64 output and verify its content
      std::string decoded_output_bytes = base64_decode(output_base64_value);

      if (decoded_output_bytes.length() != buffer_capacity) throw std::runtime_error("Decoded buffer length mismatch");
      if (decoded_output_bytes.substr(0, expected_raw_output_prefix_str.length()) != expected_raw_output_prefix_str)
      {
        throw std::runtime_error("Decoded buffer prefix mismatch.");
      }
      if (updated_size != expected_raw_output_prefix_str.length()) throw std::runtime_error("Updated size mismatch");

      std::cout << "Buffer content verified (prefix: " << expected_raw_output_prefix_str << ", Size: " << updated_size
        << ")" << std::endl;
    }, client);

    run_test("Dynamic Buffer Callback Functionality", [&](RpcClient& c)
    {
      c.clear_events();

      // typedef void(*ReadCallback)(int type, unsigned char data[], int size, void *that);
      // Mapped to: int32, buffer_ptr(size_index=2), int32, pointer
      
      json reg_req;
      reg_req["command"] = "register_callback";
      reg_req["payload"]["return_type"] = "void";
      
      json args_type(Json::arrayValue);
      args_type.append("int32");
      
      json buffer_arg;
      buffer_arg["type"] = "buffer_ptr";
      buffer_arg["size_arg_index"] = 2;
      args_type.append(buffer_arg);
      
      args_type.append("int32");
      args_type.append("pointer");
      
      reg_req["payload"]["args_type"] = args_type;

      json reg_res = c.send_request(reg_req);
      std::string callback_id = reg_res["data"]["callback_id"].asString();
      std::cout << "Dynamic Buffer Callback registered with ID: " << callback_id << std::endl;

      json call_req;
      call_req["command"] = "call_function";
      call_req["payload"]["library_id"] = library_id;
      call_req["payload"]["function_name"] = "trigger_read_callback";
      call_req["payload"]["return_type"] = "void";

      json args(Json::arrayValue);
      // 1. callback
      { json a; a["type"] = "callback"; a["value"] = callback_id; args.append(a); }
      // 2. int type
      { json a; a["type"] = "int32"; a["value"] = 99; args.append(a); }
      // 3. data (string to be treated as binary)
      { json a; a["type"] = "string"; a["value"] = "DynamicData123"; args.append(a); }
      // 4. context
      { json a; a["type"] = "pointer"; a["value"] = (Json::UInt64)0x1234; args.append(a); }
      
      call_req["payload"]["args"] = args;

      c.send_request(call_req);
      std::cout << "trigger_read_callback called, waiting for event..." << std::endl;

      json event = c.get_event();
      if (event["event"].asString() != "invoke_callback") throw std::runtime_error("Unexpected event");
      
      json cb_args = event["payload"]["args"];
      if (cb_args[0]["value"].asInt() != 99) throw std::runtime_error("Arg 0 mismatch");
      
      // Arg 1 should be buffer_ptr
      if (cb_args[1]["type"].asString() != "buffer_ptr") throw std::runtime_error("Arg 1 type mismatch");
      std::string b64_data = cb_args[1]["value"].asString();
      std::string decoded = base64_decode(b64_data);
      if (decoded != "DynamicData123") throw std::runtime_error("Arg 1 data mismatch: " + decoded);
      
      if (cb_args[2]["value"].asInt() != 14) throw std::runtime_error("Arg 2 size mismatch");
      
      std::cout << "Dynamic Buffer Callback Verified. Data: " << decoded << std::endl;
    }, client);

    run_test("Fixed Buffer Callback Functionality", [&](RpcClient& c)
    {
      c.clear_events();

      // typedef void(*FixedReadCallback)(unsigned char data[], void *that);
      // Mapped to: buffer_ptr(fixed_size=4), pointer
      
      json reg_req;
      reg_req["command"] = "register_callback";
      reg_req["payload"]["return_type"] = "void";
      
      json args_type(Json::arrayValue);
      
      json buffer_arg;
      buffer_arg["type"] = "buffer_ptr";
      buffer_arg["fixed_size"] = 4;
      args_type.append(buffer_arg);
      
      args_type.append("pointer");
      
      reg_req["payload"]["args_type"] = args_type;

      json reg_res = c.send_request(reg_req);
      std::string callback_id = reg_res["data"]["callback_id"].asString();
      std::cout << "Fixed Buffer Callback registered with ID: " << callback_id << std::endl;

      json call_req;
      call_req["command"] = "call_function";
      call_req["payload"]["library_id"] = library_id;
      call_req["payload"]["function_name"] = "trigger_fixed_read_callback";
      call_req["payload"]["return_type"] = "void";

      json args(Json::arrayValue);
      // 1. callback
      { json a; a["type"] = "callback"; a["value"] = callback_id; args.append(a); }
      // 2. context
      { json a; a["type"] = "pointer"; a["value"] = (Json::UInt64)0x5678; args.append(a); }
      
      call_req["payload"]["args"] = args;

      c.send_request(call_req);
      std::cout << "trigger_fixed_read_callback called, waiting for event..." << std::endl;

      json event = c.get_event();
      if (event["event"].asString() != "invoke_callback") throw std::runtime_error("Unexpected event");
      
      json cb_args = event["payload"]["args"];
      
      // Arg 0 should be buffer_ptr (fixed size 4)
      if (cb_args[0]["type"].asString() != "buffer_ptr") throw std::runtime_error("Arg 0 type mismatch");
      if (cb_args[0]["size"].asInt() != 4) throw std::runtime_error("Arg 0 size mismatch");
      
      std::string b64_data = cb_args[0]["value"].asString();
      std::string decoded = base64_decode(b64_data);
      
      // Expected: 0xDE, 0xAD, 0xBE, 0xEF
      if (decoded.size() != 4 || 
          (unsigned char)decoded[0] != 0xDE || 
          (unsigned char)decoded[1] != 0xAD || 
          (unsigned char)decoded[2] != 0xBE || 
          (unsigned char)decoded[3] != 0xEF) {
             throw std::runtime_error("Arg 0 data mismatch");
      }
      
      std::cout << "Fixed Buffer Callback Verified. Data size: " << decoded.size() << std::endl;
    }, client);
  }
  catch (const std::exception& e)
  {
    std::cerr << "An error occurred: " << e.what() << std::endl;
  }

  return 0;
}
