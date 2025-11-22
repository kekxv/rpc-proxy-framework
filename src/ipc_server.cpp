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

using json = Json::Value;

// --- Platform-specific Connection Implementations ---

#ifdef _WIN32
class WindowsConnection : public ClientConnection
{
public:
  explicit WindowsConnection(HANDLE pipe) : pipe_(pipe), is_open_(true)
  {
  }

  ~WindowsConnection() override
  {
    if (pipe_ != INVALID_HANDLE_VALUE)
    {
      DisconnectNamedPipe(pipe_);
      CloseHandle(pipe_);
    }
  }

  std::string read() override
  {
    DWORD bytesRead;
    uint32_t net_msg_len;
    if (!ReadFile(pipe_, &net_msg_len, sizeof(net_msg_len), &bytesRead, NULL) || bytesRead == 0)
    {
      is_open_ = false;
      return "";
    }

    uint32_t msg_len = ntohl(net_msg_len);
    std::vector<char> buffer(msg_len);
    if (!ReadFile(pipe_, buffer.data(), msg_len, &bytesRead, NULL) || bytesRead != msg_len)
    {
      is_open_ = false;
      return "";
    }
    return std::string(buffer.begin(), buffer.end());
  }

  bool write(const std::string& message) override
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    uint32_t len = message.length();
    uint32_t net_len = htonl(len);
    DWORD bytesWritten;
    if (!WriteFile(pipe_, &net_len, sizeof(net_len), &bytesWritten, NULL) ||
      !WriteFile(pipe_, message.c_str(), len, &bytesWritten, NULL))
    {
      is_open_ = false;
      return false;
    }
    return true;
  }

  bool sendEvent(const json& event_json) override
  {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return write(Json::writeString(builder, event_json));
  }

  bool isOpen() override { return is_open_; }

private:
  HANDLE pipe_;
  std::atomic<bool> is_open_;
  std::mutex write_mutex_;
};

#else
class UnixConnection : public ClientConnection
{
public:
  explicit UnixConnection(int sock) : sock_(sock), is_open_(true)
  {
  }

  ~UnixConnection() override
  {
    if (sock_ != -1)
    {
      close(sock_);
    }
  }

  std::string read() override
  {
    uint32_t net_msg_len;
    ssize_t bytes_received_for_len = 0;
    // std::cout << "[Executor: UnixConnection::read] Waiting to receive 4-byte length header..." << std::endl;
    while (bytes_received_for_len < sizeof(net_msg_len))
    {
      ssize_t current_read = recv(sock_, reinterpret_cast<char*>(&net_msg_len) + bytes_received_for_len,
                                  sizeof(net_msg_len) - bytes_received_for_len, 0);
      if (current_read == 0)
      {
        std::cout << "[Executor: UnixConnection::read] Client disconnected during length header read." << std::endl;
        is_open_ = false;
        return "";
      }
      if (current_read == -1)
      {
        std::cerr << "[Executor: UnixConnection::read] Error receiving length header: " << strerror(errno) << std::endl;
        is_open_ = false;
        return "";
      }
      bytes_received_for_len += current_read;
    }
    uint32_t msg_len = ntohl(net_msg_len);
    std::vector<char> buffer(msg_len);
    ssize_t total_read = 0;
    while (total_read < (ssize_t)msg_len)
    {
      ssize_t current_read = recv(sock_, buffer.data() + total_read, msg_len - total_read, 0);
      if (current_read <= 0)
      {
        is_open_ = false;
        return "";
      }
      total_read += current_read;
    }
    return std::string(buffer.begin(), buffer.end());
  }

