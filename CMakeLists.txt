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

# TA compatibility layer (importers for the original game's data). Kept apart
# from the engine; the engine never depends on it.
add_library(ta_compat STATIC
    ta_compat/src/hpi.cpp
    ta_compat/src/inflate.cpp
)
target_include_directories(ta_compat PUBLIC ta_compat/include)
target_compile_options(ta_compat PRIVATE ${WARN_FLAGS})

add_executable(hpi_check ta_compat/tools/hpi_check.cpp)
target_link_libraries(hpi_check PRIVATE ta_compat)
target_compile_options(hpi_check PRIVATE ${WARN_FLAGS})

enable_testing()
add_executable(sim_tests tests/sim_tests.cpp)
target_link_libraries(sim_tests PRIVATE sim)
target_compile_options(sim_tests PRIVATE ${FP_FLAGS} ${WARN_FLAGS})
target_compile_definitions(sim_tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ta_compat/tests/fixtures")
add_test(NAME sim_tests COMMAND sim_tests)

# Tests that need an installed copy of the original game (not redistributable).
set(TA_GAME_DIR "" CACHE PATH "Path to an installed Total Annihilation 3.1 directory (optional)")
if(TA_GAME_DIR)
    add_test(NAME hpi_decode_all COMMAND hpi_check "${TA_GAME_DIR}")
endif()
