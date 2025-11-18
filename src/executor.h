#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <string>
#include <memory> // For std::unique_ptr

#include "ipc_server.h"
#include "lib_manager.h"
#include "ffi_dispatcher.h"
#include "struct_manager.h"
#include "callback_manager.h"

class Executor {
public:
    Executor(); // Constructor to initialize managers
    void run(const std::string& pipe_name);

private:
    // Order of declaration matters for initialization/destruction
    IpcServer ipc_server_;
    StructManager struct_manager_;
    CallbackManager callback_manager_;
    LibManager lib_manager_;
    FfiDispatcher ffi_dispatcher_;

    std::string handleRequest(const std::string& request_json_str);
};

#endif // EXECUTOR_H
