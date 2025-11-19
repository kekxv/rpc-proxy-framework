#include "executor.h"
#include "ipc_server.h"
#include "lib_manager.h"
#include "struct_manager.h"
#include "callback_manager.h"
#include "ffi_dispatcher.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <memory>

using json = nlohmann::json;

// This function handles a single request within a client session.
// It operates on the session-specific managers.
std::string handle_session_request(
    const std::string& request_json_str,
    LibManager& lib_manager,
    StructManager& struct_manager,
    CallbackManager& callback_manager,
    FfiDispatcher& ffi_dispatcher)
{
    json response_json;
    try {
        json request_json = json::parse(request_json_str);
        std::string command = request_json.at("command");
        response_json["request_id"] = request_json.value("request_id", "");

        if (command == "load_library") {
            std::string path = request_json.at("payload").at("path");
            std::string library_id = lib_manager.load_library(path);
            response_json["status"] = "success";
            response_json["data"]["library_id"] = library_id;
        } else if (command == "unload_library") {
            std::string library_id = request_json.at("payload").at("library_id");
            lib_manager.unload_library(library_id);
            response_json["status"] = "success";
        } else if (command == "register_struct") {
            std::string struct_name = request_json.at("payload").at("struct_name");
            const json& definition = request_json.at("payload").at("definition");
            struct_manager.register_struct(struct_name, definition);
            response_json["status"] = "success";
        } else if (command == "unregister_struct") {
            std::string struct_name = request_json.at("payload").at("struct_name");
            struct_manager.unregister_struct(struct_name);
            response_json["status"] = "success";
        } else if (command == "register_callback") {
            std::string return_type = request_json.at("payload").at("return_type");
            std::vector<std::string> args_type = request_json.at("payload").at("args_type").get<std::vector<std::string>>();
            std::string callback_id = callback_manager.registerCallback(return_type, args_type);
            response_json["status"] = "success";
            response_json["data"]["callback_id"] = callback_id;
        } else if (command == "unregister_callback") {
            std::string callback_id = request_json.at("payload").at("callback_id");
            callback_manager.unregisterCallback(callback_id);
            response_json["status"] = "success";
        } else if (command == "call_function") {
            const auto& payload = request_json.at("payload");
            std::string library_id = payload.at("library_id");
            std::string function_name = payload.at("function_name");
            void* func_ptr = lib_manager.get_function(library_id, function_name);
            json result = ffi_dispatcher.call_function(func_ptr, payload);
            response_json["status"] = "success";
            response_json["data"] = result;
        } else {
            throw std::runtime_error("Unknown command: " + command);
        }
    } catch (const std::exception& e) {
        response_json["status"] = "error";
        response_json["error_message"] = e.what();
    }
    return response_json.dump();
}

Executor::Executor() {}

void Executor::run(const std::string& pipe_name) {
    auto server = IpcServer::create();
    server->listen(pipe_name);

    while (true) {
        // 1. Accept a new connection. This blocks until a client connects.
        std::unique_ptr<ClientConnection> connection = server->accept();
        if (!connection) {
            std::cerr << "Failed to accept new connection. Shutting down." << std::endl;
            break;
        }

        // 2. Create a new set of managers for this client session.
        StructManager struct_manager;
        CallbackManager callback_manager(connection.get(), &struct_manager);
        LibManager lib_manager;
        FfiDispatcher ffi_dispatcher(struct_manager, &callback_manager);

        // 3. Loop to handle all requests from this client.
        while (connection->isOpen()) {
            std::string request_str = connection->read();
            if (request_str.empty()) {
                // An empty read indicates the client has disconnected.
                break;
            }

            std::string response_str;
            try {
                response_str = handle_session_request(
                    request_str, lib_manager, struct_manager, callback_manager, ffi_dispatcher);
            } catch (const std::exception& e) {
                std::cerr << "[Executor] Error handling request: " << e.what() << std::endl;
                // Attempt to send an error response if possible
                json error_response;
                error_response["status"] = "error";
                error_response["error_message"] = e.what();
                // If request_str was valid JSON, we can try to get request_id
                try {
                    json request_json = json::parse(request_str);
                    error_response["request_id"] = request_json.value("request_id", "");
                } catch (...) {
                    // Ignore if request_str itself is malformed
                }
                response_str = error_response.dump();
            }
            
            if (!connection->write(response_str)) {
                std::cerr << "[Executor] Failed to write response (or error response). Client likely disconnected." << std::endl;
                break;
            }
        }

        // 4. Client has disconnected.
        // The managers (lib_manager, struct_manager, callback_manager) go out of scope here.
        // Their destructors are automatically called, cleaning up all resources for this session.
        std::cout << "Client disconnected. Session resources cleaned up." << std::endl;
    }
}