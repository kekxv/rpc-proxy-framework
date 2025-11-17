#include "struct_manager.h"
#include <stdexcept>
#include <algorithm> // For std::max

// Helper to map string type names to ffi_type pointers
static std::map<std::string, ffi_type*> get_basic_ffi_type_map() {
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
        {"string", &ffi_type_pointer}, // char*
        {"pointer", &ffi_type_pointer} // void*
    };
}

// Helper to get size of basic types
static size_t get_basic_size_map(const std::string& type_name) {
    if (type_name == "void") return 0;
    if (type_name == "int8") return sizeof(int8_t);
    if (type_name == "uint8") return sizeof(uint8_t);
    if (type_name == "int16") return sizeof(int16_t);
    if (type_name == "uint16") return sizeof(uint16_t);
    if (type_name == "int32") return sizeof(int32_t);
    if (type_name == "uint32") return sizeof(uint32_t);
    if (type_name == "int64") return sizeof(int64_t);
    if (type_name == "uint64") return sizeof(uint64_t);
    if (type_name == "float") return sizeof(float);
    if (type_name == "double") return sizeof(double);
    if (type_name == "string" || type_name == "pointer") return sizeof(void*);
    throw std::runtime_error("Unknown basic type for size: " + type_name);
}

// Helper to get alignment of basic types
static size_t get_basic_alignment_map(const std::string& type_name) {
    if (type_name == "void") return 1; // Arbitrary, doesn't matter
    if (type_name == "int8") return alignof(int8_t);
    if (type_name == "uint8") return alignof(uint8_t);
    if (type_name == "int16") return alignof(int16_t);
    if (type_name == "uint16") return alignof(uint16_t);
    if (type_name == "int32") return alignof(int32_t);
    if (type_name == "uint32") return alignof(uint32_t);
    if (type_name == "int64") return alignof(int64_t);
    if (type_name == "uint64") return alignof(uint64_t);
    if (type_name == "float") return alignof(float);
    if (type_name == "double") return alignof(double);
    if (type_name == "string" || type_name == "pointer") return alignof(void*);
    throw std::runtime_error("Unknown basic type for alignment: " + type_name);
}


StructManager::StructManager() : basic_type_map(get_basic_ffi_type_map()) {}

ffi_type* StructManager::get_ffi_type_from_string(const std::string& type_name) {
    auto it = basic_type_map.find(type_name);
    if (it != basic_type_map.end()) {
        return it->second;
    }
    // Check if it's a registered struct
    auto struct_it = registered_structs.find(type_name);
    if (struct_it != registered_structs.end()) {
        return struct_it->second.ffi_type_struct.get();
    }
    throw std::runtime_error("Unknown type: " + type_name);
}

size_t StructManager::get_size_from_string(const std::string& type_name) {
    auto it = basic_type_map.find(type_name);
    if (it != basic_type_map.end()) {
        return get_basic_size_map(type_name);
    }
    // Check if it's a registered struct
    auto struct_it = registered_structs.find(type_name);
    if (struct_it != registered_structs.end()) {
        return struct_it->second.total_size;
    }
    throw std::runtime_error("Unknown type for size calculation: " + type_name);
}

size_t StructManager::get_alignment_from_string(const std::string& type_name) {
    auto it = basic_type_map.find(type_name);
    if (it != basic_type_map.end()) {
        return get_basic_alignment_map(type_name);
    }
    // Check if it's a registered struct
    auto struct_it = registered_structs.find(type_name);
    if (struct_it != registered_structs.end()) {
        return struct_it->second.alignment;
    }
    throw std::runtime_error("Unknown type for alignment calculation: " + type_name);
}

void StructManager::register_struct(const std::string& name, const json& definition) {
    if (registered_structs.count(name)) {
        throw std::runtime_error("Struct '" + name + "' already registered.");
    }
    if (basic_type_map.count(name)) {
        throw std::runtime_error("Struct name '" + name + "' conflicts with a basic type.");
    }

    StructLayout layout;
    layout.name = name;
    layout.total_size = 0;
    layout.alignment = 1; // Minimum alignment

    std::vector<ffi_type*> ffi_member_types;
    ffi_member_types.reserve(definition.size());

    size_t current_offset = 0;
    size_t max_alignment = 1;

    for (const auto& member_json : definition) {
        StructMember member;
        member.name = member_json.at("name").get<std::string>();
        member.type_name = member_json.at("type").get<std::string>();

        member.ffi_type_ptr = get_ffi_type_from_string(member.type_name);
        member.size = get_size_from_string(member.type_name);
        member.alignment = get_alignment_from_string(member.type_name);

        // Calculate padding
        size_t padding = (member.alignment - (current_offset % member.alignment)) % member.alignment;
        current_offset += padding;
        member.offset = current_offset;

        current_offset += member.size;
        max_alignment = std::max(max_alignment, member.alignment);

        layout.members.push_back(member);
        ffi_member_types.push_back(member.ffi_type_ptr);
    }

    // Final padding for the struct itself
    size_t final_padding = (max_alignment - (current_offset % max_alignment)) % max_alignment;
    layout.total_size = current_offset + final_padding;
    layout.alignment = max_alignment;

    // Create ffi_type_struct
    layout.ffi_type_struct = std::make_unique<ffi_type>();
    layout.ffi_type_struct->size = layout.total_size;
    layout.ffi_type_struct->alignment = layout.alignment;
    layout.ffi_type_struct->type = FFI_TYPE_STRUCT;

    // Allocate and copy ffi_member_types to a null-terminated array
    layout.ffi_elements = std::make_unique<ffi_type*[]>(ffi_member_types.size() + 1);
    for (size_t i = 0; i < ffi_member_types.size(); ++i) {
        layout.ffi_elements[i] = ffi_member_types[i];
    }
    layout.ffi_elements[ffi_member_types.size()] = nullptr; // Null-terminate

    layout.ffi_type_struct->elements = layout.ffi_elements.get();

    registered_structs.emplace(name, std::move(layout));
}

void StructManager::unregister_struct(const std::string& name) {
    auto it = registered_structs.find(name);
    if (it == registered_structs.end()) {
        throw std::runtime_error("Struct '" + name + "' not found for unregistration.");
    }
    registered_structs.erase(it);
}

const StructLayout* StructManager::get_layout(const std::string& name) const {
    auto it = registered_structs.find(name);
    if (it == registered_structs.end()) {
        return nullptr;
    }
    return &it->second;
}

bool StructManager::is_struct(const std::string& type_name) const {
    return registered_structs.count(type_name);
}
