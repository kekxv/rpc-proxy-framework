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

#include "nlohmann/json.hpp"

#ifdef _WIN32
#include <windows.h>
#include <namedpipeapi.h>
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

class RpcClient {
public:
#ifdef _WIN32
    using SocketType = HANDLE;
    const SocketType INVALID_SOCKET = INVALID_HANDLE_VALUE;
#else
    using SocketType = int;
    const SocketType INVALID_SOCKET = -1;
#endif

    RpcClient(const std::string& pipe_name) : pipe_name_(pipe_name), sock_(INVALID_SOCKET), request_id_counter_(0), running_(false) {}

    ~RpcClient() {
        disconnect();
    }

    void connect() {
#ifdef _WIN32
        std::string pipe_path = "\\\\.\\pipe\\" + pipe_name_;
        sock_ = CreateFileA(pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (sock_ == INVALID_SOCKET) {
            throw std::runtime_error("Failed to connect to named pipe: " + std::to_string(GetLastError()));
        }
#else
        std::string socket_path = "/tmp/" + pipe_name_;
        sock_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_ == INVALID_SOCKET) {
            throw std::runtime_error("Failed to create socket");
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            close(sock_);
            sock_ = INVALID_SOCKET;
            throw std::runtime_error("Failed to connect to Unix domain socket: " + socket_path);
        }
#endif
        running_ = true;
        receiver_thread_ = std::thread(&RpcClient::receive_messages, this);
        std::cout << "Connected to " << pipe_name_ << std::endl;
    }

    void disconnect() {
        running_ = false;
        if (sock_ != INVALID_SOCKET) {
#ifdef _WIN32
            CloseHandle(sock_);
#else
            shutdown(sock_, SHUT_RDWR);
            close(sock_);
#endif
            sock_ = INVALID_SOCKET;
        }
        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
        std::cout << "Connection closed." << std::endl;
    }

