# Renderer: 8-bit palettised battle view (portable C++20, no platform code).
add_library(render STATIC
    src/battle_view.cpp
    src/bmp.cpp
    src/draw_list.cpp
    src/image.cpp
    src/model.cpp
    src/raster.cpp
)
target_include_directories(render PUBLIC include)
target_link_libraries(render PUBLIC sim)
target_compile_options(render PRIVATE ${WARN_FLAGS})
if(MSVC)
    target_compile_definitions(render PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Importers from the original game's graphics into the renderer's formats.
add_library(ta_compat_render STATIC ${CMAKE_SOURCE_DIR}/ta_compat/src/render_assets.cpp)
target_link_libraries(ta_compat_render PUBLIC ta_compat render)
target_compile_options(ta_compat_render PRIVATE ${WARN_FLAGS})

# Optional GPU backend on SDL3's GPU API (D3D12 / Vulkan / Metal). Point
# SDL3_SOURCE_DIR at an SDL 3.x source tree; it is built statically.
set(SDL3_SOURCE_DIR "" CACHE PATH "SDL 3.x source tree for the GPU backend (optional)")
if(SDL3_SOURCE_DIR)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${SDL3_SOURCE_DIR} ${CMAKE_BINARY_DIR}/sdl3 EXCLUDE_FROM_ALL)
    add_library(render_gpu STATIC src/gpu_renderer.cpp)
    target_include_directories(render_gpu PUBLIC include PRIVATE shaders)
    target_link_libraries(render_gpu PUBLIC render PRIVATE SDL3::SDL3-static)
    target_compile_options(render_gpu PRIVATE ${WARN_FLAGS})
    target_compile_definitions(render_gpu PUBLIC RENDER_HAVE_GPU=1)
endif()

add_executable(battle_view tools/battle_view.cpp)
target_link_libraries(battle_view PRIVATE ta_compat_render)
if(TARGET render_gpu)
    target_link_libraries(battle_view PRIVATE render_gpu SDL3::SDL3-static)
endif()
target_compile_options(battle_view PRIVATE ${WARN_FLAGS})

add_executable(render_tests tests/render_tests.cpp)
target_link_libraries(render_tests PRIVATE render)
target_compile_options(render_tests PRIVATE ${WARN_FLAGS})
add_test(NAME render_tests COMMAND render_tests)
