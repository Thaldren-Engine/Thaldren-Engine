#pragma once
// A frame as an ordered list of 2D operations on an 8-bit target. The battle
// view builder produces it (projection, culling, draw order, model sprites);
// a backend executes it: the CPU reference backend or the GPU backend. Both
// must produce the same palette indices.

#include <cstdint>
#include <memory>
#include <vector>

#include "render/image.h"

namespace render {

enum class DrawOp : std::uint8_t {
    Blit,       // copy the sprite, key pixels transparent
    BlitBlend,  // mix the sprite's non-key pixels with the frame through the blend table
    Line,       // one-colour line from (x, y) to (x1, y1)
};

struct DrawCmd {
    DrawOp op = DrawOp::Blit;
    const Sprite8* sprite = nullptr;  // Blit / BlitBlend
    bool persistent = false;          // sprite outlives the frame (backends may cache it)
    int x = 0, y = 0, x1 = 0, y1 = 0;
    std::uint8_t colour = 0;          // Line
    ClipRect clip;
};

struct DrawList {
    int width = 0, height = 0;
    std::uint8_t clear_index = 0;
    std::vector<DrawCmd> cmds;
    // Sprites made for this frame (model sprites, shadows, directly drawn pieces).
    std::vector<std::unique_ptr<Sprite8>> transient;

    const Sprite8* keep(Sprite8 s) {
        transient.push_back(std::make_unique<Sprite8>(std::move(s)));
        return transient.back().get();
    }
    void blit(const Sprite8* s, int x, int y, const ClipRect& clip, bool persistent) {
        cmds.push_back({DrawOp::Blit, s, persistent, x, y, 0, 0, 0, clip});
    }
    void blend(const Sprite8* s, int x, int y, const ClipRect& clip, bool persistent) {
        cmds.push_back({DrawOp::BlitBlend, s, persistent, x, y, 0, 0, 0, clip});
    }
    void line(int x0, int y0, int x1, int y1, std::uint8_t colour, const ClipRect& clip) {
        cmds.push_back({DrawOp::Line, nullptr, false, x0, y0, x1, y1, colour, clip});
    }
};

// CPU reference backend: executes the list into an 8-bit frame.
Image8 execute(const DrawList& list, const BlendTable& blend);

// Turns every Line command into a transient sprite holding exactly the
// reference line's pixels, so backends without a line primitive stay exact.
void lines_to_sprites(DrawList& list);

} // namespace render
