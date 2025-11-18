// src/callback_manager.cpp
#include "callback_manager.h"
#include <iostream>
#include <stdexcept>
#include <algorithm> // For std::transform

// For UUID generation
#ifdef _WIN32
#include <objbase.h> // For CoCreateGuid
#include <Rpc.h>     // For UuidToStringA, RpcStringFreeA
#pragma comment(lib, "Rpcrt4.lib") // Link with Rpcrt4.lib
#else
#include <uuid/uuid.h> // For uuid_generate_time, uuid_unparse
#endif

// Helper to convert ffi_type* to string for JSON serialization
std::string ffiTypeToString(ffi_type* type) {
    if (type == &ffi_type_void) return "void";
    if (type == &ffi_type_uint8) return "uint8";
    if (type == &ffi_type_sint8) return "int8";
    if (type == &ffi_type_uint16) return "uint16";
    if (type == &ffi_type_sint16) return "int16";
    if (type == &ffi_type_uint32) return "uint32";
    if (type == &ffi_type_sint32) return "int32";
    if (type == &ffi_type_uint64) return "uint64";
    if (type == &ffi_type_sint64) return "int64";
    if (type == &ffi_type_float) return "float";
    if (type == &ffi_type_double) return "double";
    if (type == &ffi_type_pointer) return "pointer";
    // Handle struct types if necessary, requires mapping back from ffi_type* to struct name
    // For now, assume pointer for structs passed by value or pointer
    return "unknown"; // Should not happen for supported types
}

CallbackManager::CallbackManager(IpcServer* ipc_server, StructManager* struct_manager)
    : ipc_server_(ipc_server), struct_manager_(struct_manager) {
    if (!ipc_server_ || !struct_manager_) {
        throw std::runtime_error("CallbackManager requires valid IpcServer and StructManager instances.");
    }
}

CallbackManager::~CallbackManager() {
    for (auto const& [id, info] : registered_callbacks_) {
        ffi_closure_free(info->closure);
    }
}

std::string CallbackManager::generateUniqueId() {
    std::string uuid_str;
#ifdef _WIN32
    GUID guid;
    CoCreateGuid(&guid);
    RPC_CSTR rpc_str;
    UuidToStringA(&guid, &rpc_str);
    uuid_str = reinterpret_cast<char*>(rpc_str);
    RpcStringFreeA(&rpc_str);
#else
    uuid_t uuid;
    uuid_generate_time(uuid);
    char s[37];
    uuid_unparse(uuid, s);
    uuid_str = s;
#endif
    return "cb-" + uuid_str;
}

ffi_type* CallbackManager::getFfiType(const std::string& type_name) {
    if (type_name == "void") return &ffi_type_void;
    if (type_name == "int8") return &ffi_type_sint8;
    if (type_name == "uint8") return &ffi_type_uint8;
    if (type_name == "int16") return &ffi_type_sint16;
    if (type_name == "uint16") return &ffi_type_uint16;
    if (type_name == "int32") return &ffi_type_sint32;
    if (type_name == "uint32") return &ffi_type_uint32;
    if (type_name == "int64") return &ffi_type_sint64;
    if (type_name == "uint64") return &ffi_type_uint64;
    if (type_name == "float") return &ffi_type_float;
    if (type_name == "double") return &ffi_type_double;
    if (type_name == "string" || type_name == "pointer") return &ffi_type_pointer;

    // Handle struct types
    if (struct_manager_->is_struct(type_name)) {
        // For now, structs are passed by value or pointer, which libffi treats as pointer
        // More complex struct handling might be needed for direct value passing
        return &ffi_type_pointer;
    }

    throw std::runtime_error("Unknown FFI type: " + type_name);
}

std::string CallbackManager::registerCallback(const std::string& return_type_name, const std::vector<std::string>& arg_type_names) {
    auto info = std::make_unique<CallbackInfo>();
    info->callback_id = generateUniqueId();
    info->ipc_server = ipc_server_;
    info->struct_manager = struct_manager_;
    info->return_type_name = return_type_name;
    info->arg_type_names = arg_type_names;

    // Resolve return type
    info->return_type = getFfiType(return_type_name);

    // Resolve argument types
    info->arg_types.reserve(arg_type_names.size());
    for (const auto& name : arg_type_names) {
        info->arg_types.push_back(getFfiType(name));
    }

    // Prepare CIF
    ffi_status status = ffi_prep_cif(&info->cif, FFI_DEFAULT_ABI,
                                     info->arg_types.size(), info->return_type,
                                     info->arg_types.data());
    if (status != FFI_OK) {
        throw std::runtime_error("Failed to prepare CIF for callback: " + std::to_string(status));
    }

    // Allocate closure and trampoline
    info->closure = static_cast<ffi_closure*>(ffi_closure_alloc(sizeof(ffi_closure), &info->trampoline_function_ptr));
    if (!info->closure) {
        throw std::runtime_error("Failed to allocate ffi_closure for callback.");
    }

    // Prepare the closure
    status = ffi_prep_closure_loc(info->closure, &info->cif, CallbackManager::ffi_trampoline, info.get(), info->trampoline_function_ptr);
    if (status != FFI_OK) {
        ffi_closure_free(info->closure);
        throw std::runtime_error("Failed to prepare ffi_closure_loc for callback: " + std::to_string(status));
    }

    std::string id = info->callback_id;
    registered_callbacks_[id] = std::move(info);
    return id;
}

