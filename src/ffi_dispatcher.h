#ifndef FFI_DISPATCHER_H
#define FFI_DISPATCHER_H

#include <nlohmann/json.hpp>
#include "struct_manager.h" // Added for StructManager

using json = nlohmann::json;

class FfiDispatcher {
public:
    FfiDispatcher(const StructManager& struct_manager); // Constructor takes StructManager reference
    json call_function(void* func_ptr, const json& payload);

private:
    const StructManager& struct_manager_; // Reference to StructManager

    // Helper to get ffi_type* for a given type name (basic or registered struct)
    ffi_type* get_ffi_type_for_name(const std::string& type_name) const;
};

#endif // FFI_DISPATCHER_H
