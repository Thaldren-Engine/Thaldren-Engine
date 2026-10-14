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
    // Name of the GPU driver in use ("direct3d12", "vulkan", "metal").
    std::string driver() const;

    struct Impl;

private:
    explicit GpuRenderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace render
