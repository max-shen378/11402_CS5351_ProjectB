cmake_minimum_required(VERSION 3.15)
project(minidb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(minidb src/main.cpp)
add_executable(run_tests tests/test_main.cpp)

enable_testing()
# Tests load data/Iris.csv relative to the working directory.
add_test(NAME unit COMMAND run_tests ${CMAKE_SOURCE_DIR}/data)
