# Playable client: input, selection, commands, interface and sound over the
# battle view (engine side, no game data names), its game-data importer and the
# executable that plays a skirmish from an installed copy of the original game.
add_library(client STATIC
    src/audio_decode.cpp
    src/canvas.cpp
    src/commands.cpp
    src/end_table.cpp
    src/front.cpp
    src/front_end.cpp
    src/game_client.cpp
    src/hud.cpp
    src/menu.cpp
    src/minimap.cpp
    src/movie.cpp
    src/music.cpp
    src/panels.cpp
    src/queued_orders.cpp
    src/screenshot.cpp
    src/settings.cpp
    src/smacker.cpp
    src/sound_desk.cpp
    src/sound_mixer.cpp
    src/sound_rules.cpp
    src/wave_file.cpp
)
if(WIN32)
    target_link_libraries(client PRIVATE mfplat mfreadwrite mfuuid ole32)
endif()
target_include_directories(client PUBLIC include)
target_link_libraries(client PUBLIC render sim)
# Multiplayer menus and session (with the front end's entry-point interface).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/include/client/front_end.h)
    target_sources(client PRIVATE src/net_menu.cpp)
    target_link_libraries(client PUBLIC net)
endif()
target_compile_options(client PRIVATE ${WARN_FLAGS})
if(MSVC)
    target_compile_definitions(client PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(TARGET render_gpu)
    target_sources(client PRIVATE src/audio_sdl.cpp)
    target_link_libraries(client PUBLIC render_gpu PRIVATE SDL3::SDL3-static)
    target_compile_definitions(client PRIVATE CLIENT_HAVE_SDL=1)
else()
    target_sources(client PRIVATE src/audio_none.cpp)
endif()

# Importer: skirmish set-up (adapter), interface content, battle view scene.
add_library(ta_compat_client STATIC
    ${CMAKE_SOURCE_DIR}/ta_compat/src/client_battle_setup.cpp
    ${CMAKE_SOURCE_DIR}/ta_compat/src/client_campaign.cpp
    ${CMAKE_SOURCE_DIR}/ta_compat/src/client_content.cpp
    ${CMAKE_SOURCE_DIR}/ta_compat/src/client_loading.cpp
    ${CMAKE_SOURCE_DIR}/ta_compat/src/client_menus.cpp
    ${CMAKE_SOURCE_DIR}/ta_compat/src/client_net_screens.cpp
    ${CMAKE_SOURCE_DIR}/ta_compat/src/client_screens.cpp
    ${CMAKE_SOURCE_DIR}/ta_compat/src/client_scene.cpp
    ${CMAKE_SOURCE_DIR}/ta_compat/src/console_commands.cpp
    ${CMAKE_SOURCE_DIR}/ta_compat/src/console_table.cpp
)
target_link_libraries(ta_compat_client PUBLIC client ta_compat_render)
target_compile_options(ta_compat_client PRIVATE ${WARN_FLAGS})

add_executable(ta_play ${CMAKE_SOURCE_DIR}/ta_compat/tools/ta_play.cpp)
target_link_libraries(ta_play PRIVATE ta_compat_client)
if(TARGET render_gpu)
    target_link_libraries(ta_play PRIVATE SDL3::SDL3-static)
    target_compile_definitions(ta_play PRIVATE CLIENT_HAVE_SDL=1)  # (the pointer self-test's SDL events)
endif()
target_compile_options(ta_play PRIVATE ${WARN_FLAGS})
if(MSVC)
    target_link_options(ta_play PRIVATE /MAP)  # function addresses for crash reports
endif()
# Every in-battle menu path leaves the battle running (skirmish and network game; needs the game).
if(WIN32 AND TA_GAME_DIR)
    add_test(NAME battle_menus_keep_battle
             COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tools/battle_menus_check.ps1"
                     -Exe "$<TARGET_FILE:ta_play>" -Game "${TA_GAME_DIR}")
endif()

# Whole-frame comparison with the original's frame draw (32-bit builds only: it calls the
# original executable's routines in a suspended process, ta_compat/tools/orig_harness).
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(frame_harness ${CMAKE_SOURCE_DIR}/ta_compat/tools/orig_harness/frame_harness.cpp)
    target_include_directories(frame_harness PRIVATE ${CMAKE_SOURCE_DIR}/ta_compat/tools/orig_harness)
    target_link_libraries(frame_harness PRIVATE ta_compat_client)
    target_compile_options(frame_harness PRIVATE /W4 /permissive-)
endif()

# The compatibility tests also check client rules fed by the importer (the unit notices' texts).
target_link_libraries(compat_tests PRIVATE client)
