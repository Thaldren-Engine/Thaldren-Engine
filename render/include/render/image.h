#pragma once
// 8-bit palettised images: the frame, sprites and the blits between them
// (spec: docs/RE_MEMORY.md, "Rendering").

#include <array>
#include <cstdint>
#include <vector>

namespace render {

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
};

using Palette = std::array<Rgb, 256>;

// 256 x 256 colour-mixing table: entry [src * 256 + dst] is the index drawn
// when a blended pixel `src` lands on `dst`.
using BlendTable = std::vector<std::uint8_t>;

// Clip rectangle with inclusive bounds.
struct ClipRect {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
};

// A palette-index image (the frame, or a scratch surface).
struct Image8 {
    int width = 0, height = 0;
    std::vector<std::uint8_t> pixels;

    Image8() = default;
    Image8(int w, int h, std::uint8_t fill = 0)
        : width(w), height(h), pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), fill) {}
    std::uint8_t* row(int y) { return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width); }
    const std::uint8_t* row(int y) const { return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width); }
    ClipRect bounds() const { return {0, 0, width - 1, height - 1}; }
};

// A sprite: pixels plus a hotspot. Drawn at (x, y), its top-left corner lands
// at (x - x_offset, y - y_offset). Pixels equal to `key` are transparent.
struct Sprite8 {
    int width = 0, height = 0;
    int x_offset = 0, y_offset = 0;
    std::uint8_t key = 0;
    bool has_key = true;
    std::vector<std::uint8_t> pixels;

    std::uint8_t at(int x, int y) const { return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]; }
};

// Copies the sprite (skipping key pixels when it has a key) clipped to `clip`.
void blit(Image8& dst, const ClipRect& clip, const Sprite8& s, int x, int y);
// Blends every non-key pixel through `table` (dst = table[src * 256 + dst]).
void blit_blend(Image8& dst, const ClipRect& clip, const Sprite8& s, int x, int y, const BlendTable& table);
// Clipped line in one colour (the line routine the original uses for beams).
void draw_line(Image8& dst, const ClipRect& clip, int x0, int y0, int x1, int y1, std::uint8_t colour);

// An index that no pixel of `pixels` uses (for transient sprites whose every
// value may be meaningful), or -1 when all 256 occur.
int unused_index(const std::vector<std::uint8_t>& pixels);

} // namespace render
