# Windows 95-compatible x87 capture tool. Configure with a 32-bit MSVC toolchain:
#   cmake -S tools/fpu_capture -B build-fpucap   (from a vcvars32 prompt)
cmake_minimum_required(VERSION 3.24)
# Release only: Debug adds runtime checks that need the C runtime.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
project(fpu_capture LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "fpu_capture must be built with 32-bit MSVC (it uses inline x87 assembly)")
endif()

add_executable(pe_win95 pe_win95.cpp)

# No C runtime, no security cookie, plain x87 code, fixed base: runs on Windows 95.
add_executable(fpu_capture fpu_capture.cpp)
set_property(TARGET fpu_capture PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_compile_options(fpu_capture PRIVATE /O1 /GS- /arch:IA32 /Zl /EHs-c- /GR-)
target_link_options(fpu_capture PRIVATE /NODEFAULTLIB /ENTRY:start /SUBSYSTEM:CONSOLE /FIXED /DYNAMICBASE:NO /NXCOMPAT:NO /SAFESEH:NO)
target_link_libraries(fpu_capture PRIVATE kernel32)
add_custom_command(TARGET fpu_capture POST_BUILD COMMAND pe_win95 $<TARGET_FILE:fpu_capture>)
add_dependencies(fpu_capture pe_win95)
