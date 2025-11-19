#include "ffi_dispatcher.h"
#include <ffi.h>
#include <stdexcept>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <malloc.h>
#else
#include <stdlib.h>
#endif

// Helper struct to manage memory for out/inout parameters
struct AllocatedArg
{
  int index;
  std::string type;
  std::string target_type;
  void* memory;
  size_t size;
  std::string direction;

  AllocatedArg(int idx, std::string t, std::string tt, void* mem, size_t s, std::string d)
    : index(idx), type(std::move(t)), target_type(std::move(tt)), memory(mem), size(s), direction(std::move(d))
  {
  }

  // Destructor to free allocated memory
  ~AllocatedArg()
  {
    if (memory)
    {
      if (type == "buffer" || target_type.find("[]") != std::string::npos)
      {
        delete[] static_cast<char*>(memory);
      }
      else
      {
        delete static_cast<char*>(memory);
      }
    }
  }
};


// Helper to map string type names to ffi_type pointers for basic types
static const std::map<std::string, ffi_type*>& get_basic_type_map()
{
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
    {"pointer", &ffi_type_pointer}, // void*
    {"buffer", &ffi_type_pointer} // char* for output
  };
  return type_map;
}

FfiDispatcher::FfiDispatcher(const StructManager& struct_manager, CallbackManager* callback_manager)
  : struct_manager_(struct_manager), callback_manager_(callback_manager)
{
  if (!callback_manager_)
  {
    throw std::runtime_error("FfiDispatcher requires a valid CallbackManager instance.");
  }
}

ffi_type* FfiDispatcher::get_ffi_type_for_name(const std::string& type_name) const
{
  auto it = get_basic_type_map().find(type_name);
  if (it != get_basic_type_map().end())
  {
    return it->second;
  }
  const StructLayout* layout = struct_manager_.get_layout(type_name);
  if (layout)
  {
    return layout->ffi_type_struct.get();
  }
  if (type_name == "callback")
  {
    return &ffi_type_pointer;
  }
  throw std::runtime_error("Unsupported type: " + type_name);
}

void FfiDispatcher::populate_memory_from_json(char* dest_ptr, const json& value_json, const std::string& type_name,
                                              FfiArgs& arg_storage)
{
  if (type_name == "int8") *reinterpret_cast<int8_t*>(dest_ptr) = value_json.get<int8_t>();
  else if (type_name == "uint8") *reinterpret_cast<uint8_t*>(dest_ptr) = value_json.get<uint8_t>();
  else if (type_name == "int16") *reinterpret_cast<int16_t*>(dest_ptr) = value_json.get<int16_t>();
  else if (type_name == "uint16") *reinterpret_cast<uint16_t*>(dest_ptr) = value_json.get<uint16_t>();
  else if (type_name == "int32") *reinterpret_cast<int32_t*>(dest_ptr) = value_json.get<int32_t>();
  else if (type_name == "uint32") *reinterpret_cast<uint32_t*>(dest_ptr) = value_json.get<uint32_t>();
  else if (type_name == "int64") *reinterpret_cast<int64_t*>(dest_ptr) = value_json.get<int64_t>();
  else if (type_name == "uint64") *reinterpret_cast<uint64_t*>(dest_ptr) = value_json.get<uint64_t>();
  else if (type_name == "float") *reinterpret_cast<float*>(dest_ptr) = value_json.get<float>();
  else if (type_name == "double") *reinterpret_cast<double*>(dest_ptr) = value_json.get<double>();
  else if (type_name == "string")
  {
    std::string str_val = value_json.get<std::string>();
    char* str_data = static_cast<char*>(arg_storage.allocate_string(str_val));
    *reinterpret_cast<char**>(dest_ptr) = str_data;
  }
  else if (type_name == "pointer")
  {
    *reinterpret_cast<void**>(dest_ptr) = reinterpret_cast<void*>(value_json.get<uintptr_t>());
  }
  else if (struct_manager_.is_struct(type_name))
  {
    const StructLayout* layout = struct_manager_.get_layout(type_name);
    if (!layout) throw std::runtime_error("Struct layout not found for type: " + type_name);
    for (const auto& member_layout : layout->members)
    {
      if (!value_json.count(member_layout.name))
        throw std::runtime_error(
          "Missing member '" + member_layout.name + "' in struct data for type: " + type_name);
      char* member_ptr = dest_ptr + member_layout.offset;
      populate_memory_from_json(member_ptr, value_json.at(member_layout.name), member_layout.type_name, arg_storage);
    }
  }
  else
  {
    throw std::runtime_error("Unhandled type for memory population: " + type_name);
  }
}

