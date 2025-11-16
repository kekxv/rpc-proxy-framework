#include "ffi_dispatcher.h"
#include <ffi.h>
#include <stdexcept>
#include <vector>
#include <map>
#include <memory>
#include <cstdint> // Added for uintptr_t
#include <cstring> // For memcpy

// Helper to map string type names to ffi_type pointers for basic types
static const std::map<std::string, ffi_type*>& get_basic_type_map() {
    static const std::map<std::string, ffi_type*> type_map = {
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
        {"string", &ffi_type_pointer}, // char*
        {"pointer", &ffi_type_pointer} // void*
    };
    return type_map;
}

// RAII wrapper for argument memory.
// This class ensures that all memory allocated for FFI arguments is
// correctly deallocated, even for complex types like strings and structs.
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

    // Allocates memory for a struct of a given size.
    void* allocate_struct(size_t size) {
        char* buffer = new char[size];
        // Initialize to zero to avoid garbage values
        std::memset(buffer, 0, size); 
        storage.push_back(buffer);
        return buffer;
    }

    // Allows external code to register a pointer for management by FfiArgs
    void add_managed_ptr(char* ptr) {
        storage.push_back(ptr);
    }

private:
    std::vector<char*> storage;
};

// FfiDispatcher implementation
FfiDispatcher::FfiDispatcher(const StructManager& struct_manager)
    : struct_manager_(struct_manager) {}

ffi_type* FfiDispatcher::get_ffi_type_for_name(const std::string& type_name) const {
    // Check basic types first
    auto it = get_basic_type_map().find(type_name);
    if (it != get_basic_type_map().end()) {
        return it->second;
    }
    // Check registered structs
    const StructLayout* layout = struct_manager_.get_layout(type_name);
    if (layout) {
        return layout->ffi_type_struct.get();
    }
    throw std::runtime_error("Unsupported type: " + type_name);
}

