# 32-bit Windows differential tester that runs routines from the original
# executable in-process. Configure separately with a 32-bit toolchain:
#   cmake -S ta_compat/tools/orig_harness -B build-harness   (from a vcvars32 prompt)
cmake_minimum_required(VERSION 3.24)
project(orig_harness LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "orig_harness must be built as a 32-bit Windows executable")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_executable(orig_harness
    orig_harness.cpp
    ${ROOT}/ta_compat/src/crt_strtod.cpp
    ${ROOT}/ta_compat/src/hpi.cpp
    ${ROOT}/ta_compat/src/inflate.cpp
    ${ROOT}/ta_compat/src/tdf.cpp
)
target_include_directories(orig_harness PRIVATE ${ROOT}/ta_compat/include ${ROOT}/engine/sim/include)
target_compile_options(orig_harness PRIVATE /W4 /permissive- /fp:precise)
