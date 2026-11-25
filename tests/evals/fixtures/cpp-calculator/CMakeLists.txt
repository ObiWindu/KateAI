cmake_minimum_required(VERSION 3.20)
project(calculator CXX)

enable_testing()

add_library(calculator calculator.cpp)
add_executable(calculator_test test_calculator.cpp)
target_link_libraries(calculator_test PRIVATE calculator)
add_test(NAME calculator_test COMMAND calculator_test)
