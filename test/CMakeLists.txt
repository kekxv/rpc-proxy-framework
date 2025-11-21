cmake_minimum_required(VERSION 3.15)

project(executor_tests CXX)

# 添加测试可执行文件
add_executable(executor_test executor_test.cpp)

message("CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE}")

# 链接必要的库
target_link_libraries(executor_test PRIVATE
        gtest_main
        gtest
        executor_core # 链接主项目的 executor_core 库
)

# 包含主项目的头文件
target_include_directories(executor_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../
        ${CMAKE_CURRENT_SOURCE_DIR}/../utils
        ${CMAKE_CURRENT_SOURCE_DIR}/../test_lib
)

# 添加测试
add_test(NAME executor_unit_tests COMMAND executor_test)

add_dependencies(executor_test my_lib)

if (WIN32)
    # 条件判断
    if ((CMAKE_BUILD_TYPE STREQUAL "Debug")) # 修改点
        target_compile_definitions(executor_test PRIVATE CMAKE_BUILD_TYPE="Debug")
    elseif ((CMAKE_BUILD_TYPE STREQUAL "Release")) # 修改点
        target_compile_definitions(executor_test PRIVATE CMAKE_BUILD_TYPE="Release")
    endif ()
    # 添加一个 post-build 事件
    add_custom_command(
            TARGET executor_test
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFI_BINARIES}"
            "${CMAKE_BINARY_DIR}/test/${CMAKE_BUILD_TYPE}"
            COMMENT "Copying executable to bin directory..."
    )
endif ()


# --- Multi-Client Integration Test ---

# 添加新的多客户端并发测试可执行文件
add_executable(multi_client_test multi_client_test.cpp)

# 链接与 executor_test 相同的库
target_link_libraries(multi_client_test PRIVATE
        gtest_main
        gtest
        executor_core
        )

# 包含主项目的头文件
target_include_directories(multi_client_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../
        ${CMAKE_CURRENT_SOURCE_DIR}/../utils
        ${CMAKE_CURRENT_SOURCE_DIR}/../test_lib
        )

# 添加测试
add_test(NAME multi_client_integration_test COMMAND multi_client_test)

# 确保测试库 my_lib 在测试前被构建
add_dependencies(multi_client_test my_lib)

# 为 Windows 添加 post-build 事件 (如果需要)
if (WIN32)
    if ((CMAKE_BUILD_TYPE STREQUAL "Debug"))
        target_compile_definitions(multi_client_test PRIVATE CMAKE_BUILD_TYPE="Debug")
    elseif ((CMAKE_BUILD_TYPE STREQUAL "Release"))
        target_compile_definitions(multi_client_test PRIVATE CMAKE_BUILD_TYPE="Release")
    endif ()

    add_custom_command(
            TARGET multi_client_test
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFI_BINARIES}"
            "${CMAKE_BINARY_DIR}/test/${CMAKE_BUILD_TYPE}"
            COMMENT "Copying FFI binaries for multi_client_test..."
    )
endif ()
