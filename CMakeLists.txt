cmake_minimum_required(VERSION 3.15)
project(rpc-proxy-framework CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
# include(ExternalProject) # No longer needed

# --- 查找 libffi ---
if (UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFI REQUIRED libffi)
elseif (WIN32)
    # For Windows, assume libffi is provided in the project's libffi/ directory
    set(LIBFFI_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libffi")

    if (CMAKE_SIZEOF_VOID_P EQUAL 8) # 64-bit build
        set(FFI_INCLUDE_DIRS "${LIBFFI_BASE_DIR}/x86-64bit-msvc-binaries")
        set(FFI_LIBRARIES "${LIBFFI_BASE_DIR}/x86-64bit-msvc-binaries/libffi-8.lib")
        set(FFI_BINARIES "${LIBFFI_BASE_DIR}/x86-64bit-msvc-binaries/libffi-8.dll")
    else () # 32-bit build
        set(FFI_INCLUDE_DIRS "${LIBFFI_BASE_DIR}/x86-32bit-msvc-binaries")
        set(FFI_LIBRARIES "${LIBFFI_BASE_DIR}/x86-32bit-msvc-binaries/libffi-8.lib")
        set(FFI_BINARIES "${LIBFFI_BASE_DIR}/x86-32bit-msvc-binaries/libffi-8.dll")
    endif ()

    # Check if files exist
    if (NOT EXISTS "${FFI_INCLUDE_DIRS}/ffi.h")
        message(FATAL_ERROR "libffi ffi.h not found at ${FFI_INCLUDE_DIRS}/ffi.h")
    endif ()
    if (NOT EXISTS "${FFI_LIBRARIES}")
        message(FATAL_ERROR "libffi library not found at ${FFI_LIBRARIES}")
    endif ()

else ()
    message(FATAL_ERROR "Unsupported platform for libffi detection.")
endif ()

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
if (UNIX AND NOT APPLE)
    # Linux 需要链接 dl
    target_link_libraries(executor_core PRIVATE dl)
endif ()
if (WIN32)
    target_link_libraries(executor_core PRIVATE ws2_32.lib)
    # 添加一个 post-build 事件
    add_custom_command(
            TARGET executor_core
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFI_BINARIES}"
            "${CMAKE_BINARY_DIR}/"
            COMMENT "Copying executable to bin directory..."
    )
endif ()

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
