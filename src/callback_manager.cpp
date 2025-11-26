#include "callback_manager.h"
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include "uuid.h" // Using stduuid library
#include "base64.h"

// Helper to convert ffi_type* to string for JSON serialization
std::string ffiTypeToString(ffi_type* type)
{
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
  return "unknown";
}

CallbackManager::CallbackManager(ClientConnection* connection, StructManager* struct_manager)
  : connection_(connection), struct_manager_(struct_manager)
{
  if (!connection_ || !struct_manager_)
  {
    throw std::runtime_error("CallbackManager requires valid ClientConnection and StructManager instances.");
  }
}

CallbackManager::~CallbackManager()
{
  for (auto const& [id, info] : registered_callbacks_)
  {
    ffi_closure_free(info->closure);
  }
}

std::string CallbackManager::generateUniqueId()
{
  std::random_device rd;
  auto seed_data = std::array<int, std::mt19937::state_size>{};
  std::generate(std::begin(seed_data), std::end(seed_data), std::ref(rd));
  std::seed_seq seq(std::begin(seed_data), std::end(seed_data));
  std::mt19937 generator(seq);
  uuids::uuid_random_generator gen{generator};
  uuids::uuid id = gen();
  return "cb-" + uuids::to_string(id);
}

ffi_type* CallbackManager::getFfiType(const std::string& type_name)
{
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
  if (type_name == "string" || type_name == "pointer" || type_name == "callback") return &ffi_type_pointer;

  if (struct_manager_->is_struct(type_name))
  {
    return struct_manager_->get_layout(type_name)->ffi_type_struct.get();
  }

  throw std::runtime_error("Unknown FFI type: " + type_name);
}

std::string CallbackManager::registerCallback(const std::string& return_type_name,
                                              const Json::Value& args_type_def)
{
  auto info = std::make_unique<CallbackInfo>();
  info->callback_id = generateUniqueId();
  info->connection = connection_;
  info->struct_manager = struct_manager_;
  info->return_type_name = return_type_name;

  info->return_type = getFfiType(return_type_name);

  if (!args_type_def.isArray()) {
      throw std::runtime_error("args_type must be an array");
  }

  info->arg_types.reserve(args_type_def.size());
  info->args_info.reserve(args_type_def.size());

  for (const auto& arg_def : args_type_def)
  {
    CallbackArgInfo arg_info;
    if (arg_def.isString()) {
        arg_info.type_name = arg_def.asString();
        arg_info.ffi_type_ptr = getFfiType(arg_info.type_name);
    } else if (arg_def.isObject()) {
        std::string type = arg_def["type"].asString();
        if (type == "buffer_ptr") {
            arg_info.type_name = "buffer_ptr";
            // buffer_ptr is physically a pointer
            arg_info.ffi_type_ptr = &ffi_type_pointer; 
            if (arg_def.isMember("size_arg_index")) {
                arg_info.size_arg_index = arg_def["size_arg_index"].asInt();
            } else if (arg_def.isMember("fixed_size")) {
                arg_info.fixed_size = arg_def["fixed_size"].asInt();
            } else {
                 throw std::runtime_error("buffer_ptr requires either size_arg_index or fixed_size");
            }
        } else {
             throw std::runtime_error("Unknown complex argument type in callback: " + type);
        }
    } else {
        throw std::runtime_error("Invalid argument definition in callback args_type");
    }

    info->args_info.push_back(arg_info);
    info->arg_types.push_back(arg_info.ffi_type_ptr);
  }

  ffi_status status = ffi_prep_cif(&info->cif, FFI_DEFAULT_ABI,
                                   info->arg_types.size(), info->return_type,
                                   info->arg_types.data());
  if (status != FFI_OK)
  {
    throw std::runtime_error("Failed to prepare CIF for callback: " + std::to_string(status));
  }

  info->closure = static_cast<ffi_closure*>(ffi_closure_alloc(sizeof(ffi_closure), &info->trampoline_function_ptr));
  if (!info->closure)
  {
    throw std::runtime_error("Failed to allocate ffi_closure for callback.");
  }

  status = ffi_prep_closure_loc(info->closure, &info->cif, CallbackManager::ffi_trampoline, info.get(),
                                info->trampoline_function_ptr);
  if (status != FFI_OK)
  {
    ffi_closure_free(info->closure);
    throw std::runtime_error("Failed to prepare ffi_closure_loc for callback: " + std::to_string(status));
  }

  std::string id = info->callback_id;
  registered_callbacks_[id] = std::move(info);
  return id;
}

void CallbackManager::unregisterCallback(const std::string& callback_id)
{
  auto it = registered_callbacks_.find(callback_id);
  if (it != registered_callbacks_.end())
  {
    ffi_closure_free(it->second->closure);
    registered_callbacks_.erase(it);
  }
  else
  {
    throw std::runtime_error("Callback with ID " + callback_id + " not found.");
  }
}