json FfiDispatcher::read_json_from_memory(const char* src_ptr, const std::string& type_name) const
{
  if (src_ptr == nullptr) return nullptr;
  if (type_name == "int8") return *reinterpret_cast<const int8_t*>(src_ptr);
  if (type_name == "uint8") return *reinterpret_cast<const uint8_t*>(src_ptr);
  if (type_name == "int16") return *reinterpret_cast<const int16_t*>(src_ptr);
  if (type_name == "uint16") return *reinterpret_cast<const uint16_t*>(src_ptr);
  if (type_name == "int32") return *reinterpret_cast<const int32_t*>(src_ptr);
  if (type_name == "uint32") return *reinterpret_cast<const uint32_t*>(src_ptr);
  if (type_name == "int64") return *reinterpret_cast<const int64_t*>(src_ptr);
  if (type_name == "uint64") return *reinterpret_cast<const uint64_t*>(src_ptr);
  if (type_name == "float") return *reinterpret_cast<const float*>(src_ptr);
  if (type_name == "double") return *reinterpret_cast<const double*>(src_ptr);
  if (type_name == "string") return std::string(*reinterpret_cast<char* const*>(src_ptr));
  if (type_name == "pointer") return reinterpret_cast<uintptr_t>(*reinterpret_cast<void* const*>(src_ptr));
  if (struct_manager_.is_struct(type_name))
  {
    const StructLayout* layout = struct_manager_.get_layout(type_name);
    if (!layout) throw std::runtime_error("Struct layout not found for type: " + type_name);
    json struct_json;
    for (const auto& member_layout : layout->members)
    {
      const char* member_ptr = src_ptr + member_layout.offset;
      struct_json[member_layout.name] = read_json_from_memory(member_ptr, member_layout.type_name);
    }
    return struct_json;
  }
  throw std::runtime_error("Unhandled type for JSON reading from memory: " + type_name);
}

void* FfiDispatcher::allocate_and_populate_arg(const json& arg_json, FfiArgs& arg_storage,
                                               std::vector<std::unique_ptr<AllocatedArg>>& allocated_args, int index)
{
  std::string type_str = arg_json.at("type");
  std::string direction = arg_json.value("direction", "in");

  if (direction == "out")
  {
    if (type_str == "buffer")
    {
      size_t buffer_size = arg_json.at("size").get<size_t>();
      char* buffer_mem = new char[buffer_size + sizeof(ffi_arg)](); // Zero-initialize
      allocated_args.emplace_back(
        std::make_unique<AllocatedArg>(index, type_str, "", buffer_mem, buffer_size, direction));
      return arg_storage.allocate(buffer_mem);
    }
    throw std::runtime_error("Direction 'out' is only supported for type 'buffer'");
  }

  if (direction == "inout")
  {
    if (type_str == "pointer")
    {
      std::string target_type = arg_json.at("target_type");
      ffi_type* target_ffi_type = get_ffi_type_for_name(target_type);
      char* mem = new char[target_ffi_type->size + sizeof(ffi_arg)];
      populate_memory_from_json(mem, arg_json.at("value"), target_type, arg_storage);
      allocated_args.emplace_back(
        std::make_unique<AllocatedArg>(index, type_str, target_type, mem, target_ffi_type->size, direction));
      return arg_storage.allocate(mem);
    }
    throw std::runtime_error("Direction 'inout' is only supported for type 'pointer'");
  }

  // Default "in" direction logic
  if (struct_manager_.is_struct(type_str))
  {
    const StructLayout* layout = struct_manager_.get_layout(type_str);
    char* struct_mem = static_cast<char*>(arg_storage.allocate_struct(layout->total_size,
                                                                      std::max(layout->alignment, sizeof(void*))));
    populate_memory_from_json(struct_mem, arg_json.at("value"), type_str, arg_storage);
    return struct_mem;
  }
  if (type_str == "pointer")
  {
    if (arg_json.contains("target_type"))
    {
      std::string target_type_name = arg_json.at("target_type");
      if (struct_manager_.is_struct(target_type_name))
      {
        const StructLayout* layout = struct_manager_.get_layout(target_type_name);
        char* struct_mem = static_cast<char*>(arg_storage.allocate_struct(
          layout->total_size, std::max(layout->alignment, sizeof(void*))));
        populate_memory_from_json(struct_mem, arg_json.at("value"), target_type_name, arg_storage);
        return arg_storage.allocate(struct_mem);
      }
      else if (target_type_name.back() == ']')
      {
        std::string element_type_name = target_type_name.substr(0, target_type_name.length() - 2);
        if (struct_manager_.is_struct(element_type_name))
        {
          const StructLayout* element_layout = struct_manager_.get_layout(element_type_name);
          const json& array_json = arg_json.at("value");
          if (!array_json.is_array()) throw std::runtime_error("Expected array for target_type " + target_type_name);
          size_t num_elements = array_json.size();
          size_t total_array_size = num_elements * element_layout->total_size;
          char* array_mem = static_cast<char*>(arg_storage.allocate_array(
            total_array_size, std::max(element_layout->alignment, sizeof(void*))));
          for (size_t i = 0; i < num_elements; ++i)
          {
            char* element_ptr = array_mem + (i * element_layout->total_size);
            populate_memory_from_json(element_ptr, array_json.at(i), element_type_name, arg_storage);
          }
          return arg_storage.allocate(array_mem);
        }
      }
    }
    return arg_storage.allocate(reinterpret_cast<void*>(arg_json.at("value").get<uintptr_t>()));
  }
  if (type_str == "string")
  {
    std::string str_val = arg_json.at("value").get<std::string>();
    char* str_data = static_cast<char*>(arg_storage.allocate_string(str_val));
    return arg_storage.allocate(str_data);
  }
  if (type_str == "callback")
  {
    std::string callback_id = arg_json.at("value").get<std::string>();
    void* trampoline_ptr = callback_manager_->getTrampolineFunctionPtr(callback_id);
    return arg_storage.allocate(trampoline_ptr);
  }

  // Basic types
  if (type_str == "int8") return arg_storage.allocate(arg_json.at("value").get<int8_t>());
  if (type_str == "uint8") return arg_storage.allocate(arg_json.at("value").get<uint8_t>());
  if (type_str == "int16") return arg_storage.allocate(arg_json.at("value").get<int16_t>());
  if (type_str == "uint16") return arg_storage.allocate(arg_json.at("value").get<uint16_t>());
  if (type_str == "int32") return arg_storage.allocate(arg_json.at("value").get<int32_t>());
  if (type_str == "uint32") return arg_storage.allocate(arg_json.at("value").get<uint32_t>());
  if (type_str == "int64") return arg_storage.allocate(arg_json.at("value").get<int64_t>());
  if (type_str == "uint64") return arg_storage.allocate(arg_json.at("value").get<uint64_t>());
  if (type_str == "float") return arg_storage.allocate(arg_json.at("value").get<float>());
  if (type_str == "double") return arg_storage.allocate(arg_json.at("value").get<double>());

  throw std::runtime_error("Unhandled argument type for allocation: " + type_str);
}

