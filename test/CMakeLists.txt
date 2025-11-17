cmake_minimum_required(VERSION 3.15)

project(executor_tests CXX)

# 添加测试可执行文件
add_executable(executor_test executor_test.cpp)

message("CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE}")
# 条件判断
if((CMAKE_BUILD_TYPE STREQUAL "Debug")) # 修改点
    target_compile_definitions(executor_test PRIVATE CMAKE_BUILD_TYPE="Debug")
elseif((CMAKE_BUILD_TYPE STREQUAL "Release")) # 修改点
    target_compile_definitions(executor_test PRIVATE CMAKE_BUILD_TYPE="Release")
endif()

# 链接必要的库
target_link_libraries(executor_test PRIVATE
    gtest_main
    gtest
    executor_core # 链接主项目的 executor_core 库
)

# 包含主项目的头文件
target_include_directories(executor_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../test_lib
)

# 添加测试
add_test(NAME executor_unit_tests COMMAND executor_test)

add_dependencies(executor_test my_lib)

