// src/callback_manager.h
#ifndef CALLBACK_MANAGER_H
#define CALLBACK_MANAGER_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <atomic>

#include <ffi.h>
#include <json/json.h> // For JSON handling
#include "ipc_server.h" // For ClientConnection
#include "struct_manager.h" // To resolve struct types for callback arguments

// Forward declaration
class ClientConnection;

struct CallbackArgInfo {
    std::string type_name;
    ffi_type* ffi_type_ptr;
    int size_arg_index = -1; // For buffer_ptr: index of the argument that holds the size
    int fixed_size = -1;     // For buffer_ptr: fixed size if size_arg_index is not used
};

struct CallbackInfo {
    ~CallbackInfo() {
        if (struct_manager) {
            for (const auto& type : retained_structs) struct_manager->release_struct(type);
        }
    }
    std::string callback_id;
    ffi_cif cif;
    ffi_closure* closure;
    void* trampoline_function_ptr;
    std::vector<ffi_type*> arg_types; // For ffi_prep_cif
    ffi_type* return_type;
    
    // Enhanced argument info
    std::vector<CallbackArgInfo> args_info;
    
    std::string return_type_name;
    std::atomic<ClientConnection*> connection{nullptr};
    StructManager* struct_manager;
    std::vector<std::string> retained_structs;
};

class CallbackManager {
public:
    CallbackManager(ClientConnection* connection, StructManager* struct_manager);
    ~CallbackManager();

    // Updated signature to accept Json::Value for args_type to support objects
    std::string registerCallback(const std::string& return_type_name, const Json::Value& args_type_def);
    void unregisterCallback(const std::string& callback_id);
    void* getTrampolineFunctionPtr(const std::string& callback_id);
    void invalidateConnection(ClientConnection* connection);

    // Static trampoline function that libffi will call
    static void ffi_trampoline(ffi_cif* cif, void* ret, void** args, void* userdata);

private:
    ffi_type* getFfiType(const std::string& type_name);
    std::string generateUniqueId();

    std::map<std::string, std::unique_ptr<CallbackInfo>> registered_callbacks_;
    ClientConnection* connection_; // Not owned
    StructManager* struct_manager_; // Not owned
};

#endif // CALLBACK_MANAGER_H
