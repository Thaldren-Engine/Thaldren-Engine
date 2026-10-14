#include "render/raster.h"

#include <algorithm>
#include <vector>

namespace render {
namespace {

// 32-bit wrapping helpers (the original computes in 32-bit registers).
inline std::int32_t wmul(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
inline std::int32_t wadd(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
inline std::int32_t wsub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
inline std::int32_t cdiv(std::int32_t a, std::int32_t b) {
    if (b == 0) return 0;
    if (a == INT32_MIN && b == -1) return a;
    return a / b;  // C truncation
}

// Per-row span ends: x (integer), then interpolated attributes in 16.16.
struct RowEnd {
    std::int32_t x = 0, u = 0, v = 0, z = 0;
};
struct Row {
    RowEnd l, r;
};

struct EdgeVertex {
    std::int32_t x, y, z, u, v;
};

// Walks one edge from a (top) to b (bottom) and appends its rows to `out`,
// starting at `cursor` (the original stores rows through a moving pointer).
void walk_edge(const EdgeVertex& a, const EdgeVertex& b, int clip_y0, int max_y, std::vector<Row>& rows,
               std::size_t& cursor, bool right) {
    if (!(a.y < b.y)) return;
    const std::int32_t dy = b.y - a.y;
    const std::int32_t sx = cdiv(wmul(wsub(b.x, a.x), 0x10000), dy);
    const std::int32_t su = cdiv(wsub(wmul(b.u, 0x10000), wmul(a.u, 0x10000)), dy);
    const std::int32_t sv = cdiv(wsub(wmul(b.v, 0x10000), wmul(a.v, 0x10000)), dy);
    const std::int32_t sz = cdiv(wsub(wmul(b.z, 0x10000), wmul(a.z, 0x10000)), dy);
    std::int32_t x = wadd(wmul(a.x, 0x10000), 0xFFFF);
    std::int32_t u = wmul(a.u, 0x10000), v = wmul(a.v, 0x10000), z = wmul(a.z, 0x10000);
    std::int32_t y = a.y;
    if (y < clip_y0) {
        const std::int32_t skip = clip_y0 - y;
        x = wadd(x, wmul(sx, skip));
        u = wadd(u, wmul(su, skip));
        v = wadd(v, wmul(sv, skip));
        z = wadd(z, wmul(sz, skip));
        y = clip_y0;
    }
    const std::int32_t end = std::min(b.y, max_y);
    for (; y < end; ++y) {
        if (cursor >= rows.size()) rows.resize(cursor + 1);  // the original would overrun its buffer
        RowEnd& e = right ? rows[cursor].r : rows[cursor].l;
        e.x = x >> 16;
        e.u = u;
        e.v = v;
        e.z = z;
        ++cursor;
        x = wadd(x, sx);
        u = wadd(u, su);
        v = wadd(v, sv);
        z = wadd(z, sz);
    }
}

template <class Plot>
void scan(RasterTarget& t, const EdgeVertex* vs, int n, Plot&& plot) {
    if (n < 1) return;
    int top = 0, bottom = 0;
    std::int32_t min_y = 999999, max_y = -999999, min_x = 999999, max_x = -999999;
    for (int i = 0; i < n; ++i) {
        if (vs[i].y < min_y) {
            min_y = vs[i].y;
            top = i;
        }
        if (max_y < vs[i].y) {
            max_y = vs[i].y;
            bottom = i;
        }
        max_x = std::max(max_x, vs[i].x);
        min_x = std::min(min_x, vs[i].x);
    }
    const ClipRect& c = t.clip;
    if (max_x < c.x0 || min_x > c.x1 || max_y < c.y0 || min_y > c.y1) return;
    if (min_y < c.y0) min_y = c.y0;
    if (max_y > c.y1) max_y = c.y1;
    if (max_y == min_y) return;

    std::vector<Row> rows(static_cast<std::size_t>(max_y - min_y));
    std::size_t cursor = 0;
    int i = top;
    do {  // left chain: backwards from the top vertex
        const int j = i - 1 < 0 ? n - 1 : i - 1;
        walk_edge(vs[i], vs[j], c.y0, max_y, rows, cursor, false);
        i = j;
    } while (i != bottom);
    cursor = 0;
    i = top;
    do {  // right chain: forwards
        const int j = i + 1 >= n ? 0 : i + 1;
        walk_edge(vs[i], vs[j], c.y0, max_y, rows, cursor, true);
        i = j;
    } while (i != bottom);

    for (std::int32_t y = min_y; y < max_y; ++y) {
        const Row& r = rows[static_cast<std::size_t>(y - min_y)];
        std::int32_t x0 = r.l.x, x1 = r.r.x;
        const std::int32_t w = x1 - x0;
        if (w <= 0) continue;
        const std::int32_t su = cdiv(wsub(r.r.u, r.l.u), w);
        const std::int32_t sv = cdiv(wsub(r.r.v, r.l.v), w);
        const std::int32_t sz = cdiv(wsub(r.r.z, r.l.z), w);
        std::int32_t u = r.l.u, v = r.l.v, z = r.l.z;
        if (x0 < c.x0) {
            const std::int32_t skip = c.x0 - x0;
            u = wadd(u, wmul(su, skip));
            v = wadd(v, wmul(sv, skip));
            z = wadd(z, wmul(sz, skip));
            x0 = c.x0;
        }
        if (x1 > c.x1) x1 = c.x1;
        std::uint8_t* px = t.pixels + static_cast<std::ptrdiff_t>(y) * t.pitch;
        std::uint8_t* dz = t.depth ? t.depth + static_cast<std::ptrdiff_t>(y) * t.pitch : nullptr;
        for (std::int32_t x = x0; x < x1; ++x) {
            const auto zb = static_cast<std::uint8_t>(static_cast<std::uint32_t>(z) >> 16);
            if (!dz || dz[x] <= zb) {
                if (plot(px[x], u, v) && dz) dz[x] = zb;
            }
            u = wadd(u, su);
            v = wadd(v, sv);
            z = wadd(z, sz);
        }
    }
}

} // namespace

void fill_polygon(RasterTarget& t, const RasterVertex* v, int count, std::uint8_t colour) {
    std::vector<EdgeVertex> vs(static_cast<std::size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i) vs[static_cast<std::size_t>(i)] = {v[i].x, v[i].y, v[i].z, 0, 0};
    scan(t, vs.data(), count, [&](std::uint8_t& p, std::int32_t, std::int32_t) {
        p = colour;
        return true;
    });
}

void textured_quad(RasterTarget& t, const RasterVertex* v, const Sprite8& tex, bool* out_of_range) {
    if (tex.width <= 0 || tex.height <= 0) return;
    const std::int32_t uw = tex.width - 1, vh = tex.height - 1;
    const EdgeVertex vs[4] = {{v[0].x, v[0].y, v[0].z, 0, 0},
                              {v[1].x, v[1].y, v[1].z, uw, 0},
                              {v[2].x, v[2].y, v[2].z, uw, vh},
                              {v[3].x, v[3].y, v[3].z, 0, vh}};
    scan(t, vs, 4, [&](std::uint8_t& p, std::int32_t u, std::int32_t vv) {
        const std::int32_t tx = u >> 16, ty = vv >> 16;
        // The original addresses texels linearly (x + y * width), so an x
        // outside the row reads a neighbouring row. Outside the whole texture
        // it reads unrelated memory; we clamp there.
        const long long lin = static_cast<long long>(tx) + static_cast<long long>(ty) * tex.width;
        if (lin >= 0 && lin < static_cast<long long>(tex.pixels.size())) {
            p = tex.pixels[static_cast<std::size_t>(lin)];
        } else {
            if (out_of_range) *out_of_range = true;
            p = tex.at(std::clamp(tx, 0, tex.width - 1), std::clamp(ty, 0, tex.height - 1));
        }
        return true;
    });
}

} // namespace render
