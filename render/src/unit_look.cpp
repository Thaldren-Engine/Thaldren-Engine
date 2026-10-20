#include "render/unit_look.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/raster.h"
#include "sim/x87.h"

namespace render {
namespace {

inline int hi16(std::int32_t v) { return static_cast<std::int16_t>(static_cast<std::uint32_t>(v) >> 16); }
inline std::int32_t wsub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

bool visible(const sim::PosePiece& p) { return (p.flags & sim::pose_flags::kVisible) != 0; }
bool cached(const sim::PosePiece& p) { return (p.flags & sim::pose_flags::kCached) != 0; }

// Bounds of the visible pieces' projected points (x, -z - y/2), starting from the origin.
struct Box {
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};
Box piece_box(const sim::PoseState& pose) {
    Box b;
    for (std::size_t i = pose.pieces.size(); i-- > 0;) {
        const sim::PosePiece& pc = pose.pieces[i];
        if (!visible(pc)) continue;
        for (std::size_t v = 0; v + 2 < pc.points.size(); v += 3) {
            const int x = hi16(pc.points[v]);
            const int y = hi16(wsub(0, pc.points[v + 2])) - (hi16(pc.points[v + 1]) >> 1);
            b.x0 = std::min(b.x0, x);
            b.x1 = std::max(b.x1, x);
            b.y0 = std::min(b.y0, y);
            b.y1 = std::max(b.y1, y);
        }
    }
    return b;
}

const Sprite8* texture_frame(const ModelPrimitive& prim, const LookUnit& u, bool use_cursor) {
    const ModelTexture* tex = prim.texture;
    if (!tex || tex->frames.empty()) return nullptr;
    std::size_t f = 0;
    if (tex->animated) {
        if (tex->team_coloured) f = u.team_colour;
        else if (use_cursor) {
            if (prim.cursor >= 0 && u.cursor_frames && static_cast<std::size_t>(prim.cursor) < u.cursor_frames->size())
                f = (*u.cursor_frames)[static_cast<std::size_t>(prim.cursor)];
            else f = u.texture_frame % tex->frames.size();
        }
    }
    return f < tex->frames.size() ? &tex->frames[f] : nullptr;
}

// x87 single-precision vector steps of the lit path (each result stored as a float).
struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 int_delta(const std::int32_t* a, const std::int32_t* b) {  // b - a, 32-bit, as floats
    return {static_cast<float>(wsub(b[0], a[0])), static_cast<float>(wsub(b[1], a[1])), static_cast<float>(wsub(b[2], a[2]))};
}
V3 cross(const V3& a, const V3& b) {
    return {static_cast<float>(static_cast<double>(a.y) * b.z - static_cast<double>(a.z) * b.y),
            static_cast<float>(static_cast<double>(a.z) * b.x - static_cast<double>(a.x) * b.z),
            static_cast<float>(static_cast<double>(a.x) * b.y - static_cast<double>(a.y) * b.x)};
}
V3 normalize(const V3& v) {
    const double len = std::sqrt((static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y) + static_cast<double>(v.z) * v.z);
    return {static_cast<float>(v.x / len), static_cast<float>(v.y / len), static_cast<float>(v.z / len)};
}

// Light level of each vertex of a piece: the mean of the unit normals of the faces using it,
// dotted with the light direction, times 5, truncated, low 5 bits; unshaded pieces get 15.
std::vector<int> vertex_lights(const ModelPiece& piece, const sim::PosePiece& pc, const std::array<float, 3>& lv) {
    const std::size_t nv = pc.points.size() / 3;
    std::vector<int> light(nv, 15);
    if (!(pc.flags & sim::pose_flags::kShaded)) return light;
    std::vector<V3> sum(nv);
    std::vector<int> count(nv, 0);
    for (std::size_t k = static_cast<std::size_t>(piece.first_drawn); k < piece.primitives.size(); ++k) {
        const ModelPrimitive& prim = piece.primitives[k];
        const std::uint16_t a = prim.normal_indices[0], b = prim.normal_indices[1], c = prim.normal_indices[2];
        V3 n{0.0f, 1.0f, 0.0f};
        if (!(a == b || b == c || c == a)) {
            static const std::int32_t zero[3] = {0, 0, 0};
            auto pt = [&](std::uint16_t i) { return static_cast<std::size_t>(i) < nv ? &pc.points[static_cast<std::size_t>(i) * 3] : zero; };
            n = normalize(cross(int_delta(pt(b), pt(a)), int_delta(pt(b), pt(c))));
        }
        for (std::uint16_t i : prim.indices) {
            if (i >= nv) continue;
            V3& s = sum[i];
            s.x = static_cast<float>(static_cast<double>(s.x) + n.x);
            s.y = static_cast<float>(static_cast<double>(n.y) + s.y);
            s.z = static_cast<float>(static_cast<double>(s.z) + n.z);
            ++count[i];
        }
    }
    for (std::size_t i = 0; i < nv; ++i) {
        V3 n = sum[i];
        if (count[i]) {
            const double d = count[i];
            n = {static_cast<float>(n.x / d), static_cast<float>(n.y / d), static_cast<float>(n.z / d)};
        }
        const double dot = (static_cast<double>(n.x) * lv[0] + static_cast<double>(n.y) * lv[1]) + static_cast<double>(n.z) * lv[2];
        light[i] = sim::x87::ftol32(dot * 5.0) & 0x1F;
    }
    return light;
}

} // namespace

void rasterize_unit_pieces(LookFrame& f, const LookUnit& u, int which, bool lit, bool double_size, const LookTables& tables,
                           LookFrame* scratch) {
    if (!u.model) return;
    LookFrame big;
    LookFrame* t = &f;
    const int k = double_size ? 2 : 1;
    if (double_size) {
        big.image.width = f.image.width * 2;
        big.image.height = f.image.height * 2;
        big.image.x_offset = f.image.x_offset * 2;
        big.image.y_offset = f.image.y_offset * 2;
        big.image.key = 1;
        big.image.pixels.assign(static_cast<std::size_t>(big.image.width) * static_cast<std::size_t>(big.image.height), 1);
        big.depth.assign(big.image.pixels.size(), 0);
        t = &big;
    }
    RasterTarget rt;
    rt.pixels = t->image.pixels.data();
    rt.depth = t->depth.empty() ? nullptr : t->depth.data();
    rt.pitch = t->image.width;
    rt.clip = {0, 0, t->image.width - 1, t->image.height - 1};
    const std::uint8_t* shade = tables.shade && tables.shade->size() >= 32 * 256 ? tables.shade->data() : nullptr;
    if (lit && !shade) lit = false;
    const bool building = u.build_left != 0.0f;
    const int bias = u.depth_bias();
    std::vector<LitVertex> pts, vs;
    std::vector<RasterVertex> rv;
    for (std::size_t i = u.pose.pieces.size(); i-- > 0;) {
        const sim::PosePiece& pc = u.pose.pieces[i];
        if (!visible(pc)) continue;
        if (!(which == -1 || which == (cached(pc) ? 1 : 0) || building)) continue;
        if (pc.node < 0 || static_cast<std::size_t>(pc.node) >= u.model->pieces.size()) continue;
        const ModelPiece& piece = u.model->pieces[static_cast<std::size_t>(pc.node)];
        std::vector<int> lights;
        if (lit) lights = vertex_lights(piece, pc, tables.light);
        pts.clear();
        for (std::size_t v = 0; v + 2 < pc.points.size(); v += 3) {
            const int x = hi16(pc.points[v]) * k;
            const int y = hi16(pc.points[v + 1]) * k;
            const int z = hi16(wsub(0, pc.points[v + 2])) * k;
            LitVertex p;
            p.x = x + t->image.x_offset;
            p.y = z - (y >> 1) + t->image.y_offset;
            p.z = (double_size ? y / 2 : y) + bias;
            p.light = lit ? lights[v / 3] : 0;
            pts.push_back(p);
        }
        for (std::size_t q = static_cast<std::size_t>(piece.first_drawn); q < piece.primitives.size(); ++q) {
            const ModelPrimitive& prim = piece.primitives[q];
            vs.clear();
            for (std::uint16_t ix : prim.indices) vs.push_back(ix < pts.size() ? pts[ix] : LitVertex{});
            if (prim.flat) {
                if (vs.empty()) continue;
                if (lit) {
                    fill_polygon_lit(rt, vs.data(), static_cast<int>(vs.size()), prim.colour, shade);
                } else {
                    rv.clear();
                    for (const LitVertex& p : vs) rv.push_back({p.x, p.y, p.z});
                    fill_polygon(rt, rv.data(), static_cast<int>(rv.size()), prim.colour);
                }
            } else if (vs.size() == 4) {
                const Sprite8* tex = texture_frame(prim, u, which == 0);
                if (!tex) continue;
                if (lit) {
                    textured_quad_lit(rt, vs.data(), *tex, shade);
                } else {
                    rv.clear();
                    for (const LitVertex& p : vs) rv.push_back({p.x, p.y, p.z});
                    textured_quad(rt, rv.data(), *tex);
                }
            }
        }
    }
    if (!double_size) return;
    // Average each 2 x 2 block through the mixing table: rows first, then the two results.
    const BlendTable* mix = tables.mix;
    const int bw = big.image.width;
    for (int y = 0; y < f.image.height; ++y)
        for (int x = 0; x < f.image.width; ++x) {
            const std::uint8_t* r0 = &big.image.pixels[static_cast<std::size_t>(2 * y) * bw + 2 * x];
            const std::uint8_t* r1 = &big.image.pixels[static_cast<std::size_t>(2 * y + 1) * bw + 2 * x];
            std::uint8_t out = r0[0];
            if (mix && mix->size() >= 65536) {
                const std::uint8_t a = (*mix)[static_cast<std::size_t>(r0[0]) * 256 + r0[1]];
                const std::uint8_t b = (*mix)[static_cast<std::size_t>(r1[0]) * 256 + r1[1]];
                out = (*mix)[static_cast<std::size_t>(a) * 256 + b];
            }
            f.image.pixels[static_cast<std::size_t>(y) * f.image.width + x] = out;
        }
    if (!f.depth.empty())
        for (int y = 0; y < f.image.height; ++y)
            for (int x = 0; x < f.image.width; ++x)
                f.depth[static_cast<std::size_t>(y) * f.image.width + x] = big.depth[static_cast<std::size_t>(2 * y) * bw + 2 * x];
    if (scratch) *scratch = std::move(big);
}

LookFrame build_unit_sprite(const LookUnit& u, bool force_depth, int which, const ViewOptions& opt, const LookTables& tables,
                            LookFrame* scratch) {
    LookFrame f;
    const Box b = piece_box(u.pose);
    f.image.width = b.x1 - b.x0 + 4;
    f.image.height = b.y1 - b.y0 + 4;
    f.image.x_offset = 2 - b.x0;
    f.image.y_offset = 2 - b.y0;
    f.image.key = 1;
    f.image.pixels.assign(static_cast<std::size_t>(f.image.width) * static_cast<std::size_t>(f.image.height), 1);
    if (force_depth || u.zbuffer || u.build_left != 0.0f) f.depth.assign(f.image.pixels.size(), 0);
    const bool lit = u.structure && opt.shading;
    const bool twice = u.structure && opt.antialias && which != 0;
    rasterize_unit_pieces(f, u, which, lit, twice, tables, scratch);
    return f;
}

void cut_below(LookFrame& f, std::uint8_t level) {
    if (f.depth.size() != f.image.pixels.size()) return;
    for (std::size_t i = 0; i < f.depth.size(); ++i)
        if (f.depth[i] <= level) f.image.pixels[i] = f.image.key;
}

void tint_below(LookFrame& f, std::uint8_t level, const std::vector<std::uint8_t>& water) {
    if (f.depth.size() != f.image.pixels.size() || water.size() < 256) return;
    for (std::size_t i = 0; i < f.depth.size(); ++i)
        if (f.depth[i] <= level && f.image.pixels[i] != f.image.key) f.image.pixels[i] = water[f.image.pixels[i]];
}

LookFrame compose_depth_look(const DepthLookInput& in, const LookFrame& sprite) {
    const LookUnit& u = *in.unit;
    // The scratch covers the sprite, the unit's visible pieces and every unit on board.
    Box all = piece_box(u.pose);
    int l = all.x0 - 2, r = all.x1 + 2, t = all.y0 - 2, b = all.y1 + 2;
    struct Placed {
        const LookUnit* unit;
        int dx, dy, dz;
    };
    std::vector<Placed> placed;
    for (const LookUnit* c : in.cargo) {
        const Box cb = piece_box(c->pose);
        const std::int32_t dx = wsub(c->pos[0], u.pos[0]), dy = wsub(c->pos[1], u.pos[1]), dz = wsub(c->pos[2], u.pos[2]);
        const int ox = hi16(dx);
        const int oy = static_cast<std::int16_t>(hi16(dz) - static_cast<std::int16_t>(dy >> 17));
        l = std::min(l, cb.x0 - 2 + ox);
        r = std::max(r, cb.x1 + 2 + ox);
        t = std::min(t, cb.y0 - 2 + oy);
        b = std::max(b, cb.y1 + 2 + oy);
        placed.push_back({c, hi16(dx), hi16(dy), hi16(dz)});
    }
    const Sprite8& s = sprite.image;
    l = std::min(l, -s.x_offset);
    r = std::max(r, s.width - s.x_offset);
    t = std::min(t, -s.y_offset);
    b = std::max(b, s.height - s.y_offset);
    LookFrame out;
    Sprite8& o = out.image;
    o.width = static_cast<std::uint16_t>(r - l);
    o.height = static_cast<std::uint16_t>(b - t);
    o.x_offset = static_cast<std::int16_t>(-l);
    o.y_offset = static_cast<std::int16_t>(-t);
    o.key = s.key;
    std::size_t n = static_cast<std::size_t>(o.width) * static_cast<std::size_t>(o.height);
    if (o.width == s.width && o.height == s.height) {
        o.pixels = s.pixels;
        out.depth = sprite.depth;
        out.depth.resize(n, 0);
    } else {
        // Copied by a keyed blit at the scratch's hotspot; the depth plane goes through the
        // same keyed blit, so depth bytes equal to the key value are not copied.
        o.pixels.assign(n, o.key);
        out.depth.assign(n, 0);
        const int ox = o.x_offset - s.x_offset, oy = o.y_offset - s.y_offset;
        for (int y = 0; y < s.height; ++y)
            for (int x = 0; x < s.width; ++x) {
                const int dx = ox + x, dy = oy + y;
                if (dx < 0 || dy < 0 || dx >= o.width || dy >= o.height) continue;
                const std::size_t si = static_cast<std::size_t>(y) * s.width + x, di = static_cast<std::size_t>(dy) * o.width + dx;
                if (s.pixels[si] != s.key) o.pixels[di] = s.pixels[si];
                if (si < sprite.depth.size() && sprite.depth[si] != s.key) out.depth[di] = sprite.depth[si];
            }
    }
    ModelDrawParams np;
    np.depth_bias = u.depth_bias();
    np.team_colour = u.team_colour;
    if (u.build_left != 0.0f && u.model) apply_nanoframe(o, out.depth, *u.model, u.pose, np, u.build_left, u.id, in.tick);
    if (!(u.structure && u.build_left != 0.0f)) rasterize_unit_pieces(out, u, 0, false, false, in.tables);
    // Units on board: their own all-pieces sprite (with the construction look), merged by depth.
    for (const Placed& p : placed) {
        const LookUnit& c = *p.unit;
        // A structure on board drawn at double size uses the very scratch being composed:
        // the composite so far is replaced by that drawing (as in the original).
        LookFrame cs = build_unit_sprite(c, true, -1, in.options, in.tables, &out);
        n = static_cast<std::size_t>(o.width) * static_cast<std::size_t>(o.height);
        ModelDrawParams cp;
        cp.depth_bias = c.depth_bias();
        cp.team_colour = c.team_colour;
        if (c.build_left != 0.0f && c.model) apply_nanoframe(cs.image, cs.depth, *c.model, c.pose, cp, c.build_left, c.id, in.tick);
        const int col = o.x_offset - cs.image.x_offset + p.dx;
        const int row = o.y_offset - cs.image.y_offset + (p.dz - (p.dy >> 1));
        if (col < 0 || row < 0) continue;
        for (int y = 0; y < cs.image.height; ++y)
            for (int x = 0; x < cs.image.width; ++x) {
                const std::size_t si = static_cast<std::size_t>(y) * cs.image.width + x;
                const std::size_t di = static_cast<std::size_t>(row + y) * o.width + static_cast<std::size_t>(col + x);
                if (di >= n) continue;  // (the original writes on past the scratch)
                const std::uint8_t px = cs.image.pixels[si];
                if (px == cs.image.key) continue;
                const int d = cs.depth[si] + p.dy;
                if (out.depth[di] <= d) {
                    o.pixels[di] = px;
                    out.depth[di] = static_cast<std::uint8_t>(d);
                }
            }
    }
    // Under the sea level: cut away, or tinted for units the viewer may see through water.
    const int below = in.sea_level - hi16(u.pos[1]);
    if (below > 0) {
        const auto level = static_cast<std::uint8_t>(below + u.depth_bias());
        if (in.own_view && in.tables.water) tint_below(out, level, *in.tables.water);
        else if (!in.own_view) cut_below(out, level);
    }
    if (u.type_flags & look_type_flags::kDigger) cut_below(out, 0x7D);
    return out;
}

Sprite8 mobile_shadow(const LookFrame& sprite, int cut_depth) {
    LookFrame s = sprite;
    for (auto& p : s.image.pixels)
        if (p != s.image.key) p = 0;
    if (cut_depth >= 0) cut_below(s, static_cast<std::uint8_t>(cut_depth));
    return std::move(s.image);
}

Sprite8 structure_shadow(const LookUnit& u, const Sprite8& unit_sprite) {
    // Bounds of the projection along the light: (x + y/4, -z - y/4), from the origin.
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    auto proj = [](const std::int32_t* p, int& x, int& y) {
        const int q = hi16(p[1]) >> 2;
        x = hi16(p[0]) + q;
        y = hi16(wsub(0, p[2])) - q;
    };
    for (std::size_t i = u.pose.pieces.size(); i-- > 0;) {
        const sim::PosePiece& pc = u.pose.pieces[i];
        if (!visible(pc)) continue;
        for (std::size_t v = 0; v + 2 < pc.points.size(); v += 3) {
            int x, y;
            proj(&pc.points[v], x, y);
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
    }
    Sprite8 s;
    s.width = x1 - x0 + 4;
    s.height = y1 - y0 + 4;
    s.x_offset = 2 - x0;
    s.y_offset = 2 - y0;
    s.key = 1;
    s.pixels.assign(static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height), 1);
    RasterTarget rt;
    rt.pixels = s.pixels.data();
    rt.pitch = s.width;
    rt.clip = {0, 0, s.width - 1, s.height - 1};
    std::vector<RasterVertex> pts, vs;
    for (std::size_t i = u.pose.pieces.size(); i-- > 0;) {
        const sim::PosePiece& pc = u.pose.pieces[i];
        if (!visible(pc) || !cached(pc) || !u.model || pc.node < 0 || static_cast<std::size_t>(pc.node) >= u.model->pieces.size()) continue;
        pts.clear();
        for (std::size_t v = 0; v + 2 < pc.points.size(); v += 3) {
            int x, y;
            proj(&pc.points[v], x, y);
            pts.push_back({x + s.x_offset, y + s.y_offset, 0});
        }
        const ModelPiece& piece = u.model->pieces[static_cast<std::size_t>(pc.node)];
        for (std::size_t q = static_cast<std::size_t>(piece.first_drawn); q < piece.primitives.size(); ++q) {
            vs.clear();
            for (std::uint16_t ix : piece.primitives[q].indices) vs.push_back(ix < pts.size() ? pts[ix] : RasterVertex{});
            if (!vs.empty()) fill_polygon(rt, vs.data(), static_cast<int>(vs.size()), 0);
        }
    }
    // Where the unit's sprite will be drawn (the shadow lands 5 pixels right of it), no shadow.
    const Sprite8& src = unit_sprite;
    const int a = 5 + src.x_offset - s.x_offset;
    const int bb = s.y_offset - src.y_offset;
    int src_col = 0, dst_col = 0, src_row = 0, dst_row = 0;
    if (a > 0) src_col = a;
    else dst_col = -a;
    if (bb < 0) src_row = -bb;
    else dst_row = bb;
    const int count = std::min<int>(s.width, src.width - src_col);
    const std::size_t n = s.pixels.size();
    for (; src_row < src.height; ++src_row, ++dst_row) {
        if (dst_row >= s.height) break;
        for (int k = 0; k < count; ++k) {
            const std::size_t si = static_cast<std::size_t>(src_row) * src.width + static_cast<std::size_t>(src_col + k);
            const std::size_t di = static_cast<std::size_t>(dst_row) * s.width + static_cast<std::size_t>(dst_col + k);
            if (si < src.pixels.size() && src.pixels[si] != src.key && di < n) s.pixels[di] = s.key;
        }
    }
    return s;
}

} // namespace render
