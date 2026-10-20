#include "client/canvas.h"

#include <algorithm>
#include <string>

namespace client {

namespace {
std::uint8_t g_last_text_colour = 0;
}
std::uint8_t last_text_colour() { return g_last_text_colour; }
void set_last_text_colour(std::uint8_t colour) { g_last_text_colour = colour; }

void fill_rect(render::Image8& dst, const render::ClipRect& clip, int x0, int y0, int x1, int y1, std::uint8_t colour) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    x0 = std::max({x0, clip.x0, 0});
    y0 = std::max({y0, clip.y0, 0});
    x1 = std::min({x1, clip.x1, dst.width - 1});
    y1 = std::min({y1, clip.y1, dst.height - 1});
    for (int y = y0; y <= y1; ++y) {
        std::uint8_t* row = dst.row(y);
        for (int x = x0; x <= x1; ++x) row[x] = colour;
    }
}

void outline_rect(render::Image8& dst, const render::ClipRect& clip, int x0, int y0, int x1, int y1, std::uint8_t colour) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    fill_rect(dst, clip, x0, y0, x1, y0, colour);
    fill_rect(dst, clip, x0, y1, x1, y1, colour);
    fill_rect(dst, clip, x0, y0, x0, y1, colour);
    fill_rect(dst, clip, x1, y0, x1, y1, colour);
}

void put_pixel(render::Image8& dst, const render::ClipRect& clip, int x, int y, std::uint8_t colour) {
    if (x < std::max(clip.x0, 0) || y < std::max(clip.y0, 0) || x > std::min(clip.x1, dst.width - 1) ||
        y > std::min(clip.y1, dst.height - 1))
        return;
    dst.row(y)[x] = colour;
}

void circle(render::Image8& dst, const render::ClipRect& clip, int cx, int cy, int r, std::uint8_t colour) {
    if (r <= 0) {
        put_pixel(dst, clip, cx, cy, colour);
        return;
    }
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        const int pts[8][2] = {{x, y}, {y, x}, {-y, x}, {-x, y}, {-x, -y}, {-y, -x}, {y, -x}, {x, -y}};
        for (const auto& p : pts) put_pixel(dst, clip, cx + p[0], cy + p[1], colour);
        ++y;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            --x;
            err += 2 * (y - x) + 1;
        }
    }
}

int Font::text_width(std::string_view text) const {
    int w = 0;
    for (unsigned char c : text) {
        if (c == 0 || c == '\n') break;
        if (glyphs[c].present) w += glyphs[c].width;
    }
    return w;
}

void draw_text(render::Image8& dst, const render::ClipRect& clip, const Font& font, std::string_view text, int x, int y, int fg,
               int bg, int max_width) {
    std::string s(text.substr(0, std::min(text.find('\n'), text.size())));
    if (max_width >= 0)
        while (!s.empty() && font.text_width(s) > max_width) s.pop_back();
    const int top = y - font.raise;
    for (unsigned char c : s) {
        const Glyph& g = font.glyphs[c];
        if (!g.present) continue;
        const int rows = g.rows > 0 ? g.rows : font.height;
        for (int r = 0; r < rows; ++r)
            for (int k = 0; k < g.width; ++k) {
                const std::size_t at = static_cast<std::size_t>(r * g.width + k);
                const bool ink = g.ink[at] != 0;
                const int v = ink ? (g.colours.empty() ? fg : g.colours[at]) : bg;
                if (v >= 0) put_pixel(dst, clip, x + g.left + k, top + g.top + r, static_cast<std::uint8_t>(v));
            }
        x += g.width;
    }
}

} // namespace client
