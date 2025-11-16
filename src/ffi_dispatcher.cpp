#include "ffi_dispatcher.h"
#include <ffi.h>
#include <stdexcept>
#include <vector>
#include <map>
#include <memory>
#include <cstdint> // Added for uintptr_t

// Helper to map string type names to ffi_type pointers
static std::map<std::string, ffi_type*> get_type_map() {
    return {
        {"void", &ffi_type_void},
        {"int8", &ffi_type_sint8},
        {"uint8", &ffi_type_uint8},
        {"int16", &ffi_type_sint16},
        {"uint16", &ffi_type_uint16},
        {"int32", &ffi_type_sint32},
        {"uint32", &ffi_type_uint32},
        {"int64", &ffi_type_sint64},
        {"uint64", &ffi_type_uint64},
        {"float", &ffi_type_float},
        {"double", &ffi_type_double},
        {"string", &ffi_type_pointer},
        {"pointer", &ffi_type_pointer}
    };
}

// RAII wrapper for argument memory.
// This class ensures that all memory allocated for FFI arguments is
// correctly deallocated, even for complex types like strings.
class FfiArgs {
public:
    ~FfiArgs() {
        for (auto& ptr : storage) {
            delete[] ptr;
        }
    }

    // Allocates memory for any POD type and stores it.
    template<typename T>
    void* allocate(T value) {
        char* buffer = new char[sizeof(T)];
        *reinterpret_cast<T*>(buffer) = value;
        storage.push_back(buffer);
        return buffer;
    }
    
    // Allocates memory for a string and a pointer to that string.
    void* allocate_string(const std::string& value) {
        // 1. Allocate and store the string data itself.
        char* str_mem = new char[value.length() + 1];
        std::copy(value.begin(), value.end(), str_mem);
        str_mem[value.length()] = '\0';
        storage.push_back(str_mem);

        // 2. Allocate and store a pointer that points to the string data.
        // ffi_call for a string argument needs a `char**`.
        char* ptr_mem = new char[sizeof(char*)];
        *reinterpret_cast<char**>(ptr_mem) = str_mem;
        storage.push_back(ptr_mem);

        return ptr_mem; // Return the pointer to the pointer.
    }

private:
    std::vector<char*> storage;
};


json FfiDispatcher::call_function(void* func_ptr, const json& payload) {
    static const auto type_map = get_type_map();

    // 1. Get return type
    std::string return_type_str = payload.at("return_type");
    auto it_return = type_map.find(return_type_str);
    if (it_return == type_map.end()) {
        throw std::runtime_error("Unsupported return type: " + return_type_str);
    }
    ffi_type* rtype = it_return->second;

    // 2. Prepare arguments
    const json& args_json = payload.at("args");
    size_t arg_count = args_json.size();
    std::vector<ffi_type*> arg_types(arg_count);
    std::vector<void*> arg_values(arg_count);
    FfiArgs arg_storage; // Manages memory for arg values

    for (size_t i = 0; i < arg_count; ++i) {
        const auto& arg = args_json[i];
        std::string type_str = arg.at("type");
        auto it_arg = type_map.find(type_str);
        if (it_arg == type_map.end()) {
            throw std::runtime_error("Unsupported argument type: " + type_str);
        }
        arg_types[i] = it_arg->second;

        if (type_str == "int8") arg_values[i] = arg_storage.allocate(arg.at("value").get<int8_t>());
        else if (type_str == "uint8") arg_values[i] = arg_storage.allocate(arg.at("value").get<uint8_t>());
        else if (type_str == "int16") arg_values[i] = arg_storage.allocate(arg.at("value").get<int16_t>());
        else if (type_str == "uint16") arg_values[i] = arg_storage.allocate(arg.at("value").get<uint16_t>());
        else if (type_str == "int32") arg_values[i] = arg_storage.allocate(arg.at("value").get<int32_t>());
        else if (type_str == "uint32") arg_values[i] = arg_storage.allocate(arg.at("value").get<uint32_t>());
        else if (type_str == "int64") arg_values[i] = arg_storage.allocate(arg.at("value").get<int64_t>());
        else if (type_str == "uint64") arg_values[i] = arg_storage.allocate(arg.at("value").get<uint64_t>());
        else if (type_str == "float") arg_values[i] = arg_storage.allocate(arg.at("value").get<float>());
        else if (type_str == "double") arg_values[i] = arg_storage.allocate(arg.at("value").get<double>());
        else if (type_str == "string") arg_values[i] = arg_storage.allocate_string(arg.at("value").get<std::string>());
        else if (type_str == "pointer") arg_values[i] = arg_storage.allocate(reinterpret_cast<void*>(arg.at("value").get<uintptr_t>()));
        else throw std::runtime_error("Unhandled argument type for allocation: " + type_str);
    }

    // 3. Prepare CIF
    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, arg_count, rtype, arg_types.data()) != FFI_OK) {
        throw std::runtime_error("ffi_prep_cif failed");
    }

    // 4. Allocate return value memory and call
    void* rvalue;
    if (rtype->size > 0) {
        rvalue = new char[rtype->size];
    } else {
        rvalue = nullptr;
    }
    
    std::unique_ptr<char[]> rvalue_deleter(static_cast<char*>(rvalue));

    ffi_call(&cif, FFI_FN(func_ptr), rvalue, arg_values.data());

    // 5. Package result
    json result;
    result["type"] = return_type_str;
    if (return_type_str == "void") result["value"] = nullptr;
    else if (return_type_str == "int8") result["value"] = *static_cast<int8_t*>(rvalue);
    else if (return_type_str == "uint8") result["value"] = *static_cast<uint8_t*>(rvalue);
    else if (return_type_str == "int16") result["value"] = *static_cast<int16_t*>(rvalue);
    else if (return_type_str == "uint16") result["value"] = *static_cast<uint16_t*>(rvalue);
    else if (return_type_str == "int32") result["value"] = *static_cast<int32_t*>(rvalue);
    else if (return_type_str == "uint32") result["value"] = *static_cast<uint32_t*>(rvalue);
    else if (return_type_str == "int64") result["value"] = *static_cast<int64_t*>(rvalue);
    else if (return_type_str == "uint64") result["value"] = *static_cast<uint64_t*>(rvalue);
    else if (return_type_str == "float") result["value"] = *static_cast<float*>(rvalue);
    else if (return_type_str == "double") result["value"] = *static_cast<double*>(rvalue);
    else if (return_type_str == "string") result["value"] = std::string(*static_cast<char**>(rvalue));
    else if (return_type_str == "pointer") result["value"] = reinterpret_cast<uintptr_t>(*static_cast<void**>(rvalue));
    else throw std::runtime_error("Unhandled return type for packaging: " + return_type_str);

    return result;
}
