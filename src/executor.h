#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <set>

// Forward declarations
class IpcServer;
class ClientConnection;

class Executor
{
public:
  Executor();
  ~Executor();

  void run(const std::string& pipe_name);
  void stop();

private:
  std::unique_ptr<IpcServer> server;
  std::atomic<bool> is_running_{false};
  std::mutex sessions_mutex_;
  std::set<ClientConnection*> active_connections_;
  std::vector<std::thread> session_threads_;

  void handle_client_session(std::unique_ptr<ClientConnection> connection);
  void close_active_connections();
  void join_session_threads();
};
#endif // EXECUTOR_H
