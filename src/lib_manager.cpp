#include "lib_manager.h"

#include <iostream>
#include <stdexcept>
#include <random>
#include <sstream>

#ifndef _WIN32
#include <dlfcn.h>
#endif

LibManager::~LibManager()
{
  for (auto const& [id, handle] : libraries)
  {
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
  }
  libraries.clear();
}

std::string LibManager::generate_uuid()
{
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<> dis(0, 15);
  static std::uniform_int_distribution<> dis2(8, 11);

  std::stringstream ss;
  ss << std::hex;
  for (int i = 0; i < 8; i++) ss << dis(gen);
  ss << "-";
  for (int i = 0; i < 4; i++) ss << dis(gen);
  ss << "-4";
  for (int i = 0; i < 3; i++) ss << dis(gen);
  ss << "-";
  ss << dis2(gen);
  for (int i = 0; i < 3; i++) ss << dis(gen);
  ss << "-";
  for (int i = 0; i < 12; i++) ss << dis(gen);
  return ss.str();
}

std::string LibManager::load_library(const std::string& path)
{
  LibraryHandle handle;
#ifdef _WIN32
  handle = LoadLibraryA(path.c_str());
#else
  handle = dlopen(path.c_str(), RTLD_LAZY);
#endif

  if (!handle)
  {
    std::cout << "load library: " << path << " fail" << std::endl;
    throw std::runtime_error("Failed to load library: " + path);
  }
  std::string library_id = "lib-" + generate_uuid();
  libraries[library_id] = handle;
  std::cout << "load library: " << path << " :" << library_id << std::endl;
  return library_id;
}

void LibManager::unload_library(const std::string& library_id)
{
  auto it = libraries.find(library_id);
  if (it == libraries.end())
  {
    throw std::runtime_error("Library not found: " + library_id);
  }

#ifdef _WIN32
  FreeLibrary(it->second);
#else
  dlclose(it->second);
#endif

  libraries.erase(it);
}

void* LibManager::get_function(const std::string& library_id, const std::string& func_name)
{
  auto it = libraries.find(library_id);
  if (it == libraries.end())
  {
    throw std::runtime_error("Library not found: " + library_id);
  }

  void* func_ptr;
#ifdef _WIN32
  func_ptr = (void*)GetProcAddress(it->second, func_name.c_str());
#else
  func_ptr = dlsym(it->second, func_name.c_str());
#endif

  if (!func_ptr)
  {
    throw std::runtime_error("Function not found: " + func_name);
  }

  return func_ptr;
}
