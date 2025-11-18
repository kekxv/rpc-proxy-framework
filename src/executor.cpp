#include "executor.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <future> // Added for std::promise and std::future

using json = nlohmann::json;

Executor::Executor()
    : struct_manager_(), // Initialize StructManager first
      ipc_server_(),     // Initialize IpcServer
      callback_manager_(&ipc_server_, &struct_manager_), // Initialize CallbackManager with IPC and StructManager
      lib_manager_(),    // Initialize LibManager
      ffi_dispatcher_(struct_manager_, &callback_manager_) // Initialize FfiDispatcher with StructManager and CallbackManager
{
    // All members are initialized in the initializer list
}

std::string Executor::handleRequest(const std::string& request_json_str) {
    std::cout << "[Executor] Received request: " << request_json_str << std::endl;
    json response_json;
    try {
        json request_json = json::parse(request_json_str);
        std::string command = request_json.at("command");
        response_json["request_id"] = request_json.value("request_id", "");

        std::cout << "[Executor] Processing command: " << command << std::endl;

        if (command == "load_library") {
            std::string path = request_json.at("payload").at("path");
            std::string library_id = lib_manager_.load_library(path);
            response_json["status"] = "success";
            response_json["data"]["library_id"] = library_id;
        } else if (command == "unload_library") {
            std::string library_id = request_json.at("payload").at("library_id");
            lib_manager_.unload_library(library_id);
            response_json["status"] = "success";
        } else if (command == "register_struct") {
            std::string struct_name = request_json.at("payload").at("struct_name");
            const json& definition = request_json.at("payload").at("definition");
            struct_manager_.register_struct(struct_name, definition);
            response_json["status"] = "success";
        } else if (command == "unregister_struct") {
            std::string struct_name = request_json.at("payload").at("struct_name");
            struct_manager_.unregister_struct(struct_name);
            response_json["status"] = "success";
        } else if (command == "register_callback") {
            std::string return_type_name = request_json.at("payload").at("return_type");
            std::vector<std::string> arg_type_names = request_json.at("payload").at("args_type").get<std::vector<std::string>>();
            std::string callback_id = callback_manager_.registerCallback(return_type_name, arg_type_names);
            response_json["status"] = "success";
            response_json["data"]["callback_id"] = callback_id;
        } else if (command == "unregister_callback") {
            std::string callback_id = request_json.at("payload").at("callback_id");
            callback_manager_.unregisterCallback(callback_id);
            response_json["status"] = "success";
        } else if (command == "call_function") {
            const auto& payload = request_json.at("payload");
            std::string library_id = payload.at("library_id");
            std::string function_name = payload.at("function_name");

            std::cout << "[Executor] Calling function '" << function_name << "' in library '" << library_id << "'" << std::endl;
            void* func_ptr = lib_manager_.get_function(library_id, function_name);
            std::cout << "[Executor] Got function pointer: " << func_ptr << std::endl;

            json result = ffi_dispatcher_.call_function(func_ptr, payload);
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
}

void Executor::run(const std::string& pipe_name) {
    ipc_server_.start(pipe_name, [this](const std::string& request_str) {
        return handleRequest(request_str);
    });

    std::cout << "Executor listening on pipe: " << pipe_name << std::endl;
    // The ipc_server_.start() call is now blocking indefinitely, keeping the executor process alive.
    // A real implementation would need a way to signal shutdown to ipc_server_ and then exit.
}
