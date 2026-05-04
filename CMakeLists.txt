cmake_minimum_required(VERSION 3.14)
project(Pegasus VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(/usr/include)

# 后续加模块在这里追加
add_subdirectory(tests/test_json)