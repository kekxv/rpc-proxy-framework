#ifndef FFI_DISPATCHER_H
#define FFI_DISPATCHER_H

#include <nlohmann/json.hpp>

using json = nlohmann::json;

class FfiDispatcher {
public:
    json call_function(void* func_ptr, const json& payload);
};

#endif // FFI_DISPATCHER_H
