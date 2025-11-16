cmake_minimum_required(VERSION 3.10)
project(my_lib C)

add_library(my_lib SHARED my_lib.c)

# 设置输出目录
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# 移除 'lib' 前缀，使库名在各平台统一为 my_lib.so/my_lib.dylib/my_lib.dll
set_target_properties(my_lib PROPERTIES PREFIX "")
