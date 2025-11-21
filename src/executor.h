#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <string>
#include <memory>
#include <atomic>

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

  // 新增私有方法，用于线程函数
  void handle_client_session(std::unique_ptr<ClientConnection> connection);
};
#endif // EXECUTOR_H
