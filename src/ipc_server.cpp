#include "ipc_server.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cstdint>
#include <queue>
#include <mutex>
#include <condition_variable>
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

class IpcServer::Impl {
public:
    virtual ~Impl() = default;
    virtual void start(const std::string& pipe_name, IpcServer::RequestHandler handler) = 0;
    virtual void sendEvent(const nlohmann::json& event_json) = 0;

protected:
    std::queue<std::string> outgoing_message_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::atomic<bool> running_{true}; // Initialize to true, managed by Executor
};

#ifdef _WIN32
class WindowsIpcServer : public IpcServer::Impl {
public:
    WindowsIpcServer() : client_pipe_(INVALID_HANDLE_VALUE) {}
    ~WindowsIpcServer() override {
        running_ = false;
        queue_cv_.notify_all();
        if (client_pipe_ != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(client_pipe_);
            CloseHandle(client_pipe_);
        }
    }

    void start(const std::string& pipe_name, IpcServer::RequestHandler handler) override {
        std::string full_pipe_name = "\\.\pipe\" + pipe_name;
        running_ = true;

        while (running_) {
            HANDLE hPipe = CreateNamedPipeA(full_pipe_name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
            if (hPipe == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("Failed to create named pipe.");
            }

            std::cout << "Waiting for client connection on " << full_pipe_name << "..." << std::endl;
            if (ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED)) {
                std::cout << "Client connected." << std::endl;
                {
                    std::lock_guard<std::mutex> lock(client_mutex_);
                    client_pipe_ = hPipe;
                }
                
                std::thread sender_thread(&WindowsIpcServer::send_loop, this);
                handle_connection(hPipe, handler); 

                {
                    std::lock_guard<std::mutex> lock(client_mutex_);
                    client_pipe_ = INVALID_HANDLE_VALUE;
                }
                queue_cv_.notify_all(); 
                sender_thread.join();
            }
            CloseHandle(hPipe);
        }
    }

    void sendEvent(const nlohmann::json& event_json) override {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        outgoing_message_queue_.push(event_json.dump());
        queue_cv_.notify_one();
    }

private:
    HANDLE client_pipe_;
    std::mutex client_mutex_;

    void send_loop() {
        while (true) {
            std::string message;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this] { return !outgoing_message_queue_.empty() || client_pipe_ == INVALID_HANDLE_VALUE; });
                if (client_pipe_ == INVALID_HANDLE_VALUE) break;
                message = outgoing_message_queue_.front();
                outgoing_message_queue_.pop();
            }

            uint32_t message_len = message.length();
            uint32_t net_message_len = htonl(message_len);
            DWORD bytesWritten;
            
            std::lock_guard<std::mutex> lock(client_mutex_);
            if (client_pipe_ == INVALID_HANDLE_VALUE) break;

            if (!WriteFile(client_pipe_, &net_message_len, sizeof(net_message_len), &bytesWritten, NULL) || !WriteFile(client_pipe_, message.c_str(), message_len, &bytesWritten, NULL)) {
                std::cerr << "Error writing to pipe. Client may have disconnected." << std::endl;
                break;
            }
        }
    }

    void handle_connection(HANDLE hPipe, const IpcServer::RequestHandler& handler) {
        while (true) {
            DWORD bytesRead;
            uint32_t net_msg_len;
            if (!ReadFile(hPipe, &net_msg_len, sizeof(net_msg_len), &bytesRead, NULL) || bytesRead == 0) {
                break;
            }

            uint32_t msg_len = ntohl(net_msg_len);
            std::vector<char> request_buf(msg_len);
            if (!ReadFile(hPipe, request_buf.data(), msg_len, &bytesRead, NULL) || bytesRead != msg_len) {
                break;
            }

            std::string request(request_buf.begin(), request_buf.end());
            std::string response = handler(request);

            uint32_t response_len = response.length();
            uint32_t net_response_len = htonl(response_len);
            DWORD bytesWritten;
            if (!WriteFile(hPipe, &net_response_len, sizeof(net_response_len), &bytesWritten, NULL) || !WriteFile(hPipe, response.c_str(), response_len, &bytesWritten, NULL)) {
                break;
            }
        }
        std::cout << "Client disconnected." << std::endl;
    }
};
#else
class UnixIpcServer : public IpcServer::Impl {
public:
    UnixIpcServer() : client_sock_(-1) {}
    ~UnixIpcServer() override {
        running_ = false;
        queue_cv_.notify_all();
        if (server_fd_ != -1) {
            close(server_fd_);
        }
        if (client_sock_ != -1) {
            close(client_sock_);
        }
    }

