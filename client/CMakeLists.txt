# Playable client: input, selection, commands, interface and sound over the
# battle view (engine side, no game data names), its game-data importer and the
# executable that plays a skirmish from an installed copy of the original game.
add_library(client STATIC
    src/canvas.cpp
    src/commands.cpp
    src/front.cpp
    src/game_client.cpp
    src/hud.cpp
)
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
    ${CMAKE_SOURCE_DIR}/ta_compat/src/client_content.cpp
    ${CMAKE_SOURCE_DIR}/ta_compat/src/client_scene.cpp
)
target_link_libraries(ta_compat_client PUBLIC client ta_compat_render)
target_compile_options(ta_compat_client PRIVATE ${WARN_FLAGS})

add_executable(ta_play ${CMAKE_SOURCE_DIR}/ta_compat/tools/ta_play.cpp)
target_link_libraries(ta_play PRIVATE ta_compat_client)
if(TARGET render_gpu)
    target_link_libraries(ta_play PRIVATE SDL3::SDL3-static)
endif()
target_compile_options(ta_play PRIVATE ${WARN_FLAGS})
if(MSVC)
    target_link_options(ta_play PRIVATE /MAP)  # function addresses for crash reports
endif()
