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

namespace {

// 32-bit multiply keeping the low 32 bits (the original's IMUL).
int mul32(int a, int b) { return static_cast<int>(static_cast<unsigned>(a) * static_cast<unsigned>(b)); }

// The original's segment clip (0x4BEA20) against an inclusive rectangle: each end in turn is
// moved onto the edge it lies beyond (left, top, right, bottom; first the start, then the end),
// the other coordinate moved by (distance * other delta) / delta in 32-bit C division with the
// deltas taken before clipping. A segment wholly outside, or needing a move along a zero delta,
// is dropped.
bool clip_segment(const ClipRect& c, int& x0, int& y0, int& x1, int& y1) {
    const bool right = x0 <= x1, down = y0 <= y1;
    const int dy = y1 - y0, dx = x1 - x0;
    if (x0 < c.x0) {
        if (!right || dx == 0) return false;
        y0 += mul32(c.x0 - x0, dy) / dx;
        x0 = c.x0;
    }
    if (y0 < c.y0) {
        if (!down || dy == 0) return false;
        x0 += mul32(c.y0 - y0, dx) / dy;
        y0 = c.y0;
    }
    if (c.x1 < x0) {
        if (right || dx == 0) return false;
        y0 += mul32(c.x1 - x0, dy) / dx;
        x0 = c.x1;
    }
    if (c.y1 < y0) {
        if (down || dy == 0) return false;
        x0 += mul32(c.y1 - y0, dx) / dy;
        y0 = c.y1;
    }
    if (x1 < c.x0) {
        if (right || dx == 0) return false;
        y1 += mul32(c.x0 - x1, dy) / dx;
        x1 = c.x0;
    }
    if (y1 < c.y0) {
        if (down || dy == 0) return false;
        x1 += mul32(c.y0 - y1, dx) / dy;
        y1 = c.y0;
    }
    if (c.x1 < x1) {
        if (!right || dx == 0) return false;
        y1 += mul32(c.x1 - x1, dy) / dx;
        x1 = c.x1;
    }
    if (y1 <= c.y1) return true;
    if (!down || dy == 0) return false;
    x1 += mul32(c.y1 - y1, dx) / dy;
    y1 = c.y1;
    return true;
}

} // namespace

void draw_line(Image8& dst, const ClipRect& clip, int x0, int y0, int x1, int y1, std::uint8_t colour) {
    // The original's line (0x4BE950): the segment is clipped (clip_segment), then stepped
    // from its left end (ends swapped when needed) along the longer axis: error 2*minor - major,
    // a step on the minor axis whenever the error is not negative (then error += 2*minor -
    // 2*major, else += 2*minor); major + 1 pixels. Vertical lines run from the upper end.
    if (!clip_segment(clip, x0, y0, x1, y1)) return;
    // (The raster routine's own clip to the surface keeps segments already inside it.)
    if (x0 < 0 || y0 < 0 || x1 < 0 || y1 < 0 || x0 >= dst.width || x1 >= dst.width || y0 >= dst.height || y1 >= dst.height) return;
    if (x1 == x0) {
        const int ya = std::min(y0, y1), n = std::abs(y1 - y0) + 1;
        for (int k = 0; k < n; ++k) dst.row(ya + k)[x0] = colour;
        return;
    }
    if (x1 < x0) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }
    const int dx = x1 - x0;
    int dy = y1 - y0;
    if (dy == 0) {
        for (int k = 0; k <= dx; ++k) dst.row(y0)[x0 + k] = colour;
        return;
    }
    const int sy = dy < 0 ? -1 : 1;
    dy = std::abs(dy);
    int x = x0, y = y0;
    if (dy <= dx) {
        int err = 2 * dy - dx;
        for (int k = 0; k <= dx; ++k) {
            dst.row(y)[x] = colour;
            ++x;
            if (err < 0) err += 2 * dy;
            else {
                err += 2 * dy - 2 * dx;
                y += sy;
            }
        }
    } else {
        int err = 2 * dx - dy;
        for (int k = 0; k <= dy; ++k) {
            dst.row(y)[x] = colour;
            y += sy;
            if (err < 0) err += 2 * dx;
            else {
                err += 2 * dx - 2 * dy;
                ++x;
            }
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
