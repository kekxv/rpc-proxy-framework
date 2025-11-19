#include "callback_manager.h"
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include "uuid.h" // Using stduuid library

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
                                              const std::vector<std::string>& arg_type_names)
{
  auto info = std::make_unique<CallbackInfo>();
  info->callback_id = generateUniqueId();
  info->connection = connection_;
  info->struct_manager = struct_manager_;
  info->return_type_name = return_type_name;
  info->arg_type_names = arg_type_names;

  info->return_type = getFfiType(return_type_name);

  info->arg_types.reserve(arg_type_names.size());
  for (const auto& name : arg_type_names)
  {
    info->arg_types.push_back(getFfiType(name));
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

  nlohmann::json event_payload;
  event_payload["callback_id"] = info->callback_id;
  nlohmann::json args_json = nlohmann::json::array();

  for (size_t i = 0; i < cif->nargs; ++i)
  {
    nlohmann::json arg_data;
    const std::string& type_name = info->arg_type_names[i];
    arg_data["type"] = type_name;

    if (type_name == "int8") arg_data["value"] = *static_cast<int8_t*>(args[i]);
    else if (type_name == "uint8") arg_data["value"] = *static_cast<uint8_t*>(args[i]);
    else if (type_name == "int16") arg_data["value"] = *static_cast<int16_t*>(args[i]);
    else if (type_name == "uint16") arg_data["value"] = *static_cast<uint16_t*>(args[i]);
    else if (type_name == "int32") arg_data["value"] = *static_cast<int32_t*>(args[i]);
    else if (type_name == "uint32") arg_data["value"] = *static_cast<uint32_t*>(args[i]);
    else if (type_name == "int64") arg_data["value"] = *static_cast<int64_t*>(args[i]);
    else if (type_name == "uint64") arg_data["value"] = *static_cast<uint64_t*>(args[i]);
    else if (type_name == "float") arg_data["value"] = *static_cast<float*>(args[i]);
    else if (type_name == "double") arg_data["value"] = *static_cast<double*>(args[i]);
    else if (type_name == "string")
    {
      char* str_ptr = *static_cast<char**>(args[i]);
      arg_data["value"] = (str_ptr ? std::string(str_ptr) : nullptr);
    }
    else if (info->struct_manager->is_struct(type_name))
    {
      void* struct_ptr = *static_cast<void**>(args[i]);
      arg_data["value"] = info->struct_manager->serializeStruct(type_name, struct_ptr);
    }
    else if (type_name == "pointer")
    {
      arg_data["value"] = reinterpret_cast<uintptr_t>(*static_cast<void**>(args[i]));
    }
    else
    {
      std::cerr << "Warning: Unhandled FFI type in trampoline for argument " << i << std::endl;
      arg_data["value"] = nullptr;
    }
    args_json.push_back(arg_data);
  }
  event_payload["args"] = args_json;

  nlohmann::json event_json;
  event_json["event"] = "invoke_callback";
  event_json["payload"] = event_payload;

  info->connection->sendEvent(event_json);

  if (info->return_type != &ffi_type_void)
  {
    // For now, we assume void return or a default value for callbacks.
    memset(ret, 0, info->return_type->size);
  }
}
