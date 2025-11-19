#include "ipc_server.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstdint>
#include <mutex>
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

using json = nlohmann::json;

// --- Platform-specific Connection Implementations ---

#ifdef _WIN32
class WindowsConnection : public ClientConnection {
public:
    explicit WindowsConnection(HANDLE pipe) : pipe_(pipe), is_open_(true) {}
    ~WindowsConnection() override {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(pipe_);
            CloseHandle(pipe_);
        }
    }

    std::string read() override {
        DWORD bytesRead;
        uint32_t net_msg_len;
        if (!ReadFile(pipe_, &net_msg_len, sizeof(net_msg_len), &bytesRead, NULL) || bytesRead == 0) {
            is_open_ = false;
            return "";
        }

        uint32_t msg_len = ntohl(net_msg_len);
        std::vector<char> buffer(msg_len);
        if (!ReadFile(pipe_, buffer.data(), msg_len, &bytesRead, NULL) || bytesRead != msg_len) {
            is_open_ = false;
            return "";
        }
        return std::string(buffer.begin(), buffer.end());
    }

    bool write(const std::string& message) override {
        std::lock_guard<std::mutex> lock(write_mutex_);
        uint32_t len = message.length();
        uint32_t net_len = htonl(len);
        DWORD bytesWritten;
        if (!WriteFile(pipe_, &net_len, sizeof(net_len), &bytesWritten, NULL) ||
            !WriteFile(pipe_, message.c_str(), len, &bytesWritten, NULL)) {
            is_open_ = false;
            return false;
        }
        return true;
    }

    bool sendEvent(const json& event_json) override {
        return write(event_json.dump());
    }

    bool isOpen() override { return is_open_; }

private:
    HANDLE pipe_;
    std::atomic<bool> is_open_;
    std::mutex write_mutex_;
};

#else
class UnixConnection : public ClientConnection {
public:
    explicit UnixConnection(int sock) : sock_(sock), is_open_(true) {}
    ~UnixConnection() override {
        if (sock_ != -1) {
            close(sock_);
        }
    }

    std::string read() override {
        uint32_t net_msg_len;
        ssize_t read_bytes = recv(sock_, &net_msg_len, sizeof(net_msg_len), 0);
        if (read_bytes <= 0) {
            is_open_ = false;
            return "";
        }

        uint32_t msg_len = ntohl(net_msg_len);
        std::vector<char> buffer(msg_len);
        ssize_t total_read = 0;
        while (total_read < (ssize_t)msg_len) {
            ssize_t current_read = recv(sock_, buffer.data() + total_read, msg_len - total_read, 0);
            if (current_read <= 0) {
                is_open_ = false;
                return "";
            }
            total_read += current_read;
        }
        return std::string(buffer.begin(), buffer.end());
    }

    bool write(const std::string& message) override {
        std::lock_guard<std::mutex> lock(write_mutex_);
        uint32_t len = message.length();
        uint32_t net_len = htonl(len);
        if (send(sock_, &net_len, sizeof(net_len), 0) == -1 ||
            send(sock_, message.c_str(), len, 0) == -1) {
            is_open_ = false;
            return false;
        }
        return true;
    }

    bool sendEvent(const json& event_json) override {
        return write(event_json.dump());
    }

    bool isOpen() override { return is_open_; }

private:
    int sock_;
    std::atomic<bool> is_open_;
    std::mutex write_mutex_;
};
#endif

// --- Platform-specific Server Implementations ---

#ifdef _WIN32
class WindowsIpcServer : public IpcServer {
public:
    WindowsIpcServer() {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("WSAStartup failed.");
        }
    }
    ~WindowsIpcServer() override {
        stop();
        WSACleanup();
    }

    void listen(const std::string& name) override {
        pipe_name_ = "\\\\.\\pipe\\" + name;
    }

    std::unique_ptr<ClientConnection> accept() override {
        HANDLE hPipe = CreateNamedPipeA(pipe_name_.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
        if (hPipe == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to create named pipe for listening.");
        }
        listener_pipe_ = hPipe;

        std::cout << "Waiting for client connection on " << pipe_name_ << "..." << std::endl;
        if (ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED)) {
            std::cout << "Client connected." << std::endl;
            listener_pipe_ = INVALID_HANDLE_VALUE;
            return std::make_unique<WindowsConnection>(hPipe);
        }
        
        CloseHandle(hPipe);
        listener_pipe_ = INVALID_HANDLE_VALUE;
        return nullptr;
    }

    void stop() override {
        if (listener_pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(listener_pipe_);
            listener_pipe_ = INVALID_HANDLE_VALUE;
        }
    }
private:
    std::string pipe_name_;
    HANDLE listener_pipe_ = INVALID_HANDLE_VALUE;
};
#else
class UnixIpcServer : public IpcServer {
public:
    UnixIpcServer() = default;
    ~UnixIpcServer() override { stop(); }

    void listen(const std::string& name) override {
        socket_path_ = "/tmp/" + name;
        if ((server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0)) == 0) {
            throw std::runtime_error("Socket creation failed");
        }
        
        struct sockaddr_un address;
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, socket_path_.c_str(), sizeof(address.sun_path) - 1);
        unlink(socket_path_.c_str());

        if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
            throw std::runtime_error("Socket bind failed for path: " + socket_path_);
        }

        if (::listen(server_fd_, 5) < 0) {
            throw std::runtime_error("Socket listen failed");
        }
    }

    std::unique_ptr<ClientConnection> accept() override {
        std::cout << "Waiting for client connection on " << socket_path_ << "..." << std::endl;
        int new_socket = ::accept(server_fd_, NULL, NULL);
        if (new_socket < 0) {
            return nullptr;
        }
        std::cout << "Client connected." << std::endl;
        return std::make_unique<UnixConnection>(new_socket);
    }

    void stop() override {
        if (server_fd_ != -1) {
            close(server_fd_);
            unlink(socket_path_.c_str());
            server_fd_ = -1;
        }
    }
private:
    int server_fd_ = -1;
    std::string socket_path_;
};
#endif

// --- IpcServer Factory Method ---

std::unique_ptr<IpcServer> IpcServer::create() {
#ifdef _WIN32
    return std::make_unique<WindowsIpcServer>();
#else
    return std::make_unique<UnixIpcServer>();
#endif
}