void CallbackManager::unregisterCallback(const std::string& callback_id) {
    auto it = registered_callbacks_.find(callback_id);
    if (it != registered_callbacks_.end()) {
        ffi_closure_free(it->second->closure);
        registered_callbacks_.erase(it);
    } else {
        throw std::runtime_error("Callback with ID " + callback_id + " not found.");
    }
}

void* CallbackManager::getTrampolineFunctionPtr(const std::string& callback_id) {
    auto it = registered_callbacks_.find(callback_id);
    if (it != registered_callbacks_.end()) {
        return it->second->trampoline_function_ptr;
    }
    throw std::runtime_error("Callback with ID " + callback_id + " not found.");
}

void CallbackManager::ffi_trampoline(ffi_cif* cif, void* ret, void** args, void* userdata) {
    CallbackInfo* info = static_cast<CallbackInfo*>(userdata);
    if (!info || !info->ipc_server) {
        // This should ideally not happen if setup correctly
        std::cerr << "Error: CallbackInfo or IpcServer not available in trampoline." << std::endl;
        return;
    }

    nlohmann::json event_payload;
    event_payload["callback_id"] = info->callback_id;
    nlohmann::json args_json = nlohmann::json::array();

    for (size_t i = 0; i < cif->nargs; ++i) {
        nlohmann::json arg_data;
        arg_data["type"] = info->arg_type_names[i]; // Use stored type name for serialization

        // Deserialize argument based on type
        ffi_type* arg_ffi_type = cif->arg_types[i];
        if (arg_ffi_type == &ffi_type_sint8) {
            arg_data["value"] = *static_cast<int8_t*>(args[i]);
        } else if (arg_ffi_type == &ffi_type_uint8) {
            arg_data["value"] = *static_cast<uint8_t*>(args[i]);
        } else if (arg_ffi_type == &ffi_type_sint16) {
            arg_data["value"] = *static_cast<int16_t*>(args[i]);
        } else if (arg_ffi_type == &ffi_type_uint16) {
            arg_data["value"] = *static_cast<uint16_t*>(args[i]);
        } else if (arg_ffi_type == &ffi_type_sint32) {
            arg_data["value"] = *static_cast<int32_t*>(args[i]);
        } else if (arg_ffi_type == &ffi_type_uint32) {
            arg_data["value"] = *static_cast<uint32_t*>(args[i]);
        } else if (arg_ffi_type == &ffi_type_sint64) {
            arg_data["value"] = *static_cast<int64_t*>(args[i]);
        } else if (arg_ffi_type == &ffi_type_uint64) {
            arg_data["value"] = *static_cast<uint64_t*>(args[i]);
        } else if (arg_ffi_type == &ffi_type_float) {
            arg_data["value"] = *static_cast<float*>(args[i]);
        } else if (arg_ffi_type == &ffi_type_double) {
            arg_data["value"] = *static_cast<double*>(args[i]);
        } else if (arg_ffi_type == &ffi_type_pointer) {
            // Handle string (char*) and generic pointers
            if (info->arg_type_names[i] == "string") {
                char* str_ptr = *static_cast<char**>(args[i]);
                arg_data["value"] = (str_ptr ? std::string(str_ptr) : nullptr);
            } else if (info->struct_manager->is_struct(info->arg_type_names[i])) {
                // Handle struct pointers
                void* struct_ptr = *static_cast<void**>(args[i]);
                if (struct_ptr) {
                    arg_data["value"] = info->struct_manager->serializeStruct(info->arg_type_names[i], struct_ptr);
                } else {
                    arg_data["value"] = nullptr;
                }
            } else {
                // Generic pointer, represent as hex string or null
                arg_data["value"] = reinterpret_cast<uintptr_t>(*static_cast<void**>(args[i]));
            }
        } else {
            std::cerr << "Warning: Unhandled FFI type in trampoline for argument " << i << std::endl;
            arg_data["value"] = nullptr; // Or some other default
        }
        args_json.push_back(arg_data);
    }
    event_payload["args"] = args_json;

    nlohmann::json event_json;
    event_json["event"] = "invoke_callback";
    event_json["payload"] = event_payload;

    info->ipc_server->sendEvent(event_json);

    // Handle return value if not void
    if (info->return_type != &ffi_type_void) {
        // For now, we don't expect callbacks to return values that the controller dictates
        // If a return value is needed, the controller would need to send a response
        // and the trampoline would block waiting for it. This is a complex async scenario.
        // For simplicity, we assume void return or a default value.
        // If the native code expects a return value, it will get whatever is in *ret.
        // For example, if it expects an int, it might get 0.
        if (info->return_type == &ffi_type_sint32 || info->return_type == &ffi_type_uint32) {
            *static_cast<int32_t*>(ret) = 0; // Default return for int
        } else if (info->return_type == &ffi_type_pointer) {
            *static_cast<void**>(ret) = nullptr; // Default return for pointer
        }
        // ... handle other types
    }
}