    void start(const std::string& socket_path, IpcServer::RequestHandler handler) override {
        struct sockaddr_un address;
        std::string actual_socket_path = "/tmp/" + socket_path;

        if ((server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0)) == 0) {
            throw std::runtime_error("Socket creation failed");
        }

        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, actual_socket_path.c_str(), sizeof(address.sun_path) - 1);
        unlink(actual_socket_path.c_str());

        if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
            throw std::runtime_error("Socket bind failed for path: " + actual_socket_path);
        }

        if (listen(server_fd_, 1) < 0) {
            throw std::runtime_error("Socket listen failed");
        }
        
        running_ = true;
        while (running_) {
            std::cout << "Waiting for client connection on " << actual_socket_path << "..." << std::endl;
            int new_socket;
            socklen_t addrlen = sizeof(address);
            if ((new_socket = accept(server_fd_, (struct sockaddr*)&address, &addrlen)) < 0) {
                if (!running_) break;
                std::cerr << "Socket accept failed." << std::endl;
                continue;
            }
            
            std::cout << "Client connected." << std::endl;
            {
                std::lock_guard<std::mutex> lock(client_mutex_);
                client_sock_ = new_socket;
            }

            std::thread sender_thread(&UnixIpcServer::send_loop, this);
            handle_connection(new_socket, handler); 

            {
                std::lock_guard<std::mutex> lock(client_mutex_);
                client_sock_ = -1;
            }
            queue_cv_.notify_all(); 
            sender_thread.join();
            close(new_socket);
        }
    }

    void sendEvent(const nlohmann::json& event_json) override {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        outgoing_message_queue_.push(event_json.dump());
        queue_cv_.notify_one();
    }

private:
    int server_fd_ = -1;
    int client_sock_;
    std::mutex client_mutex_;

    void send_loop() {
        while (true) {
            std::string message;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this] { return !outgoing_message_queue_.empty() || client_sock_ == -1; });
                if (client_sock_ == -1) break;
                message = outgoing_message_queue_.front();
                outgoing_message_queue_.pop();
            }

            uint32_t message_len = message.length();
            uint32_t net_message_len = htonl(message_len);

            std::lock_guard<std::mutex> lock(client_mutex_);
            if (client_sock_ == -1) break;

            if (send(client_sock_, &net_message_len, sizeof(net_message_len), 0) == -1 || send(client_sock_, message.c_str(), message_len, 0) == -1) {
                std::cerr << "Error writing to socket. Client may have disconnected." << std::endl;
                break;
            }
        }
    }

    void handle_connection(int sock, const IpcServer::RequestHandler& handler) {
        while (true) {
            uint32_t net_msg_len;
            ssize_t read_bytes = recv(sock, &net_msg_len, sizeof(net_msg_len), 0);
            if (read_bytes <= 0) {
                break;
            }

            uint32_t msg_len = ntohl(net_msg_len);
            std::vector<char> request_buf(msg_len);
            ssize_t total_read = 0;
            while (total_read < (ssize_t)msg_len) {
                ssize_t current_read = recv(sock, request_buf.data() + total_read, msg_len - total_read, 0);
                if (current_read <= 0) goto end_loop;
                total_read += current_read;
            }

            if (total_read == (ssize_t)msg_len) {
                std::string request(request_buf.begin(), request_buf.end());
                std::string response = handler(request);

                uint32_t response_len = response.length();
                uint32_t net_response_len = htonl(response_len);
                if (send(sock, &net_response_len, sizeof(net_response_len), 0) == -1 || send(sock, response.c_str(), response_len, 0) == -1) {
                    break;
                }
            } else {
                break;
            }
        }
    end_loop:
        std::cout << "Client disconnected." << std::endl;
    }
};
#endif

IpcServer::IpcServer() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        throw std::runtime_error("WSAStartup failed.");
    }
    pimpl = new WindowsIpcServer();
#else
    pimpl = new UnixIpcServer();
#endif
}

IpcServer::~IpcServer() {
    delete pimpl;
#ifdef _WIN32
    WSACleanup();
#endif
}

void IpcServer::start(const std::string& pipe_name, IpcServer::RequestHandler handler) {
    pimpl->start(pipe_name, handler);
}

void IpcServer::sendEvent(const nlohmann::json& event_json) {
    pimpl->sendEvent(event_json);
}