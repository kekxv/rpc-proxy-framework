cmake_minimum_required(VERSION 3.15)
project(cpp_controller_example CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

# --- Add JsonCpp ---
FetchContent_Declare(
  jsoncpp
  GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
  GIT_TAG        1.9.6
)
FetchContent_MakeAvailable(jsoncpp)


add_executable(cpp_controller_example
        ${CMAKE_CURRENT_SOURCE_DIR}/../../utils/base64.cpp
        cpp_controller_example.cpp
)


target_include_directories(cpp_controller_example PUBLIC
        ${jsoncpp_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../
)

target_link_libraries(cpp_controller_example PRIVATE jsoncpp_lib_static)

# 平台特定的链接
if(UNIX AND NOT APPLE)
  # Linux 需要链接 dl
  target_link_libraries(cpp_controller_example PRIVATE dl)
endif()
