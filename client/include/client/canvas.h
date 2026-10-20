#pragma once
// Drawing onto an 8-bit image for the interface layer: filled and outlined
// rectangles, pixels, lines, sprites and one-bit fonts. Rectangles are
// inclusive on all sides; everything is clipped to the given rectangle.

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "render/image.h"

namespace client {

void fill_rect(render::Image8& dst, const render::ClipRect& clip, int x0, int y0, int x1, int y1, std::uint8_t colour);
// The text colour the bars' text routine used last: the original's text routine (0x4C14F0) draws
// in the colour last set, so text drawn without setting one (range labels, the profile panel)
// takes it.
std::uint8_t last_text_colour();
void set_last_text_colour(std::uint8_t colour);
void outline_rect(render::Image8& dst, const render::ClipRect& clip, int x0, int y0, int x1, int y1, std::uint8_t colour);
void put_pixel(render::Image8& dst, const render::ClipRect& clip, int x, int y, std::uint8_t colour);
// A circle outline of radius r around (cx, cy) (midpoint algorithm).
void circle(render::Image8& dst, const render::ClipRect& clip, int cx, int cy, int r, std::uint8_t colour);

// A one-bit font: glyphs of one height and varying width.
struct Glyph {
    int width = 0;
    bool present = false;
    std::vector<std::uint8_t> ink;  // width * height, row-major, 1 = foreground
    // Coloured glyphs (fonts drawn from picture art): the pixel values where ink is 1; the
    // foreground colour is not used for them.
    std::vector<std::uint8_t> colours;
    // Glyphs taller or shorter than the cell (picture fonts): ink has width * rows entries,
    // drawn from row `top` of the cell (may lie outside it). rows 0: the font's height, top 0.
    int top = 0, rows = 0;
    int left = 0;  // ink drawn from this column of the cell (picture fonts: minus the frame's x offset)
};

struct Font {
    int height = 0;  // rows per glyph
    int raise = 0;   // a glyph drawn at y starts at row y - raise
    std::array<Glyph, 256> glyphs{};

    // Sum of the glyph widths up to the end or the first newline.
    int text_width(std::string_view text) const;
};

// Draws text with its top-left at (x, y - raise): ink pixels in `fg`, the rest of
// each glyph cell in `bg` unless bg < 0 (transparent). With max_width >= 0, trailing
// characters are dropped until the text fits.
void draw_text(render::Image8& dst, const render::ClipRect& clip, const Font& font, std::string_view text, int x, int y,
               int fg, int bg = -1, int max_width = -1);

} // namespace client
