#include "render/model.h"

#include <array>

#include "sim/x87.h"

#include <algorithm>

namespace render {
namespace {

// Integer part of a 16.16 value as the original takes it: the high half as a signed short.
inline int hi16(std::int32_t v) { return static_cast<std::int16_t>(static_cast<std::uint32_t>(v) >> 16); }

const Sprite8* pick_frame(const ModelPrimitive& prim, const ModelDrawParams& p, bool use_animation) {
    const ModelTexture* tex = prim.texture;
    if (!tex || tex->frames.empty()) return nullptr;
    std::size_t f = 0;
    if (tex->animated) {
        if (tex->team_coloured) f = p.team_colour;
        else if (use_animation) {
            // The primitive's own cursor (advanced once per simulation step), else the counter.
            if (prim.cursor >= 0 && p.cursor_frames && static_cast<std::size_t>(prim.cursor) < p.cursor_frames->size())
                f = (*p.cursor_frames)[static_cast<std::size_t>(prim.cursor)];
            else f = p.animation_frame % tex->frames.size();
        }
    }
    if (f >= tex->frames.size()) return nullptr;  // the original reads past the frame table
    return &tex->frames[f];
}

void draw_piece(RasterTarget& t, const ModelPiece& piece, const std::vector<RasterVertex>& pts,
                const ModelDrawParams& p, bool use_animation) {
    std::vector<RasterVertex> vs;
    for (std::size_t k = static_cast<std::size_t>(piece.first_drawn); k < piece.primitives.size(); ++k) {
        const ModelPrimitive& prim = piece.primitives[k];
        vs.clear();
        for (std::uint16_t i : prim.indices) vs.push_back(i < pts.size() ? pts[i] : RasterVertex{});
        if (prim.flat) {
            if (!vs.empty()) fill_polygon(t, vs.data(), static_cast<int>(vs.size()), prim.colour);
        } else if (vs.size() == 4) {
            if (const Sprite8* f = pick_frame(prim, p, use_animation)) textured_quad(t, vs.data(), *f);
        }
    }
}

bool piece_visible(const sim::PosePiece& pc) { return (pc.flags & sim::pose_flags::kVisible) != 0; }
bool piece_cached(const sim::PosePiece& pc) { return (pc.flags & sim::pose_flags::kCached) != 0; }

} // namespace

RasterVertex project_sprite_point(const std::int32_t* p, int depth_bias) {
    const int x = hi16(p[0]);
    const int y = hi16(p[1]);
    const int z = hi16(static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(p[2])));
    return {x, z - (y >> 1), y + depth_bias};
}

Sprite8 rasterize_model_sprite(const Model& model, const sim::PoseState& pose, const ModelDrawParams& params) {
    int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    for (std::size_t i = pose.pieces.size(); i-- > 0;) {
        const sim::PosePiece& pc = pose.pieces[i];
        if (!piece_visible(pc)) continue;
        for (std::size_t v = 0; v + 2 < pc.points.size(); v += 3) {
            const RasterVertex r = project_sprite_point(&pc.points[v], 0);
            min_x = std::min(min_x, r.x);
            max_x = std::max(max_x, r.x);
            min_y = std::min(min_y, r.y);
            max_y = std::max(max_y, r.y);
        }
    }
    Sprite8 s;
    s.width = max_x - min_x + 4;
    s.height = max_y - min_y + 4;
    s.x_offset = 2 - min_x;
    s.y_offset = 2 - min_y;
    s.key = 1;
    s.pixels.assign(static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height), 1);

    RasterTarget t;
    t.pixels = s.pixels.data();
    t.pitch = s.width;
    t.clip = {0, 0, s.width - 1, s.height - 1};
    std::vector<RasterVertex> pts;
    for (std::size_t i = pose.pieces.size(); i-- > 0;) {
        const sim::PosePiece& pc = pose.pieces[i];
        if (!piece_visible(pc) || pc.node < 0 || static_cast<std::size_t>(pc.node) >= model.pieces.size()) continue;
        if (!params.all_pieces && !piece_cached(pc)) continue;
        pts.clear();
        for (std::size_t v = 0; v + 2 < pc.points.size(); v += 3) {
            RasterVertex r = project_sprite_point(&pc.points[v], params.depth_bias);
            r.x += s.x_offset;
            r.y += s.y_offset;
            pts.push_back(r);
        }
        draw_piece(t, model.pieces[static_cast<std::size_t>(pc.node)], pts, params, false);
    }
    return s;
}