json FfiDispatcher::call_function(void* func_ptr, const json& payload) {
    // 1. Get return type
    std::string return_type_str = payload.at("return_type");
    ffi_type* rtype = get_ffi_type_for_name(return_type_str);

    // 2. Prepare arguments
    const json& args_json = payload.at("args");
    size_t arg_count = args_json.size();
    std::vector<ffi_type*> arg_types(arg_count);
    std::vector<void*> arg_values(arg_count);
    FfiArgs arg_storage; // Manages memory for arg values

    for (size_t i = 0; i < arg_count; ++i) {
        const auto& arg = args_json[i];
        std::string type_str = arg.at("type");
        arg_types[i] = get_ffi_type_for_name(type_str);

        if (struct_manager_.is_struct(type_str)) {
            // Handle struct argument
            const StructLayout* layout = struct_manager_.get_layout(type_str);
            if (!layout) {
                throw std::runtime_error("Struct layout not found for type: " + type_str);
            }
            char* struct_mem = static_cast<char*>(arg_storage.allocate_struct(layout->total_size));
            
            const json& struct_data = arg.at("value");
            for (const auto& member_layout : layout->members) {
                if (!struct_data.count(member_layout.name)) {
                    throw std::runtime_error("Missing member '" + member_layout.name + "' in struct data for type: " + type_str);
                }
                const json& member_value_json = struct_data.at(member_layout.name);
                char* member_ptr = struct_mem + member_layout.offset;

                // Copy member value based on its type
                if (member_layout.type_name == "int8") *reinterpret_cast<int8_t*>(member_ptr) = member_value_json.get<int8_t>();
                else if (member_layout.type_name == "uint8") *reinterpret_cast<uint8_t*>(member_ptr) = member_value_json.get<uint8_t>();
                else if (member_layout.type_name == "int16") *reinterpret_cast<int16_t*>(member_ptr) = member_value_json.get<int16_t>();
                else if (member_layout.type_name == "uint16") *reinterpret_cast<uint16_t*>(member_ptr) = member_value_json.get<uint16_t>();
                else if (member_layout.type_name == "int32") *reinterpret_cast<int32_t*>(member_ptr) = member_value_json.get<int32_t>();
                else if (member_layout.type_name == "uint32") *reinterpret_cast<uint32_t*>(member_ptr) = member_value_json.get<uint32_t>();
                else if (member_layout.type_name == "int64") *reinterpret_cast<int64_t*>(member_ptr) = member_value_json.get<int64_t>();
                else if (member_layout.type_name == "uint64") *reinterpret_cast<uint64_t*>(member_ptr) = member_value_json.get<uint64_t>();
                else if (member_layout.type_name == "float") *reinterpret_cast<float*>(member_ptr) = member_value_json.get<float>();
                else if (member_layout.type_name == "double") *reinterpret_cast<double*>(member_ptr) = member_value_json.get<double>();
                else if (member_layout.type_name == "string") {
                    // For string members in a struct, we need to allocate the string data
                    // and store a pointer to it within the struct.
                    std::string str_val = member_value_json.get<std::string>();
                    char* str_data = new char[str_val.length() + 1];
                    std::copy(str_val.begin(), str_val.end(), str_data);
                    str_data[str_val.length()] = '\0';
                    arg_storage.add_managed_ptr(str_data); // Managed by FfiArgs
                    *reinterpret_cast<char**>(member_ptr) = str_data;
                }
                else if (member_layout.type_name == "pointer") *reinterpret_cast<void**>(member_ptr) = reinterpret_cast<void*>(member_value_json.get<uintptr_t>());
                else {
                    throw std::runtime_error("Unhandled member type for struct population: " + member_layout.type_name);
                }
            }
            arg_values[i] = struct_mem;

        } else {
            // Handle basic type argument
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
        std::memset(rvalue, 0, rtype->size); // Initialize to zero
    } else {
        rvalue = nullptr;
    }
    
    std::unique_ptr<char[]> rvalue_deleter(static_cast<char*>(rvalue));

    ffi_call(&cif, FFI_FN(func_ptr), rvalue, arg_values.data());

    // 5. Package result
    json result;
    result["type"] = return_type_str;
    if (return_type_str == "void") {
        result["value"] = nullptr;
    } else if (struct_manager_.is_struct(return_type_str)) {
        // Handle struct return value
        const StructLayout* layout = struct_manager_.get_layout(return_type_str);
        if (!layout) {
            throw std::runtime_error("Struct layout not found for return type: " + return_type_str);
        }
        json struct_result_data;
        char* struct_mem = static_cast<char*>(rvalue);

        for (const auto& member_layout : layout->members) {
            char* member_ptr = struct_mem + member_layout.offset;
            if (member_layout.type_name == "int8") struct_result_data[member_layout.name] = *reinterpret_cast<int8_t*>(member_ptr);
            else if (member_layout.type_name == "uint8") struct_result_data[member_layout.name] = *reinterpret_cast<uint8_t*>(member_ptr);
            else if (member_layout.type_name == "int16") struct_result_data[member_layout.name] = *reinterpret_cast<int16_t*>(member_ptr);
            else if (member_layout.type_name == "uint16") struct_result_data[member_layout.name] = *reinterpret_cast<uint16_t*>(member_ptr);
            else if (member_layout.type_name == "int32") struct_result_data[member_layout.name] = *reinterpret_cast<int32_t*>(member_ptr);
            else if (member_layout.type_name == "uint32") struct_result_data[member_layout.name] = *reinterpret_cast<uint32_t*>(member_ptr);
            else if (member_layout.type_name == "int64") struct_result_data[member_layout.name] = *reinterpret_cast<int64_t*>(member_ptr);
            else if (member_layout.type_name == "uint64") struct_result_data[member_layout.name] = *reinterpret_cast<uint64_t*>(member_ptr);
            else if (member_layout.type_name == "float") struct_result_data[member_layout.name] = *reinterpret_cast<float*>(member_ptr);
            else if (member_layout.type_name == "double") struct_result_data[member_layout.name] = *reinterpret_cast<double*>(member_ptr);
            else if (member_layout.type_name == "string") struct_result_data[member_layout.name] = std::string(*reinterpret_cast<char**>(member_ptr));
            else if (member_layout.type_name == "pointer") struct_result_data[member_layout.name] = reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(member_ptr));
            else {
                throw std::runtime_error("Unhandled member type for struct return packaging: " + member_layout.type_name);
            }
        }
        result["value"] = struct_result_data;
    }
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
