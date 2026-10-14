cmake_minimum_required(VERSION 3.24)
project(Engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Floating-point determinism: no fast-math, no FMA contraction. The simulation
# must produce identical results on every compiler and CPU.
if(MSVC)
    # /fp:precise does not contract to FMA unless /fp:contract is given.
    set(FP_FLAGS /fp:precise)
    set(WARN_FLAGS /W4 /permissive-)
else()
    set(FP_FLAGS -ffp-contract=off -fno-fast-math)
    set(WARN_FLAGS -Wall -Wextra -Wpedantic)
endif()

add_library(sim STATIC
    engine/sim/src/rng.cpp
    engine/sim/src/step_budget.cpp
    engine/sim/src/trig.cpp
    engine/sim/src/world.cpp
)
target_include_directories(sim PUBLIC engine/sim/include)
target_compile_options(sim PRIVATE ${FP_FLAGS} ${WARN_FLAGS})

enable_testing()
add_executable(sim_tests tests/sim_tests.cpp)
target_link_libraries(sim_tests PRIVATE sim)
target_compile_options(sim_tests PRIVATE ${FP_FLAGS} ${WARN_FLAGS})
target_compile_definitions(sim_tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ta_compat/tests/fixtures")
add_test(NAME sim_tests COMMAND sim_tests)
