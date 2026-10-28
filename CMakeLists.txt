cmake_minimum_required(VERSION 3.16)
project(sabtree LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(sabtree INTERFACE)
target_include_directories(sabtree INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_executable(test_bplus_tree tests/test_bplus_tree.cpp)
target_link_libraries(test_bplus_tree PRIVATE sabtree)
add_test(NAME test_bplus_tree COMMAND test_bplus_tree)