    json send_request(const json& request_payload) {
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
        WriteFile(sock_, &network_order_length, 4, &bytes_written, NULL);
        WriteFile(sock_, request_str.c_str(), length, &bytes_written, NULL);
#else
        write(sock_, &network_order_length, 4);
        write(sock_, request_str.c_str(), length);
#endif
        
        std::cout << "--> Sending Request [" << request["command"] << "] id=" << req_id << std::endl;

        // Wait for the response
        if (future.wait_for(std::chrono::seconds(10)) == std::future_status::timeout) {
            throw std::runtime_error("Timeout waiting for response for request ID " + req_id);
        }
        
        json response = future.get();
        std::cout << "<-- Received Response for id=" << req_id << ": " << response.dump() << std::endl;
        return response;
    }

private:
    void receive_messages() {
        while (running_) {
            uint32_t network_order_response_length;
#ifdef _WIN32
            DWORD bytes_read;
            if (!ReadFile(sock_, &network_order_response_length, 4, &bytes_read, NULL) || bytes_read == 0) {
                if (running_) std::cerr << "Executor disconnected." << std::endl;
                break;
            }
#else
            ssize_t read_bytes = read(sock_, &network_order_response_length, 4);
            if (read_bytes <= 0) {
                if (running_) std::cerr << "Executor disconnected." << std::endl;
                break;
            }
#endif
            uint32_t response_length = ntohl(network_order_response_length);
            std::vector<char> response_buffer(response_length);
#ifdef _WIN32
            if (!ReadFile(sock_, response_buffer.data(), response_length, &bytes_read, NULL) || bytes_read == 0) {
                if (running_) std::cerr << "Executor disconnected during read." << std::endl;
                break;
            }
#else
            read_bytes = read(sock_, response_buffer.data(), response_length);
            if (read_bytes <= 0) {
                if (running_) std::cerr << "Executor disconnected during read." << std::endl;
                break;
            }
#endif
            std::string response_str(response_buffer.begin(), response_buffer.end());
            json response = json::parse(response_str);

            if (response.contains("request_id")) {
                std::string req_id = response["request_id"];
                std::shared_ptr<std::promise<json>> promise;
                {
                    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
                    auto it = pending_requests_.find(req_id);
                    if (it != pending_requests_.end()) {
                        promise = it->second;
                        pending_requests_.erase(it);
                    }
                }
                if (promise) {
                    promise->set_value(response);
                }
            } else if (response.contains("event")) {
                std::cout << "<-- Received Event [" << response["event"] << "]: " << response.dump() << std::endl;
            }
        }
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

void run_test(const std::string& name, const std::function<void()>& test_func) {
    std::cout << "\n--- Running Test: " << name << " ---" << std::endl;
    try {
        test_func();
        std::cout << "--- Test '" << name << "' PASSED ---" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "--- Test '" << name << "' FAILED: " << e.what() << " ---" << std::endl;
        throw;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <pipe_name>" << std::endl;
        return 1;
    }

    try {
        RpcClient client(argv[1]);
        client.connect();

        std::string library_id;

        run_test("Register Point Struct", [&]() {
            json req = {{"command", "register_struct"}, {"payload", {{"struct_name", "Point"}, {"definition", {{{"name", "x"}, {"type", "int32"}}, {{"name", "y"}, {"type", "int32"}}}}}}};
            json res = client.send_request(req);
            if (res["status"] != "success") throw std::runtime_error("Failed to register struct");
        });

        run_test("Load Library", [&]() {
            fs::path lib_path = fs::current_path() / "build" / "test_lib";
            #ifdef _WIN32
                lib_path /= "my_lib.dll";
            #elif __APPLE__
                lib_path /= "my_lib.dylib";
            #else
                lib_path /= "my_lib.so";
            #endif
            json req = {{"command", "load_library"}, {"payload", {{"path", lib_path.string()}}}};
            json res = client.send_request(req);
            if (res["status"] != "success") throw std::runtime_error("Failed to load library");
            library_id = res["data"]["library_id"].get<std::string>();
        });

        run_test("Add Function", [&]() {
            json req = {{"command", "call_function"}, {"payload", {{"library_id", library_id}, {"function_name", "add"}, {"return_type", "int32"}, {"args", {{{"type", "int32"}, {"value", 10}}, {{"type", "int32"}, {"value", 20}}}}}}};
            json res = client.send_request(req);
            if (res["data"]["return"]["value"] != 30) throw std::runtime_error("Add function failed");
        });

        run_test("Callback Functionality", [&]() {
            json reg_req = {{"command", "register_callback"}, {"payload", {{"return_type", "void"}, {"args_type", {"string", "int32"}}}}};
            json reg_res = client.send_request(reg_req);
            std::string callback_id = reg_res["data"]["callback_id"].get<std::string>();

            json call_req = {{"command", "call_function"}, {"payload", {{"library_id", library_id}, {"function_name", "call_my_callback"}, {"return_type", "void"}, {"args", {{{"type", "callback"}, {"value", callback_id}}, {{"type", "string"}, {"value", "Hello from C++!"}}}}}}};
            client.send_request(call_req);
            // In this simple example, we just check the console output for the event.
            // A more robust client would use a queue.
            std::this_thread::sleep_for(std::chrono::seconds(1));
        });

        run_test("Write Out Buff Functionality", [&]() {
            int buffer_capacity = 64;
            json req = {{"command", "call_function"}, {"payload", {
                {"library_id", library_id},
                {"function_name", "writeOutBuff"},
                {"return_type", "int32"},
                {"args", {
                    {{"type", "buffer"}, {"direction", "out"}, {"size", buffer_capacity}},
                    {{"type", "pointer"}, {"target_type", "int32"}, {"direction", "inout"}, {"value", buffer_capacity}}
                }}
            }}};
            json res = client.send_request(req);
            if (res["status"] != "success") throw std::runtime_error("writeOutBuff call failed");
            
            int return_code = res["data"]["return"]["value"].get<int>();
            if (return_code != 0) throw std::runtime_error("writeOutBuff returned non-zero status");

            json out_params = res["data"]["out_params"];
            std::string buffer_content;
            int updated_size = -1;
            for(const auto& param : out_params) {
                if (param["index"] == 0) buffer_content = param["value"].get<std::string>();
                if (param["index"] == 1) updated_size = param["value"].get<int>();
            }
            
            std::string expected_string = "Hello from writeOutBuff!";
            if (buffer_content != expected_string) throw std::runtime_error("Buffer content mismatch");
            if (updated_size != expected_string.length()) throw std::runtime_error("Updated size mismatch");
        });

    } catch (const std::exception& e) {
        std::cerr << "An error occurred: " << e.what() << std::endl;
    }

    return 0;
}
