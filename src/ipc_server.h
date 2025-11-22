#ifndef IPC_SERVER_H
#define IPC_SERVER_H

#include <string>
#include <memory>
#include <json/json.h>

// Represents a single client connection
class ClientConnection {
public:
    virtual ~ClientConnection() = default;
    virtual std::string read() = 0;
    virtual bool write(const std::string& message) = 0;
    virtual bool sendEvent(const Json::Value& event_json) = 0;
    virtual bool isOpen() = 0;
};

// Abstract base class for the IPC server
class IpcServer {
public:
    virtual ~IpcServer() = default;

    virtual void listen(const std::string& pipe_name) = 0;
    virtual std::unique_ptr<ClientConnection> accept() = 0;
    virtual void stop() = 0;

    // Factory method to create a platform-specific server instance
    static std::unique_ptr<IpcServer> create();
};

#endif // IPC_SERVER_H