json FfiDispatcher::call_function(void* func_ptr, const json& payload)
{
  std::string return_type_str = payload.at("return_type");
  ffi_type* rtype = get_ffi_type_for_name(return_type_str);

  const json& args_json = payload.at("args");
  size_t arg_count = args_json.size();
  std::vector<ffi_type*> arg_types(arg_count);
  std::vector<void*> arg_values(arg_count);
  FfiArgs arg_storage;
  std::vector<std::unique_ptr<AllocatedArg>> allocated_args;

  for (size_t i = 0; i < arg_count; ++i)
  {
    const auto& arg = args_json[i];
    std::string type_str = arg.at("type");
    arg_types[i] = get_ffi_type_for_name(type_str);
    arg_values[i] = allocate_and_populate_arg(arg, arg_storage, allocated_args, i);
  }

  ffi_cif cif;
  if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, arg_count, rtype, arg_types.data()) != FFI_OK)
  {
    throw std::runtime_error("ffi_prep_cif failed");
  }

  // https://github.com/libffi/libffi/issues/946
  void* rvalue = (rtype->size > 0) ? new char[rtype->size + sizeof(ffi_arg)]() : nullptr;
  std::unique_ptr<char[]> rvalue_deleter(static_cast<char*>(rvalue));

  ffi_call(&cif, FFI_FN(func_ptr), rvalue, arg_values.data());

  json result;
  json return_val;
  return_val["type"] = return_type_str;
  if (return_type_str == "void")
  {
    return_val["value"] = nullptr;
  }
  else
  {
    return_val["value"] = read_json_from_memory(static_cast<char*>(rvalue), return_type_str);
  }
  result["return"] = return_val;

  json out_params = json::array();
  for (const auto& alloc_arg : allocated_args)
  {
    json out_param;
    out_param["index"] = alloc_arg->index;
    if (alloc_arg->type == "buffer")
    {
      out_param["type"] = "string"; // Assume out buffers are strings for now
      out_param["value"] = std::string(static_cast<char*>(alloc_arg->memory));
    }
    else if (alloc_arg->type == "pointer")
    {
      out_param["type"] = alloc_arg->target_type;
      out_param["value"] = read_json_from_memory(static_cast<char*>(alloc_arg->memory), alloc_arg->target_type);
    }
    out_params.push_back(out_param);
  }
  result["out_params"] = out_params;

  return result;
}
