#ifndef IPC_SERVER_H
#define IPC_SERVER_H

#include <string>
#include <functional>
#include <nlohmann/json.hpp> // Include nlohmann/json for json type

class IpcServer {
public:
    using RequestHandler = std::function<std::string(const std::string&)>;

    IpcServer();
    ~IpcServer();

    void start(const std::string& pipe_name, RequestHandler handler);
    void sendEvent(const nlohmann::json& event_json); // New method to send asynchronous events

public: // Changed from private to public
    class Impl;
    Impl* pimpl;
};

#endif // IPC_SERVER_H
