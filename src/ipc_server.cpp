#include "ipc_server.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cerrno>
#include <limits>
#include <thread>
#include <cstddef>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
    close();
  }

  std::string read() override
  {
    uint32_t net_msg_len;
    if (!readExact(&net_msg_len, sizeof(net_msg_len))) return "";

    uint32_t msg_len = ntohl(net_msg_len);
    if (msg_len == 0 || msg_len > kMaxIpcFrameSize)
    {
      std::cerr << "[Executor: WindowsConnection::read] Invalid frame length: " << msg_len << std::endl;
      close();
      return "";
    }
    std::vector<char> buffer(msg_len);
    if (!readExact(buffer.data(), buffer.size())) return "";
    return std::string(buffer.begin(), buffer.end());
  }

  bool write(const std::string& message) override
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!is_open_ || message.empty() || message.size() > kMaxIpcFrameSize ||
        message.size() > (std::numeric_limits<uint32_t>::max)()) return false;
    uint32_t len = static_cast<uint32_t>(message.size());
    uint32_t net_len = htonl(len);
    return writeAll(&net_len, sizeof(net_len)) && writeAll(message.data(), message.size());
  }

  bool sendEvent(const json& event_json) override
  {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return write(Json::writeString(builder, event_json));
  }

  bool isOpen() override { return is_open_; }

  void close() override
  {
    HANDLE pipe = pipe_.exchange(INVALID_HANDLE_VALUE);
    is_open_ = false;
    if (pipe != INVALID_HANDLE_VALUE)
    {
      CancelIoEx(pipe, NULL);
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
    }
  }

private:
  bool readExact(void* data, size_t size)
  {
    auto* bytes = static_cast<unsigned char*>(data);
    size_t total = 0;
    while (total < size && is_open_)
    {
      HANDLE pipe = pipe_.load();
      if (pipe == INVALID_HANDLE_VALUE) return false;
      DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - total, (std::numeric_limits<DWORD>::max)()));
      DWORD bytes_read = 0;
      if (!ReadFile(pipe, bytes + total, chunk, &bytes_read, NULL) || bytes_read == 0)
      {
        is_open_ = false;
        return false;
      }
      total += bytes_read;
    }
    return total == size;
  }

  bool writeAll(const void* data, size_t size)
  {
    const auto* bytes = static_cast<const unsigned char*>(data);
    size_t total = 0;
    while (total < size && is_open_)
    {
      HANDLE pipe = pipe_.load();
      if (pipe == INVALID_HANDLE_VALUE) return false;
      DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - total, (std::numeric_limits<DWORD>::max)()));
      DWORD bytes_written = 0;
      if (!WriteFile(pipe, bytes + total, chunk, &bytes_written, NULL) || bytes_written == 0)
      {
        is_open_ = false;
        return false;
      }
      total += bytes_written;
    }
    return total == size;
  }

  std::atomic<HANDLE> pipe_;
  std::atomic<bool> is_open_;
  std::mutex write_mutex_;
};

#else
class UnixConnection : public ClientConnection
{
public:
  explicit UnixConnection(int sock) : sock_(sock), is_open_(true)
  {
#ifdef __APPLE__
    int enabled = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
  }

  ~UnixConnection() override
  {
    close();
  }

  std::string read() override
  {
    uint32_t net_msg_len;
    if (!readExact(&net_msg_len, sizeof(net_msg_len))) return "";
    uint32_t msg_len = ntohl(net_msg_len);
    if (msg_len == 0 || msg_len > kMaxIpcFrameSize)
    {
      std::cerr << "[Executor: UnixConnection::read] Invalid frame length: " << msg_len << std::endl;
      close();
      return "";
    }
    std::vector<char> buffer(msg_len);
    if (!readExact(buffer.data(), buffer.size())) return "";
    return std::string(buffer.begin(), buffer.end());
  }

  bool write(const std::string& message) override
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!is_open_ || message.empty() || message.size() > kMaxIpcFrameSize ||
        message.size() > (std::numeric_limits<uint32_t>::max)()) return false;
    uint32_t len = static_cast<uint32_t>(message.size());
    uint32_t net_len = htonl(len);
    return writeAll(&net_len, sizeof(net_len)) && writeAll(message.data(), message.size());
  }

  bool sendEvent(const json& event_json) override
  {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return write(Json::writeString(builder, event_json));
  }

  bool isOpen() override { return is_open_; }

  void close() override
  {
    int sock = sock_.exchange(-1);
    is_open_ = false;
    if (sock != -1)
    {
      shutdown(sock, SHUT_RDWR);
      ::close(sock);
    }
  }

