#pragma once
// GPU backend (SDL3 GPU API: Direct3D 12, Vulkan or Metal). Executes a
// DrawList on the GPU into an 8-bit index target (R8, index / 255) with the
// same results as the CPU reference, then optionally resolves it through the
// palette in a shader. Model rasterisation stays on the CPU (it reproduces
// the original's integer scan conversion, which GPU rasterisation cannot);
// the GPU composites terrain, sprites, model sprites, shadows and blends.

#include <memory>
#include <string>
#include <vector>

#include "render/draw_list.h"
#include "render/presenter.h"

namespace render {

class GpuRenderer : public FrameRenderer {
public:
    // nullptr when no GPU device can be created (see last_error()).
    static std::unique_ptr<GpuRenderer> create(const BlendTable& blend);
    static const std::string& last_error();
    ~GpuRenderer() override;

    // Draws the list (Line commands must have been turned into sprites with
    // lines_to_sprites) and reads the index frame back.
    bool render(const DrawList& list, Image8& out) override;
    // Draws the list and resolves it through `palette` on the GPU, reading
    // back RGBA8 pixels (width * height * 4).
    bool render_rgba(const DrawList& list, const Palette& palette, std::vector<std::uint8_t>& rgba);
    // Window output: opens a window (frame size x scale) on first use and
    // shows each drawn list there, resolved through the palette on the GPU.
    // Returns false when the window was closed or on error.
    bool present_window(const DrawList& list, const Palette& palette, int scale, const char* title);
    // The same into a window the caller made and pumps events for (an SDL_Window*):
    // the frame is scaled by `scale` and drawn at the top-left of its drawable area.
    bool present_to_window(void* sdl_window, const DrawList& list, const Palette& palette, int scale);

    // Name of the GPU driver in use ("direct3d12", "vulkan", "metal").
    std::string driver() const;

    struct Impl;

private:
    bool present_frame(const DrawList& list, const Palette& palette, int scale);
    explicit GpuRenderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace render