Sprite8 rasterize_model_sprite_depth(const Model& model, const sim::PoseState& pose, const ModelDrawParams& params,
                                     std::vector<std::uint8_t>& depth) {
    // Same bounds and order as the plain sprite, drawn with a depth plane (cleared to 0).
    Sprite8 s = rasterize_model_sprite(model, pose, ModelDrawParams{params.depth_bias, params.team_colour, params.animation_frame, true, true, params.cursor_frames});
    std::fill(s.pixels.begin(), s.pixels.end(), static_cast<std::uint8_t>(1));
    depth.assign(s.pixels.size(), 0);
    RasterTarget t;
    t.pixels = s.pixels.data();
    t.depth = depth.data();
    t.pitch = s.width;
    t.clip = {0, 0, s.width - 1, s.height - 1};
    std::vector<RasterVertex> pts;
    for (std::size_t i = pose.pieces.size(); i-- > 0;) {
        const sim::PosePiece& pc = pose.pieces[i];
        if (!piece_visible(pc) || pc.node < 0 || static_cast<std::size_t>(pc.node) >= model.pieces.size()) continue;
        if (!params.all_pieces && !piece_cached(pc)) continue;
        pts.clear();
        for (std::size_t v = 0; v + 2 < pc.points.size(); v += 3) {
            RasterVertex r = project_sprite_point(&pc.points[v], params.depth_bias);
            r.x += s.x_offset;
            r.y += s.y_offset;
            pts.push_back(r);
        }
        draw_piece(t, model.pieces[static_cast<std::size_t>(pc.node)], pts, params, false);
    }
    return s;
}

std::array<std::uint8_t, 2> nanoframe_colours(std::uint16_t unit_id, std::uint32_t tick) {
    auto pulse = [](std::uint32_t v) {
        return static_cast<std::uint8_t>((v & 0x10) ? 0xAF - (v & 0xF) : (v & 0xF) + 0xA0);
    };
    const std::uint32_t a = (static_cast<std::uint32_t>(unit_id) ^ 5u) + tick * 33u / 30u;
    const std::uint32_t b = (static_cast<std::uint32_t>(unit_id) ^ 9u) + tick * 57u / 30u;
    return {pulse(a), pulse(b)};
}

void apply_nanoframe(Sprite8& s, std::vector<std::uint8_t>& depth, const Model& model, const sim::PoseState& pose,
                     const ModelDrawParams& params, float build_left, std::uint16_t unit_id, std::uint32_t tick) {
    if (build_left == 0.0f || depth.size() != s.pixels.size()) return;
    const auto colours = nanoframe_colours(unit_id, tick);
    const int c1 = colours[0], c2 = colours[1];
    // The band and what goes above (p_above), below (p_below) and in it, by how much is left
    // (0..255): -2 = transparent, -1 = unchanged, else a palette index.
    const int v = static_cast<int>(sim::x87::ftol32(static_cast<double>(build_left) * 255.0));
    int band = 0, above = -1, below = -1, in_band = c1;
    if (v > 0xEB) {
        band = (v - 0xEB) * 0xFF / 20;
        above = below = -2;
    } else if (v > 200) {
        band = (v - 200) * 0xFF / 35;
        above = below = -2;
    } else if (v > 0x73) {
        band = (0x73 - v) * 0xFF / 0x55 - 1;
        above = -2;
        below = c1;
        in_band = c2;
    } else if (v > 0x1E) {
        band = (0x1E - v) * 0xFF / 0x55 - 1;
        above = c1;
        below = -1;
        in_band = c2;
    } else {
        band = v * 0xFF / 0x1E;
    }
    const auto b = static_cast<std::uint8_t>(band);
    const std::uint8_t lo = b < 4 ? 0 : static_cast<std::uint8_t>(b - 4);
    for (std::size_t i = 0; i < s.pixels.size(); ++i) {
        if (s.pixels[i] == s.key) continue;
        const std::uint8_t d = depth[i];
        const int mode = d < lo ? below : d < b ? in_band : above;
        if (mode == -2) s.pixels[i] = s.key;
        else if (mode != -1) s.pixels[i] = static_cast<std::uint8_t>(mode);
    }
    // Then every primitive of every visible piece is outlined in the second colour.
    RasterTarget t;
    t.pixels = s.pixels.data();
    t.depth = depth.data();
    t.pitch = s.width;
    t.clip = {0, 0, s.width - 1, s.height - 1};
    std::vector<RasterVertex> pts, vs;
    for (std::size_t i = pose.pieces.size(); i-- > 0;) {
        const sim::PosePiece& pc = pose.pieces[i];
        if (!piece_visible(pc) || pc.node < 0 || static_cast<std::size_t>(pc.node) >= model.pieces.size()) continue;
        pts.clear();
        for (std::size_t k = 0; k + 2 < pc.points.size(); k += 3) {
            RasterVertex r = project_sprite_point(&pc.points[k], params.depth_bias);
            r.x += s.x_offset;
            r.y += s.y_offset;
            pts.push_back(r);
        }
        const ModelPiece& piece = model.pieces[static_cast<std::size_t>(pc.node)];
        for (std::size_t p = static_cast<std::size_t>(piece.first_drawn); p < piece.primitives.size(); ++p) {
            vs.clear();
            for (std::uint16_t ix : piece.primitives[p].indices) vs.push_back(ix < pts.size() ? pts[ix] : RasterVertex{});
            if (vs.empty()) continue;
            vs.push_back(vs.front());  // closed
            outline_polygon(t, vs.data(), static_cast<int>(vs.size()), static_cast<std::uint8_t>(c2));
        }
    }
}

