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
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# --- 添加 Google Test ---
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0 # 使用一个稳定的版本
)
FetchContent_MakeAvailable(googletest)

enable_testing()

# --- 定义核心库 ---
add_library(executor_core STATIC
  src/executor.cpp
  src/ipc_server.cpp
  src/lib_manager.cpp
  src/ffi_dispatcher.cpp
  src/struct_manager.cpp
)

target_include_directories(executor_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${FFI_INCLUDE_DIRS}
  ${json_SOURCE_DIR}/include
)

target_link_libraries(executor_core PRIVATE
  ${FFI_LIBRARIES}
)

# --- 平台特定的链接 ---
if(UNIX AND NOT APPLE)
  # Linux 需要链接 dl
  target_link_libraries(executor_core PRIVATE dl)
endif()

# --- 定义可执行文件 ---
add_executable(executor
  src/main.cpp
)

# --- 包含头文件路径 ---
target_include_directories(executor PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# --- 链接库 ---
target_link_libraries(executor PRIVATE
  executor_core
)

add_subdirectory(test_lib)
add_subdirectory(test)
