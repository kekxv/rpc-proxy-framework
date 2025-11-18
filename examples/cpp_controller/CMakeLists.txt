cmake_minimum_required(VERSION 3.15)
project(cpp_controller_example CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 nlohmann/json (假设已通过 FetchContent 或其他方式引入到主项目中)
# 如果没有，您可能需要手动下载或使用 FetchContent
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(cpp_controller_example cpp_controller_example.cpp)

target_link_libraries(cpp_controller_example PRIVATE nlohmann_json::nlohmann_json)

# 平台特定的链接
if(UNIX AND NOT APPLE)
  # Linux 需要链接 dl
  target_link_libraries(cpp_controller_example PRIVATE dl)
endif()
