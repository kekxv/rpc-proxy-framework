#ifndef RPC_PROXY_FRAMEWORK_FFI_DISPATCHER_H
#define RPC_PROXY_FRAMEWORK_FFI_DISPATCHER_H

#include <nlohmann/json.hpp>
#include "struct_manager.h"
#include "callback_manager.h" // Include CallbackManager
#include <ffi.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint> // Added for uintptr_t
#include <cstring> // For memcpy

using json = nlohmann::json;

// Forward declarations
class CallbackManager;
struct AllocatedArg;

// RAII wrapper for argument memory.
// This class ensures that all memory allocated for FFI arguments is
// correctly deallocated, even for complex types like strings and structs.
class FfiArgs {
public:
    ~FfiArgs() {
        for (auto& ptr : new_allocated_storage) {
            delete[] ptr;
        }
        for (auto& ptr : aligned_allocated_storage) {
#ifdef _WIN32
            _aligned_free(ptr);
#else
            free(ptr); // posix_memalign uses free
#endif
        }
    }

    // Allocates memory for any POD type and stores it.
    template<typename T>
    void* allocate(T value) {
        char* buffer = new char[sizeof(T)];
        *reinterpret_cast<T*>(buffer) = value;
        new_allocated_storage.push_back(buffer);
        return buffer;
    }
    
    // Allocates memory for a string and copies the value.
    void* allocate_string(const std::string& value) {
        char* str_mem = new char[value.length() + 1];
        std::copy(value.begin(), value.end(), str_mem);
        str_mem[value.length()] = '\0';
        new_allocated_storage.push_back(str_mem);
        return str_mem; // Return the pointer to the string data.
    }

    // Allocates memory for a struct of a given size and alignment using platform-specific aligned allocation.
    void* allocate_struct(size_t size, size_t alignment) {
        void* buffer = nullptr;
#ifdef _WIN32
        buffer = _aligned_malloc(size, alignment);
#else
        // posix_memalign requires alignment to be a power of 2 and a multiple of sizeof(void*)
        // We assume alignment from StructManager is already valid.
        if (posix_memalign(&buffer, alignment, size) != 0) {
            buffer = nullptr; // Indicate allocation failure
        }
#endif
        if (!buffer) {
            throw std::bad_alloc();
        }
        std::memset(buffer, 0, size); // Initialize to zero
        aligned_allocated_storage.push_back(buffer);
        return buffer;
    }

    // Allocates memory for an array of elements
    void* allocate_array(size_t total_size, size_t alignment) {
        return allocate_struct(total_size, alignment); // Reuse struct allocation for aligned memory
    }

    // Allows external code to register a pointer for management by FfiArgs
    void add_managed_ptr(char* ptr) {
        new_allocated_storage.push_back(ptr);
    }

private:
    std::vector<char*> new_allocated_storage; // For memory allocated with 'new'
    std::vector<void*> aligned_allocated_storage; // For memory allocated with aligned functions
};

class FfiDispatcher {
public:
    FfiDispatcher(const StructManager& struct_manager, CallbackManager* callback_manager);

    json call_function(void* func_ptr, const json& payload);

private:
    const StructManager& struct_manager_;
    CallbackManager* callback_manager_; // Not owned

    ffi_type* get_ffi_type_for_name(const std::string& type_name) const;
    void populate_memory_from_json(char* dest_ptr, const json& value_json, const std::string& type_name, FfiArgs& arg_storage);
    json read_json_from_memory(const char* src_ptr, const std::string& type_name) const;
    void* allocate_and_populate_arg(const json& arg_json, FfiArgs& arg_storage, std::vector<std::unique_ptr<AllocatedArg>>& allocated_args, int index);
};

#endif //RPC_PROXY_FRAMEWORK_FFI_DISPATCHER_H