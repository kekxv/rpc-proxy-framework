#ifndef LIB_MANAGER_H
#define LIB_MANAGER_H

#include <string>
#include <map>

#ifdef _WIN32
#include <windows.h>
using LibraryHandle = HMODULE;
#else
using LibraryHandle = void*;
#endif

class LibManager {
public:
    ~LibManager();

    std::string load_library(const std::string& path);
    void unload_library(const std::string& library_id);
    void* get_function(const std::string& library_id, const std::string& func_name);

private:
    std::string generate_uuid();
    std::map<std::string, LibraryHandle> libraries;
};

#endif // LIB_MANAGER_H
