cmake_minimum_required(VERSION 3.15)
project(rpc-proxy-framework CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
# include(ExternalProject) # No longer needed

# --- 查找 libffi ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFI REQUIRED libffi)

# --- 添加 JSON 库 (nlohmann/json) ---
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.2
)
FetchContent_MakeAvailable(json)


# --- 定义可执行文件 ---
add_executable(executor
  src/main.cpp
  src/executor.cpp
  src/ipc_server.cpp
  src/lib_manager.cpp
  src/ffi_dispatcher.cpp
  src/struct_manager.cpp
)

# --- 包含头文件路径 ---
target_include_directories(executor PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${FFI_INCLUDE_DIRS}
  ${json_SOURCE_DIR}/include
)

# --- 链接库 ---
target_link_libraries(executor PRIVATE
  ${FFI_LIBRARIES}
)

# --- 平台特定的链接 ---
if(UNIX AND NOT APPLE)
  # Linux 需要链接 dl
  target_link_libraries(executor PRIVATE dl)
endif()
