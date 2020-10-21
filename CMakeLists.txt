cmake_minimum_required(VERSION 3.16)
project(scalene_php)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_VERBOSE_MAKEFILE on)

add_library(scalene_php SHARED
        runtime/scalene.cpp
        runtime/scalene.h)
add_executable(c_tests
        runtime/tests/test.c)

target_include_directories(scalene_php PUBLIC
        jemalloc/include)

target_link_directories(scalene_php PUBLIC
        jemalloc/lib)

target_link_libraries(scalene_php PUBLIC
        jemalloc
        atomic)
target_link_libraries(c_tests PRIVATE
        scalene_php)

set_target_properties(c_tests PROPERTIES
        LINKER_LANGUAGE C)
