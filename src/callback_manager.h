// src/callback_manager.h
#ifndef CALLBACK_MANAGER_H
#define CALLBACK_MANAGER_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

#include <ffi.h>
#include <nlohmann/json.hpp> // For JSON handling
#include "ipc_server.h" // To send events back to controller
#include "struct_manager.h" // To resolve struct types for callback arguments

// Forward declaration
class IpcServer;

struct CallbackInfo {
    std::string callback_id;
    ffi_cif cif;
    ffi_closure* closure;
    void* trampoline_function_ptr;
    std::vector<ffi_type*> arg_types; // Owned by CallbackInfo
    ffi_type* return_type; // Owned by CallbackInfo
    std::vector<std::string> arg_type_names; // For serialization
    std::string return_type_name; // For serialization
    // Pointer to the IPC server to send events
    IpcServer* ipc_server;
    // Pointer to the StructManager to resolve types
    StructManager* struct_manager;
};

class CallbackManager {
public:
    CallbackManager(IpcServer* ipc_server, StructManager* struct_manager);
    ~CallbackManager();

    // Registers a callback signature and returns a unique callback_id
    // return_type_name and arg_type_names are JSON-compatible type strings
    std::string registerCallback(const std::string& return_type_name, const std::vector<std::string>& arg_type_names);

    // Unregisters a callback by its ID, freeing resources
    void unregisterCallback(const std::string& callback_id);

    // Gets the ffi_type* for a given type name
    ffi_type* getFfiType(const std::string& type_name);

    // Static trampoline function that libffi will call
    static void ffi_trampoline(ffi_cif* cif, void* ret, void** args, void* userdata);

    // Gets the trampoline function pointer for a registered callback_id
    void* getTrampolineFunctionPtr(const std::string& callback_id);

private:
    std::map<std::string, std::unique_ptr<CallbackInfo>> registered_callbacks_;
    IpcServer* ipc_server_; // Not owned
    StructManager* struct_manager_; // Not owned

    // Helper to generate unique IDs
    std::string generateUniqueId();
};

#endif // CALLBACK_MANAGER_H
