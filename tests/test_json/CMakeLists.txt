cmake_minimum_required(VERSION 3.14)
project(test_json)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# rapidjson 在 /usr/include，gcc 默认路径，无需额外声明
# 但显式写更清晰
include_directories(/usr/include)

add_executable(test_json test_json.cpp)