void draw_uncached_pieces(RasterTarget& target, const Model& model, const sim::PoseState& pose,
                          std::int32_t unit_view_x, std::int32_t unit_y, std::int32_t unit_view_z,
                          int origin_x, int origin_y, const ModelDrawParams& params) {
    std::vector<RasterVertex> pts;
    for (std::size_t i = pose.pieces.size(); i-- > 0;) {
        const sim::PosePiece& pc = pose.pieces[i];
        if (!piece_visible(pc) || (!params.all_pieces && piece_cached(pc))) continue;
        if (pc.node < 0 || static_cast<std::size_t>(pc.node) >= model.pieces.size()) continue;
        pts.clear();
        for (std::size_t v = 0; v + 2 < pc.points.size(); v += 3) {
            const auto add = [](std::int32_t a, std::int32_t b) {
                return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
            };
            const int x = hi16(add(pc.points[v], unit_view_x)) + origin_x;
            const int z = hi16(static_cast<std::int32_t>(static_cast<std::uint32_t>(unit_view_z) -
                                                         static_cast<std::uint32_t>(pc.points[v + 2])));
            const int y = hi16(add(pc.points[v + 1], unit_y));
            pts.push_back({x, z - (y >> 1) + origin_y, 0});
        }
        draw_piece(target, model.pieces[static_cast<std::size_t>(pc.node)], pts, params, params.direct_cursor);
    }
}

void draw_model_piece(RasterTarget& t, const ModelPiece& piece, const std::vector<RasterVertex>& pts,
                      const ModelDrawParams& params, bool use_animation) {
    draw_piece(t, piece, pts, params, use_animation);
}

sim::PoseState pose_for_drawing(const sim::PoseState& pose, std::int16_t pitch, std::uint16_t heading,
                                std::int16_t bank) {
    sim::PoseState copy = pose;
    if (copy.model()) copy.sync(pitch, heading, bank);
    return copy;
}

namespace {

// Everything of a pose state the sync reads or keeps.
bool same_pose(const sim::PoseState& a, const sim::PoseState& b) {
    if (a.model() != b.model() || a.root != b.root || a.anim_dirty != b.anim_dirty || a.cache_draw_count != b.cache_draw_count ||
        a.base_angle != b.base_angle || a.fault() != b.fault() || a.pieces.size() != b.pieces.size())
        return false;
    for (std::size_t i = 0; i < a.pieces.size(); ++i) {
        const sim::PosePiece& p = a.pieces[i];
        const sim::PosePiece& q = b.pieces[i];
        if (p.node != q.node || p.move != q.move || p.turn != q.turn || p.xform != q.xform || p.skip_transform != q.skip_transform ||
            p.flags != q.flags || p.sibling != q.sibling || p.child != q.child || p.parent != q.parent || p.points != q.points)
            return false;
    }
    return true;
}

} // namespace

const sim::PoseState& pose_for_drawing(PoseDrawCache& cache, std::size_t id, const sim::PoseState& pose, std::int16_t pitch,
                                       std::uint16_t heading, std::int16_t bank) {
    if (cache.entries.size() <= id) cache.entries.resize(id + 1);
    PoseDrawCache::Entry& e = cache.entries[id];
    if (e.valid && e.pitch == pitch && e.heading == heading && e.bank == bank && same_pose(e.in, pose)) return e.out;
    e.in = pose;
    e.out = pose;
    if (e.out.model()) e.out.sync(pitch, heading, bank);
    e.pitch = pitch;
    e.heading = heading;
    e.bank = bank;
    e.valid = true;
    ++e.version;
    return e.out;
}

} // namespace render
