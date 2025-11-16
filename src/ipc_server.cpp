#include "ipc_server.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cstdint>

#ifdef _WIN32
// It's important to include winsock2.h before windows.h
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h> // For ntohl, htonl
#endif

class IpcServer::Impl {
public:
    virtual ~Impl() = default;
    virtual void start(const std::string& pipe_name, IpcServer::RequestHandler handler) = 0;
};

#ifdef _WIN32
class WindowsIpcServer : public IpcServer::Impl {
public:
    void start(const std::string& pipe_name, IpcServer::RequestHandler handler) override {
        std::string full_pipe_name = "\\\\.\\pipe\\" + pipe_name;

        while (true) {
            HANDLE hPipe = CreateNamedPipeA(
                full_pipe_name.c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                4096, 4096, 0, NULL);

            if (hPipe == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("Failed to create named pipe.");
            }

            if (ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED)) {
                std::thread([hPipe, handler]() {
                    handle_connection(hPipe, handler);
                }).detach();
            } else {
                CloseHandle(hPipe);
            }
        }
    }

private:
    static void handle_connection(HANDLE hPipe, const IpcServer::RequestHandler& handler) {
        while (true) {
            DWORD bytesRead;
            uint32_t net_msg_len;
            
            BOOL success = ReadFile(hPipe, &net_msg_len, sizeof(net_msg_len), &bytesRead, NULL);

            if (!success || bytesRead == 0) {
                // Client disconnected or an error occurred.
                break;
            }

            if (bytesRead == sizeof(net_msg_len)) {
                uint32_t msg_len = ntohl(net_msg_len);
                std::vector<char> request_buf(msg_len);

                if (ReadFile(hPipe, request_buf.data(), msg_len, &bytesRead, NULL) && bytesRead == msg_len) {
                    std::string request(request_buf.begin(), request_buf.end());
                    std::string response = handler(request);

                    uint32_t response_len = response.length();
                    uint32_t net_response_len = htonl(response_len);
                    DWORD bytesWritten;
                    WriteFile(hPipe, &net_response_len, sizeof(net_response_len), &bytesWritten, NULL);
                    WriteFile(hPipe, response.c_str(), response.length(), &bytesWritten, NULL);
                } else {
                    break; // Incomplete message read
                }
            } else {
                break; // Incomplete length read
            }
        }

        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
};
#else
class UnixIpcServer : public IpcServer::Impl {
public:
    void start(const std::string& socket_path, IpcServer::RequestHandler handler) override {
        int server_fd;
        struct sockaddr_un address;

        // On macOS, socket path can't be too long. Let's place it in /tmp
        std::string actual_socket_path = "/tmp/" + socket_path;

        if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == 0) {
            throw std::runtime_error("Socket creation failed");
        }

        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, actual_socket_path.c_str(), sizeof(address.sun_path) - 1);
        unlink(actual_socket_path.c_str());

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            throw std::runtime_error("Socket bind failed for path: " + actual_socket_path);
        }

        if (listen(server_fd, 5) < 0) {
            throw std::runtime_error("Socket listen failed");
        }

        while (true) {
            int new_socket;
            int addrlen = sizeof(address);
            if ((new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
                continue; // Or log error
            }
            std::thread([new_socket, handler]() {
                handle_connection(new_socket, handler);
            }).detach();
        }
    }

private:
    static void handle_connection(int sock, const IpcServer::RequestHandler& handler) {
        while (true) {
            uint32_t net_msg_len;
            ssize_t read_bytes = read(sock, &net_msg_len, sizeof(net_msg_len));

            if (read_bytes <= 0) { // 0 means client closed connection, < 0 is an error
                break; // Exit loop and close socket
            }

            if ((size_t)read_bytes == sizeof(net_msg_len)) {
                uint32_t msg_len = ntohl(net_msg_len);
                std::vector<char> request_buf(msg_len);

                // Ensure we read the full message
                ssize_t total_read = 0;
                while(total_read < (ssize_t)msg_len) {
                    ssize_t current_read = read(sock, request_buf.data() + total_read, msg_len - total_read);
                    if (current_read <= 0) {
                        goto end_loop;
                    }
                    total_read += current_read;
                }

                if (total_read == (ssize_t)msg_len) {
                    std::string request(request_buf.begin(), request_buf.end());
                    std::string response = handler(request);

                    uint32_t response_len = response.length();
                    uint32_t net_response_len = htonl(response_len);
                    send(sock, &net_response_len, sizeof(net_response_len), 0);
                    send(sock, response.c_str(), response.length(), 0);
                } else {
                    break; // Incomplete message read
                }
            } else {
                break; // Incomplete length read
            }
        }
    end_loop:
        close(sock);
    }
};
#endif

IpcServer::IpcServer() {
#ifdef _WIN32
    // WSAStartup is required for using winsock functions like ntohl, htonl
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