  bool write(const std::string& message) override
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    uint32_t len = message.length();
    uint32_t net_len = htonl(len);
    if (send(sock_, &net_len, sizeof(net_len), 0) == -1 ||
      send(sock_, message.c_str(), len, 0) == -1)
    {
      is_open_ = false;
      return false;
    }
    return true;
  }

  bool sendEvent(const json& event_json) override
  {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return write(Json::writeString(builder, event_json));
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
class WindowsIpcServer : public IpcServer
{
public:
  WindowsIpcServer()
  {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
      throw std::runtime_error("WSAStartup failed.");
    }
  }

  ~WindowsIpcServer() override
  {
    stop();
    WSACleanup();
  }

  void listen(const std::string& name) override
  {
    pipe_name_ = "\\\\.\\pipe\\" + name;
  }

  std::unique_ptr<ClientConnection> accept() override
  {
    HANDLE hPipe = CreateNamedPipeA(pipe_name_.c_str(), PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL);
    
    if (hPipe == INVALID_HANDLE_VALUE)
    {
        std::cerr << "[TID:" << std::this_thread::get_id() << "][Server] CreateNamedPipeA failed with error: " << GetLastError() << std::endl;
        // throw is too aggressive in a loop, let's return null and let the runner decide.
        return nullptr;
    }
    listener_pipe_ = hPipe;


    BOOL connected = ConnectNamedPipe(hPipe, NULL);
    if (!connected && GetLastError() == ERROR_PIPE_CONNECTED) {
        connected = TRUE;
    }

    if (connected)
    {
      listener_pipe_ = INVALID_HANDLE_VALUE;
      return std::make_unique<WindowsConnection>(hPipe);
    }
    
    CloseHandle(hPipe);
    listener_pipe_ = INVALID_HANDLE_VALUE;
    return nullptr;
  }

  void stop() override
  {
    // To reliably unblock ConnectNamedPipe, we connect to ourselves with a dummy client.
    // This is more robust than just closing the handle from another thread.
    HANDLE hPipe = CreateFileA(
        pipe_name_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hPipe != INVALID_HANDLE_VALUE) {
        // Just connect and immediately close. The goal is just to unblock the listener.
        CloseHandle(hPipe);
    } else {
        if (listener_pipe_ != INVALID_HANDLE_VALUE)
        {
          CloseHandle(listener_pipe_);
          listener_pipe_ = INVALID_HANDLE_VALUE;
        }
    }
  }

private:
  std::string pipe_name_;
  HANDLE listener_pipe_ = INVALID_HANDLE_VALUE;
};
#else
class UnixIpcServer : public IpcServer
{
public:
  UnixIpcServer() = default;
  ~UnixIpcServer() override { stop(); }

  void listen(const std::string& name) override
  {
    socket_path_ = "/tmp/" + name;
    if ((server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0)) == 0)
    {
      throw std::runtime_error("Socket creation failed");
    }

    struct sockaddr_un address;
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, socket_path_.c_str(), sizeof(address.sun_path) - 1);
    unlink(socket_path_.c_str());

    if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0)
    {
      throw std::runtime_error("Socket bind failed for path: " + socket_path_);
    }

    if (::listen(server_fd_, 5) < 0)
    {
      throw std::runtime_error("Socket listen failed");
    }
  }

  std::unique_ptr<ClientConnection> accept() override
  {
    std::cout << "Waiting for client connection on " << socket_path_ << "..." << std::endl;
    int new_socket = ::accept(server_fd_, NULL, NULL);
    if (new_socket < 0)
    {
      return nullptr;
    }
    std::cout << "Client connected." << std::endl;
    return std::make_unique<UnixConnection>(new_socket);
  }

  void stop() override
  {
    if (server_fd_ == -1) {
        return;
    }
    
    // The "self-connect" pattern is the most robust way to unblock a blocking accept() call.
    int dummy_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (dummy_socket != -1) {
        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
        // Connect to our own socket to unblock the accept() call. No need to check for success.
        ::connect(dummy_socket, (struct sockaddr*)&addr, sizeof(addr));
        close(dummy_socket);
    }

    // Now we can safely close the original server socket.
    close(server_fd_);
    unlink(socket_path_.c_str());
    server_fd_ = -1;
  }

private:
  int server_fd_ = -1;
  std::string socket_path_;
};
#endif

// --- IpcServer Factory Method ---

std::unique_ptr<IpcServer> IpcServer::create()
{
#ifdef _WIN32
  return std::make_unique<WindowsIpcServer>();
#else
  return std::make_unique<UnixIpcServer>();
#endif
}