private:
  bool readExact(void* data, size_t size)
  {
    auto* bytes = static_cast<unsigned char*>(data);
    size_t total = 0;
    while (total < size && is_open_)
    {
      int sock = sock_.load();
      if (sock == -1) return false;
      ssize_t count = recv(sock, bytes + total, size - total, 0);
      if (count > 0) total += static_cast<size_t>(count);
      else if (count < 0 && errno == EINTR) continue;
      else
      {
        is_open_ = false;
        return false;
      }
    }
    return total == size;
  }

  bool writeAll(const void* data, size_t size)
  {
    const auto* bytes = static_cast<const unsigned char*>(data);
    size_t total = 0;
    while (total < size && is_open_)
    {
      int sock = sock_.load();
      if (sock == -1) return false;
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      ssize_t count = send(sock, bytes + total, size - total, flags);
      if (count > 0) total += static_cast<size_t>(count);
      else if (count < 0 && errno == EINTR) continue;
      else
      {
        is_open_ = false;
        return false;
      }
    }
    return total == size;
  }

  std::atomic<int> sock_;
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
    std::cout << "Waiting for client connection on " << pipe_name_ << "..." << std::endl;
    HANDLE hPipe = CreateNamedPipeA(pipe_name_.c_str(), PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                                    4096, 4096, 0, NULL);

    if (hPipe == INVALID_HANDLE_VALUE)
    {
      std::cerr << "[TID:" << std::this_thread::get_id() << "][Server] CreateNamedPipeA failed with error: " <<
        GetLastError() << std::endl;
      // throw is too aggressive in a loop, let's return null and let the runner decide.
      return nullptr;
    }
    listener_pipe_.store(hPipe);


    BOOL connected = ConnectNamedPipe(hPipe, NULL);
    if (!connected && GetLastError() == ERROR_PIPE_CONNECTED)
    {
      connected = TRUE;
    }

    if (connected)
    {
      listener_pipe_.exchange(INVALID_HANDLE_VALUE);
      std::cout << "Client connected." << std::endl;
      return std::make_unique<WindowsConnection>(hPipe);
    }

    if (listener_pipe_.exchange(INVALID_HANDLE_VALUE) == hPipe) CloseHandle(hPipe);
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

    if (hPipe != INVALID_HANDLE_VALUE)
    {
      // Just connect and immediately close. The goal is just to unblock the listener.
      CloseHandle(hPipe);
    }
    else
    {
      HANDLE listener = listener_pipe_.exchange(INVALID_HANDLE_VALUE);
      if (listener != INVALID_HANDLE_VALUE)
      {
        CancelIoEx(listener, NULL);
        CloseHandle(listener);
      }
    }
  }

private:
  std::string pipe_name_;
  std::atomic<HANDLE> listener_pipe_{INVALID_HANDLE_VALUE};
};
#else
class UnixIpcServer : public IpcServer
{
public:
  UnixIpcServer() = default;
  ~UnixIpcServer() override { stop(); }

  void listen(const std::string& name) override
  {
    if (name.empty()) throw std::runtime_error("Socket name must not be empty");
    socket_path_ = "/tmp/" + name;
    if (socket_path_.size() >= sizeof(sockaddr_un{}.sun_path))
      throw std::runtime_error("Socket path is too long: " + socket_path_);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
      throw std::runtime_error("Socket creation failed: " + std::string(strerror(errno)));
    }
    server_fd_.store(server_fd);

    struct sockaddr_un address{};
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, socket_path_.c_str(), sizeof(address.sun_path) - 1);
    unlink(socket_path_.c_str());

    const socklen_t address_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socket_path_.size() + 1);

    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&address), address_len) < 0)
    {
      const std::string error = strerror(errno);
      ::close(server_fd_.exchange(-1));
      throw std::runtime_error("Socket bind failed for path " + socket_path_ + ": " + error);
    }

    if (::listen(server_fd, 16) < 0)
    {
      const std::string error = strerror(errno);
      ::close(server_fd_.exchange(-1));
      unlink(socket_path_.c_str());
      throw std::runtime_error("Socket listen failed: " + error);
    }
  }

  std::unique_ptr<ClientConnection> accept() override
  {
    std::cout << "Waiting for client connection on " << socket_path_ << "..." << std::endl;
    int server_fd = server_fd_.load();
    if (server_fd == -1) return nullptr;
    int new_socket;
    do { new_socket = ::accept(server_fd, NULL, NULL); } while (new_socket < 0 && errno == EINTR);
    if (new_socket < 0)
    {
      return nullptr;
    }
    std::cout << "Client connected." << std::endl;
    return std::make_unique<UnixConnection>(new_socket);
  }

  void stop() override
  {
    int server_fd = server_fd_.exchange(-1);
    if (server_fd == -1)
    {
      return;
    }

    // The "self-connect" pattern is the most robust way to unblock a blocking accept() call.
    int dummy_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (dummy_socket != -1)
    {
      struct sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
      // Connect to our own socket to unblock the accept() call. No need to check for success.
      const socklen_t addr_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socket_path_.size() + 1);
      ::connect(dummy_socket, reinterpret_cast<struct sockaddr*>(&addr), addr_len);
      close(dummy_socket);
    }

    // Now we can safely close the original server socket.
    close(server_fd);
    unlink(socket_path_.c_str());
  }

private:
  std::atomic<int> server_fd_{-1};
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
