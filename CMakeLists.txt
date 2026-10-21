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
    # -fwrapv: signed overflow wraps like the original's add/imul. The code already routes
    # arithmetic that can wrap through unsigned helpers (docs/parity/NUMERICS_FIXED.md); this
    # is the safety net for any site missed. MSVC has no equivalent switch.
    set(FP_FLAGS -ffp-contract=off -fno-fast-math -fwrapv)
    set(WARN_FLAGS -Wall -Wextra -Wpedantic)
endif()

add_library(sim STATIC
    engine/sim/src/ai.cpp
    engine/sim/src/anim.cpp
    engine/sim/src/battle.cpp
    engine/sim/src/battle_start.cpp
    engine/sim/src/bitstream.cpp
    engine/sim/src/commands.cpp
    engine/sim/src/commands_core.cpp
    engine/sim/src/console.cpp
    engine/sim/src/effects.cpp
    engine/sim/src/effect_lists.cpp
    engine/sim/src/features.cpp
    engine/sim/src/combat.cpp
    engine/sim/src/economy.cpp
    engine/sim/src/fire.cpp
    engine/sim/src/footprint.cpp
    engine/sim/src/ground_settle.cpp
    engine/sim/src/heightmap.cpp
    engine/sim/src/lockstep.cpp
    engine/sim/src/motion.cpp
    engine/sim/src/net_states.cpp
    engine/sim/src/net_units.cpp
    engine/sim/src/missions.cpp
    engine/sim/src/mission_goals.cpp
    engine/sim/src/construction.cpp
    engine/sim/src/unit_missions_common.cpp
    engine/sim/src/air_missions.cpp
    engine/sim/src/ground_missions.cpp
    engine/sim/src/nano_spray.cpp
    engine/sim/src/orders.cpp
    engine/sim/src/pose.cpp
    engine/sim/src/projectile.cpp
    engine/sim/src/route_search.cpp
    engine/sim/src/rng.cpp
    engine/sim/src/script_vm.cpp
    engine/sim/src/sight.cpp
    engine/sim/src/sky.cpp
    engine/sim/src/step_budget.cpp
    engine/sim/src/targeting.cpp
    engine/sim/src/trig.cpp
    engine/sim/src/unit.cpp
    engine/sim/src/unit_create.cpp
    engine/sim/src/unit_tick.cpp
    engine/sim/src/victory.cpp
    engine/sim/src/weapons.cpp
    engine/sim/src/world.cpp
    engine/sim/src/x87_math.cpp
    engine/sim/src/x87_trig.cpp
)
target_include_directories(sim PUBLIC engine/sim/include)
target_compile_options(sim PRIVATE ${FP_FLAGS} ${WARN_FLAGS})

# Network play: wire format, frames, transports, the battle's network side and the battle room.
add_library(net STATIC
    engine/net/src/codec.cpp
    engine/net/src/dplay_serial.cpp
    engine/net/src/dplay_session.cpp
    engine/net/src/dplay_tcpip.cpp
    engine/net/src/dplay_wire.cpp
    engine/net/src/frames.cpp
    engine/net/src/lobby.cpp
    engine/net/src/net_battle.cpp
    engine/net/src/protocol.cpp
    engine/net/src/transport.cpp
    engine/net/src/udp_transport.cpp
)
target_include_directories(net PUBLIC engine/net/include)
target_link_libraries(net PUBLIC sim)
if(WIN32)
    target_link_libraries(net PUBLIC ws2_32)
endif()
target_compile_options(net PRIVATE ${FP_FLAGS} ${WARN_FLAGS})

# TA compatibility layer (importers for the original game's data). Kept apart
# from the engine; the engine never depends on it.
add_library(ta_compat STATIC
    ta_compat/src/ai_profile.cpp
    ta_compat/src/battle_economy.cpp
    ta_compat/src/battle_features.cpp
    ta_compat/src/battle_loader.cpp
    ta_compat/src/battle_types.cpp
    ta_compat/src/campaign.cpp
    ta_compat/src/mission_text.cpp
    ta_compat/src/notice_texts.cpp
    ta_compat/src/online_services.cpp
    ta_compat/src/preferences.cpp
    ta_compat/src/cob.cpp
    ta_compat/src/crt_strtod.cpp
    ta_compat/src/featuredefs.cpp
    ta_compat/src/net_setup.cpp
    ta_compat/src/restrict_list.cpp
    ta_compat/src/gaf.cpp
    ta_compat/src/hpi.cpp
    ta_compat/src/inflate.cpp
    ta_compat/src/model3do.cpp
    ta_compat/src/saved_game.cpp
    ta_compat/src/save_sections.cpp
    ta_compat/src/sight_tables.cpp
    ta_compat/src/tdf.cpp
    ta_compat/src/tnt.cpp
    ta_compat/src/translate.cpp
    ta_compat/src/unitdefs.cpp
    ta_compat/src/weapondefs.cpp
)
target_include_directories(ta_compat PUBLIC ta_compat/include)
target_link_libraries(ta_compat PUBLIC sim net)
target_compile_options(ta_compat PRIVATE ${FP_FLAGS} ${WARN_FLAGS})

foreach(tool hpi_check tdf_check vfs_cat unitdef_check weapondef_check model_check feature_check game_flow game_replay campaign_check save_check save_make move_bench)
    add_executable(${tool} ta_compat/tools/${tool}.cpp)
    target_link_libraries(${tool} PRIVATE ta_compat)
    target_compile_options(${tool} PRIVATE ${WARN_FLAGS})
endforeach()

if(MSVC)
    target_link_options(move_bench PRIVATE /MAP)  # function addresses for sampling profiles
endif()

add_executable(net_play ta_compat/tools/net_play.cpp)
target_link_libraries(net_play PRIVATE ta_compat net)
target_compile_options(net_play PRIVATE ${WARN_FLAGS})

# Network games over the original's session protocol (with the original program or ourselves).
add_executable(dplay_play ta_compat/tools/dplay_play.cpp)
target_link_libraries(dplay_play PRIVATE ta_compat net)
target_compile_options(dplay_play PRIVATE ${WARN_FLAGS})

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

add_executable(net_tests tests/net_tests.cpp)
target_link_libraries(net_tests PRIVATE net)
target_compile_options(net_tests PRIVATE ${FP_FLAGS} ${WARN_FLAGS})
add_test(NAME net_tests COMMAND net_tests)

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
    # A scripted two-machine network game: trees, rocks, metal and wrecks shot at, fires,
    # reclaims and kills; both machines' feature maps must agree at the end.
    add_test(NAME net_features_match COMMAND net_play "${TA_GAME_DIR}" features 900 1)
endif()

# Renderer (8-bit battle view), its game-data importers and the offline viewer.
add_subdirectory(render)
# Playable client (SDL3 window and sound when the GPU backend is built).
add_subdirectory(client)
