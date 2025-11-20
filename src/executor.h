#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <string>
#include <memory>

class IpcServer; // Forward declaration

class Executor {
public:
    Executor();
    ~Executor(); // Destructor to manage resource cleanup
    void run(const std::string& pipe_name);
    void stop();

private:
    std::unique_ptr<IpcServer> server;
};

#endif // EXECUTOR_H
