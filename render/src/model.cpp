#include "render/model.h"

#include <algorithm>

namespace render {
namespace {

// Integer part of a 16.16 value as the original takes it: the high half as a signed short.
inline int hi16(std::int32_t v) { return static_cast<std::int16_t>(static_cast<std::uint32_t>(v) >> 16); }

const Sprite8* pick_frame(const ModelTexture* tex, const ModelDrawParams& p, bool use_animation) {
    if (!tex || tex->frames.empty()) return nullptr;
    std::size_t f = 0;
    if (tex->animated) {
        if (tex->team_coloured) f = p.team_colour;
        else if (use_animation) f = p.animation_frame % tex->frames.size();
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
            if (const Sprite8* f = pick_frame(prim.texture, p, use_animation)) textured_quad(t, vs.data(), *f);
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

void draw_uncached_pieces(RasterTarget& target, const Model& model, const sim::PoseState& pose,
                          std::int32_t unit_view_x, std::int32_t unit_y, std::int32_t unit_view_z,
                          int origin_x, int origin_y, const ModelDrawParams& params) {
    std::vector<RasterVertex> pts;
    for (std::size_t i = pose.pieces.size(); i-- > 0;) {
        const sim::PosePiece& pc = pose.pieces[i];
        if (!piece_visible(pc) || piece_cached(pc)) continue;
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
        draw_piece(target, model.pieces[static_cast<std::size_t>(pc.node)], pts, params, true);
    }
}

sim::PoseState pose_for_drawing(const sim::PoseState& pose, std::int16_t pitch, std::uint16_t heading,
                                std::int16_t bank) {
    sim::PoseState copy = pose;
    if (copy.model()) copy.sync(pitch, heading, bank);
    return copy;
}

} // namespace render