void* CallbackManager::getTrampolineFunctionPtr(const std::string& callback_id)
{
  auto it = registered_callbacks_.find(callback_id);
  if (it != registered_callbacks_.end())
  {
    return it->second->trampoline_function_ptr;
  }
  throw std::runtime_error("Callback with ID " + callback_id + " not found.");
}

void CallbackManager::ffi_trampoline(ffi_cif* cif, void* ret, void** args, void* userdata)
{
  CallbackInfo* info = static_cast<CallbackInfo*>(userdata);
  if (!info || !info->connection)
  {
    std::cerr << "Error: CallbackInfo or ClientConnection not available in trampoline." << std::endl;
    return;
  }

  Json::Value event_payload;
  event_payload["callback_id"] = info->callback_id;
  Json::Value args_json(Json::arrayValue);

  // We need to be able to access argument values by index to resolve sizes.
  // args[i] is a pointer to the value.
  auto get_int_arg_val = [&](int index) -> int64_t {
      if (index < 0 || index >= (int)info->args_info.size()) return 0;
      const auto& arg_info = info->args_info[index];
      void* val_ptr = args[index];
      
      if (arg_info.type_name == "int8") return *static_cast<int8_t*>(val_ptr);
      if (arg_info.type_name == "uint8") return *static_cast<uint8_t*>(val_ptr);
      if (arg_info.type_name == "int16") return *static_cast<int16_t*>(val_ptr);
      if (arg_info.type_name == "uint16") return *static_cast<uint16_t*>(val_ptr);
      if (arg_info.type_name == "int32") return *static_cast<int32_t*>(val_ptr);
      if (arg_info.type_name == "uint32") return *static_cast<uint32_t*>(val_ptr);
      if (arg_info.type_name == "int64") return *static_cast<int64_t*>(val_ptr);
      if (arg_info.type_name == "uint64") return *static_cast<uint64_t*>(val_ptr); // Cast to int64 for simplicity
      return 0;
  };

  for (size_t i = 0; i < cif->nargs; ++i)
  {
    Json::Value arg_data;
    const auto& arg_info = info->args_info[i];
    const std::string& type_name = arg_info.type_name;
    arg_data["type"] = type_name;

    if (type_name == "int8") arg_data["value"] = *static_cast<int8_t*>(args[i]);
    else if (type_name == "uint8") arg_data["value"] = *static_cast<uint8_t*>(args[i]);
    else if (type_name == "int16") arg_data["value"] = *static_cast<int16_t*>(args[i]);
    else if (type_name == "uint16") arg_data["value"] = *static_cast<uint16_t*>(args[i]);
    else if (type_name == "int32") arg_data["value"] = *static_cast<int32_t*>(args[i]);
    else if (type_name == "uint32") arg_data["value"] = *static_cast<uint32_t*>(args[i]);
    else if (type_name == "int64") arg_data["value"] = (Json::Int64)*static_cast<int64_t*>(args[i]);
    else if (type_name == "uint64") arg_data["value"] = (Json::UInt64)*static_cast<uint64_t*>(args[i]);
    else if (type_name == "float") arg_data["value"] = *static_cast<float*>(args[i]);
    else if (type_name == "double") arg_data["value"] = *static_cast<double*>(args[i]);
    else if (type_name == "string")
    {
      char* str_ptr = *static_cast<char**>(args[i]);
      if (str_ptr) arg_data["value"] = std::string(str_ptr);
      else arg_data["value"] = Json::nullValue;
    }
    else if (info->struct_manager->is_struct(type_name))
    {
      void* struct_ptr = *static_cast<void**>(args[i]);
      arg_data["value"] = info->struct_manager->serializeStruct(type_name, struct_ptr);
    }
    else if (type_name == "pointer")
    {
      arg_data["value"] = (Json::UInt64)reinterpret_cast<uintptr_t>(*static_cast<void**>(args[i]));
    }
    else if (type_name == "buffer_ptr")
    {
        // Dynamic buffer handling
        void* ptr = *static_cast<void**>(args[i]);
        if (ptr) {
            int64_t size = 0;
            if (arg_info.size_arg_index >= 0) {
                size = get_int_arg_val(arg_info.size_arg_index);
            } else if (arg_info.fixed_size > 0) {
                size = arg_info.fixed_size;
            }
            
            if (size > 0) {
                arg_data["value"] = base64_encode(static_cast<unsigned char*>(ptr), (size_t)size);
                arg_data["size"] = (Json::UInt64)size;
            } else {
                arg_data["value"] = "";
                arg_data["size"] = 0;
            }
        } else {
            arg_data["value"] = Json::nullValue;
        }
    }
    else
    {
      std::cerr << "Warning: Unhandled FFI type in trampoline for argument " << i << std::endl;
      arg_data["value"] = Json::nullValue;
    }
    args_json.append(arg_data);
  }
  event_payload["args"] = args_json;

  Json::Value event_json;
  event_json["event"] = "invoke_callback";
  event_json["payload"] = event_payload;

  info->connection->sendEvent(event_json);

  if (info->return_type != &ffi_type_void)
  {
    // For now, we assume void return or a default value for callbacks.
    memset(ret, 0, info->return_type->size);
  }
}
