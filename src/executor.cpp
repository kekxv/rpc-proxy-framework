#include "executor.h"
#include "ipc_server.h"
#include "lib_manager.h"
#include "ffi_dispatcher.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <future> // Added for std::promise and std::future

using json = nlohmann::json;

void Executor::run(const std::string& pipe_name) {
    IpcServer server;
    LibManager lib_manager;
    FfiDispatcher ffi_dispatcher;

    server.start(pipe_name, [&](const std::string& request_str) -> std::string {
        std::cout << "[Executor] Received request: " << request_str << std::endl;
        json response_json;
        try {
            json request_json = json::parse(request_str);
            std::string command = request_json.at("command");
            response_json["request_id"] = request_json.value("request_id", "");

            std::cout << "[Executor] Processing command: " << command << std::endl;

            if (command == "load_library") {
                std::string path = request_json.at("payload").at("path");
                std::string library_id = lib_manager.load_library(path);
                response_json["status"] = "success";
                response_json["data"]["library_id"] = library_id;
            } else if (command == "unload_library") {
                std::string library_id = request_json.at("payload").at("library_id");
                lib_manager.unload_library(library_id);
                response_json["status"] = "success";
            } else if (command == "call_function") {
                const auto& payload = request_json.at("payload");
                std::string library_id = payload.at("library_id");
                std::string function_name = payload.at("function_name");
                
                std::cout << "[Executor] Calling function '" << function_name << "' in library '" << library_id << "'" << std::endl;
                void* func_ptr = lib_manager.get_function(library_id, function_name);
                std::cout << "[Executor] Got function pointer: " << func_ptr << std::endl;
                
                json result = ffi_dispatcher.call_function(func_ptr, payload);
                std::cout << "[Executor] Function call finished." << std::endl;

                response_json["status"] = "success";
                response_json["data"] = result;
            } else {
                throw std::runtime_error("Unknown command: " + command);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Executor] Exception caught: " << e.what() << std::endl;
            response_json["status"] = "error";
            response_json["error_message"] = e.what();
        }
        
        std::cout << "[Executor] Sending response: " << response_json.dump() << std::endl;
        return response_json.dump();
    });

    std::cout << "Executor listening on pipe: " << pipe_name << std::endl;
    // The server's run loop is blocking, so we just let it run.
    // For simplicity, this example has the server run indefinitely.
    // A real implementation would need a way to signal shutdown.
    std::promise<void>().get_future().wait(); // Block forever
}
