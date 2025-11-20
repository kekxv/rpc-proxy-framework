cmake_minimum_required(VERSION 3.15)
project(cpp_controller_example CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

# --- 添加 JSON 库 (nlohmann/json) ---
FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)


add_executable(cpp_controller_example
        ${CMAKE_CURRENT_SOURCE_DIR}/../../utils/base64.cpp
        cpp_controller_example.cpp
)


target_include_directories(cpp_controller_example PUBLIC
        ${json_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../
)

# 平台特定的链接
if(UNIX AND NOT APPLE)
  # Linux 需要链接 dl
  target_link_libraries(cpp_controller_example PRIVATE dl)
endif()
