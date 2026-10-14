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
    engine/sim/src/battle.cpp
    engine/sim/src/combat.cpp
    engine/sim/src/fire.cpp
    engine/sim/src/footprint.cpp
    engine/sim/src/ground_settle.cpp
    engine/sim/src/heightmap.cpp
    engine/sim/src/motion.cpp
    engine/sim/src/missions.cpp
    engine/sim/src/orders.cpp
    engine/sim/src/pose.cpp
    engine/sim/src/projectile.cpp
    engine/sim/src/rng.cpp
    engine/sim/src/script_vm.cpp
    engine/sim/src/step_budget.cpp
    engine/sim/src/targeting.cpp
    engine/sim/src/trig.cpp
    engine/sim/src/unit.cpp
    engine/sim/src/unit_create.cpp
    engine/sim/src/unit_tick.cpp
    engine/sim/src/weapons.cpp
    engine/sim/src/world.cpp
    engine/sim/src/x87_math.cpp
    engine/sim/src/x87_trig.cpp
)
target_include_directories(sim PUBLIC engine/sim/include)
target_compile_options(sim PRIVATE ${FP_FLAGS} ${WARN_FLAGS})

# TA compatibility layer (importers for the original game's data). Kept apart
# from the engine; the engine never depends on it.
add_library(ta_compat STATIC
    ta_compat/src/battle_types.cpp
    ta_compat/src/cob.cpp
    ta_compat/src/crt_strtod.cpp
    ta_compat/src/featuredefs.cpp
    ta_compat/src/gaf.cpp
    ta_compat/src/hpi.cpp
    ta_compat/src/inflate.cpp
    ta_compat/src/model3do.cpp
    ta_compat/src/tdf.cpp
    ta_compat/src/tnt.cpp
    ta_compat/src/unitdefs.cpp
    ta_compat/src/weapondefs.cpp
)
target_include_directories(ta_compat PUBLIC ta_compat/include)
target_link_libraries(ta_compat PUBLIC sim)
target_compile_options(ta_compat PRIVATE ${FP_FLAGS} ${WARN_FLAGS})

foreach(tool hpi_check tdf_check vfs_cat unitdef_check weapondef_check model_check feature_check)
    add_executable(${tool} ta_compat/tools/${tool}.cpp)
    target_link_libraries(${tool} PRIVATE ta_compat)
    target_compile_options(${tool} PRIVATE ${WARN_FLAGS})
endforeach()

add_executable(fpu_profile_check tools/fpu_profile_check.cpp)
target_link_libraries(fpu_profile_check PRIVATE sim)
target_compile_options(fpu_profile_check PRIVATE ${WARN_FLAGS})

enable_testing()
add_executable(sim_tests tests/sim_tests.cpp)
target_link_libraries(sim_tests PRIVATE sim)
target_compile_options(sim_tests PRIVATE ${FP_FLAGS} ${WARN_FLAGS})
target_compile_definitions(sim_tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ta_compat/tests/fixtures")
add_test(NAME sim_tests COMMAND sim_tests)

add_executable(compat_tests tests/compat_tests.cpp)
target_link_libraries(compat_tests PRIVATE ta_compat)
target_compile_options(compat_tests PRIVATE ${WARN_FLAGS})
target_compile_definitions(compat_tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ta_compat/tests/fixtures")
add_test(NAME compat_tests COMMAND compat_tests)

# Tests that need an installed copy of the original game (not redistributable).
set(TA_GAME_DIR "" CACHE PATH "Path to an installed Total Annihilation 3.1 directory (optional)")
if(TA_GAME_DIR)
    add_test(NAME hpi_decode_all COMMAND hpi_check "${TA_GAME_DIR}")
    add_test(NAME tdf_parse_all COMMAND tdf_check "${TA_GAME_DIR}")
    add_test(NAME unitdefs_match_game COMMAND unitdef_check "${TA_GAME_DIR}"
             "${CMAKE_CURRENT_SOURCE_DIR}/ta_compat/tests/fixtures/defs_skirmish_moonquartet.bin")
    add_test(NAME weapondefs_match_game COMMAND weapondef_check "${TA_GAME_DIR}"
             "${CMAKE_CURRENT_SOURCE_DIR}/ta_compat/tests/fixtures/defs_skirmish_moonquartet.bin")
    add_test(NAME models_match_game COMMAND model_check "${TA_GAME_DIR}"
             "${CMAKE_CURRENT_SOURCE_DIR}/ta_compat/tests/fixtures/defs_skirmish_moonquartet.bin")
    add_test(NAME features_match_game COMMAND feature_check "${TA_GAME_DIR}"
             "${CMAKE_CURRENT_SOURCE_DIR}/ta_compat/tests/fixtures/defs_skirmish_moonquartet.bin" "Moon Quartet")
endif()

# Renderer (8-bit battle view), its game-data importers and the offline viewer.
add_subdirectory(render)
