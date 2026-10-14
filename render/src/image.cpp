#include "render/image.h"

#include <algorithm>
#include <cstdlib>

namespace render {
namespace {

// Intersects the sprite rectangle placed at (x, y) with the clip and the image.
struct Span {
    int sx = 0, sy = 0, dx = 0, dy = 0, w = 0, h = 0;
};

bool place(const Image8& dst, const ClipRect& clip, const Sprite8& s, int x, int y, Span& out) {
    const int left = x - s.x_offset, top = y - s.y_offset;
    const int cx0 = std::max(clip.x0, 0), cy0 = std::max(clip.y0, 0);
    const int cx1 = std::min(clip.x1, dst.width - 1), cy1 = std::min(clip.y1, dst.height - 1);
    const int x0 = std::max(left, cx0), y0 = std::max(top, cy0);
    const int x1 = std::min(left + s.width - 1, cx1), y1 = std::min(top + s.height - 1, cy1);
    if (x0 > x1 || y0 > y1) return false;
    out = {x0 - left, y0 - top, x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    return true;
}

} // namespace

void blit(Image8& dst, const ClipRect& clip, const Sprite8& s, int x, int y) {
    Span p;
    if (!place(dst, clip, s, x, y, p)) return;
    for (int j = 0; j < p.h; ++j) {
        const std::uint8_t* src = s.pixels.data() + static_cast<std::size_t>(p.sy + j) * static_cast<std::size_t>(s.width) + static_cast<std::size_t>(p.sx);
        std::uint8_t* d = dst.row(p.dy + j) + p.dx;
        if (!s.has_key) {
            std::copy(src, src + p.w, d);
            continue;
        }
        for (int i = 0; i < p.w; ++i)
            if (src[i] != s.key) d[i] = src[i];
    }
}

void blit_blend(Image8& dst, const ClipRect& clip, const Sprite8& s, int x, int y, const BlendTable& table) {
    if (table.size() < 65536) return;
    Span p;
    if (!place(dst, clip, s, x, y, p)) return;
    for (int j = 0; j < p.h; ++j) {
        const std::uint8_t* src = s.pixels.data() + static_cast<std::size_t>(p.sy + j) * static_cast<std::size_t>(s.width) + static_cast<std::size_t>(p.sx);
        std::uint8_t* d = dst.row(p.dy + j) + p.dx;
        for (int i = 0; i < p.w; ++i)
            if (!s.has_key || src[i] != s.key) d[i] = table[static_cast<std::size_t>(src[i]) * 256u + d[i]];
    }
}

void draw_line(Image8& dst, const ClipRect& clip, int x0, int y0, int x1, int y1, std::uint8_t colour) {
    // Integer DDA (Bresenham), clipped per pixel. The original clips the
    // segment first and then steps it; the pixel choice can differ by one at
    // ties (not yet verified).
    const int cx0 = std::max(clip.x0, 0), cy0 = std::max(clip.y0, 0);
    const int cx1 = std::min(clip.x1, dst.width - 1), cy1 = std::min(clip.y1, dst.height - 1);
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 1 << 16; ++guard) {
        if (x0 >= cx0 && x0 <= cx1 && y0 >= cy0 && y0 <= cy1) dst.row(y0)[x0] = colour;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

int unused_index(const std::vector<std::uint8_t>& pixels) {
    std::array<bool, 256> used{};
    for (std::uint8_t p : pixels) used[p] = true;
    for (int i = 255; i >= 0; --i)
        if (!used[static_cast<std::size_t>(i)]) return i;
    return -1;
}

} // namespace render
