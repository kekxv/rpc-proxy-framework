#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <string>
#include "struct_manager.h" // Added for StructManager

class Executor {
public:
    void run(const std::string& pipe_name);

private:
    StructManager struct_manager_; // Member to manage struct definitions
};

#endif // EXECUTOR_H
