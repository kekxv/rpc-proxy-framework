#ifndef IPC_SERVER_H
#define IPC_SERVER_H

#include <string>
#include <functional>

class IpcServer {
public:
    using RequestHandler = std::function<std::string(const std::string&)>;

    IpcServer();
    ~IpcServer();

    void start(const std::string& pipe_name, RequestHandler handler);

public: // Changed from private to public
    class Impl;
    Impl* pimpl;
};

#endif // IPC_SERVER_H
