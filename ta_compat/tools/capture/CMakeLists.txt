# Standalone 32-bit Windows tool; configure separately with a Win32/x86 toolchain:
#   cmake -S ta_compat/tools/capture -B build-capture -A Win32   (or NMake from vcvars32)
cmake_minimum_required(VERSION 3.24)
project(ta_capture LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ta_capture must be built as a 32-bit Windows executable")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ta_capture capture.cpp)
target_compile_options(ta_capture PRIVATE /W4 /permissive-)
target_compile_definitions(ta_capture PRIVATE _CRT_SECURE_NO_WARNINGS)
