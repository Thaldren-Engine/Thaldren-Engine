#include "render/battle_view.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "sim/x87.h"
#include "sim/x87_trig.h"
#include <unordered_map>

namespace render {
namespace {

inline int trunc_div16(int v) { return v / 16; }  // C truncation (the original adds 15 to negatives, then shifts)

int hi16(std::int32_t v) { return static_cast<std::int16_t>(static_cast<std::uint32_t>(v) >> 16); }

void draw_terrain(const BattleScene& s, DrawList& out) {
    const Terrain& t = s.terrain;
    const Camera& c = s.camera;
    if (!t.map || !t.tiles || t.width <= 0) return;
    const ClipRect clip = c.view_clip();
    // Tiles covering the view; each is copied whole and clipped to the view.
    const int tx0 = std::max(0, c.scroll_x >> 5), ty0 = std::max(0, c.scroll_y >> 5);
    const int tx1 = std::min(t.width - 1, (c.scroll_x + c.view_width - 1) >> 5);
    const int ty1 = std::min(t.height - 1, (c.scroll_y + c.view_height - 1) >> 5);
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) {
            const std::size_t m = static_cast<std::size_t>(ty) * static_cast<std::size_t>(t.width) + static_cast<std::size_t>(tx);
            if (m >= t.map->size()) continue;
            const std::uint16_t idx = (*t.map)[m];
            if (idx >= t.tiles->size()) continue;
            out.blit(&(*t.tiles)[idx], c.origin_x + tx * 32 - c.scroll_x, c.origin_y + ty * 32 - c.scroll_y, clip, true);
        }
}

// Draws through `draw` straight onto a frame-sized scratch (restricted to `clip`) and keeps
// only the pixels written, as a transient sprite: `draw` runs twice, once for the colours
// and once with `mask` set, where it must write 1 wherever it would write a colour.
template <class Draw> void emit_direct(const Camera& c, const ClipRect& clip, DrawList& out, Draw&& draw) {
    Image8 scratch(c.screen_width, c.screen_height, 0);
    Image8 mask(c.screen_width, c.screen_height, 0);
    RasterTarget t;
    t.pixels = scratch.pixels.data();
    t.pitch = scratch.width;
    t.clip = clip;
    draw(t, false);
    RasterTarget m = t;
    m.pixels = mask.pixels.data();
    draw(m, true);
    int x0 = c.screen_width, y0 = c.screen_height, x1 = -1, y1 = -1;
    for (int y = 0; y < mask.height; ++y)
        for (int x = 0; x < mask.width; ++x)
            if (mask.row(y)[x]) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    if (x1 < 0) return;
    Sprite8 d;
    d.width = x1 - x0 + 1;
    d.height = y1 - y0 + 1;
    std::vector<std::uint8_t> written;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (mask.row(y)[x]) written.push_back(scratch.row(y)[x]);
    const int key = unused_index(written);
    d.key = static_cast<std::uint8_t>(key < 0 ? 0 : key);
    d.pixels.resize(static_cast<std::size_t>(d.width) * static_cast<std::size_t>(d.height));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            d.pixels[static_cast<std::size_t>(y - y0) * static_cast<std::size_t>(d.width) + static_cast<std::size_t>(x - x0)] =
                mask.row(y)[x] ? scratch.row(y)[x] : d.key;
    out.blit(out.keep(std::move(d)), x0, y0, clip, false);
}

// A copy of `piece` whose primitives all draw in colour 1 (the mask pass of emit_direct).
ModelPiece solid_piece(const ModelPiece& piece) {
    ModelPiece p = piece;
    for (auto& prim : p.primitives) {
        if (!prim.flat && (prim.indices.size() != 4 || !prim.texture)) continue;
        prim.flat = true;
        prim.colour = 1;
    }
    return p;
}

void draw_model_at(const BattleScene& s, const Model& model, const sim::PoseModel& pm, const std::array<std::int32_t, 3>& pos,
                   std::int16_t pitch, std::uint16_t heading, std::int16_t bank, DrawList& out) {
    const Camera& c = s.camera;
    sim::PoseState pose;
    pose.build(pm, {});
    pose.sync(pitch, heading, bank);
    ModelDrawParams mp;
    mp.all_pieces = true;
    out.blit(out.keep(rasterize_model_sprite(model, pose, mp)), screen_x(c, pos[0]), screen_y(c, pos[1], pos[2]), c.view_clip(), false);
}

void draw_unit(const BattleScene& s, const UnitInstance& ui, DrawList& out);

void draw_feature(const BattleScene& s, const FeatureInstance& f, DrawList& out) {
    const Camera& c = s.camera;
    if (f.proxy) {
        draw_unit(s, *f.proxy, out);
        return;
    }
    if (f.model && f.pose_model) {
        // Drawn through a stand-in unit at the slot's place (the original's proxy unit).
        draw_model_at(s, *f.model, *f.pose_model, f.pos, f.pitch, f.heading, f.bank, out);
        return;
    }
    int sum = 0;
    if (s.heights) {
        sum = s.heights->cell_height(f.cell_x, f.cell_z) + s.heights->cell_height(f.cell_x + 1, f.cell_z) +
              s.heights->cell_height(f.cell_x, f.cell_z + 1) + s.heights->cell_height(f.cell_x + 1, f.cell_z + 1);
    }
    const int x = (f.footprint_x * 16) / 2 + f.cell_x * 16 + c.origin_x - c.scroll_x;
    const int y = (f.footprint_z * 16) / 2 - (sum >> 3) + f.cell_z * 16 + c.origin_y - c.scroll_y;
    const ClipRect clip = c.view_clip();
    if (f.shadow && s.feature_shadows && s.options.feature_shadows) {
        if (f.shadow_blended) out.blend(f.shadow, x, y, clip, true);
        else out.blit(f.shadow, x, y, clip, true);
    }
    if (f.image) {
        if (f.image_blended) out.blend(f.image, x, y, clip, true);
        else out.blit(f.image, x, y, clip, true);
    }
}

LookUnit look_unit(const BattleScene& s, const UnitInstance& ui) {
    const sim::Unit& u = *ui.unit;
    LookUnit lu;
    lu.model = ui.model;
    lu.pose = s.pose_cache ? pose_for_drawing(*s.pose_cache, u.id, u.pose, u.pitch, u.heading, u.bank)
                           : pose_for_drawing(u.pose, u.pitch, u.heading, u.bank);
    lu.pos = u.pos;
    lu.team_colour = ui.params.team_colour;
    lu.texture_frame = ui.params.animation_frame;
    lu.cursor_frames = ui.params.cursor_frames;
    lu.type_flags = ui.type_flags | (ui.shadow ? 0u : look_type_flags::kNoShadow);
    lu.structure = (u.flags & 0x20000000u) != 0;
    lu.zbuffer = (u.draw_flags & 1u) != 0;
    lu.build_left = u.build_fraction_left;
    lu.id = u.id;
    (void)s;
    return lu;
}

// The unit's plain sprite through the sprite cache: made again when the drawn pose (by its
// version in the pose cache) or any other input of build_unit_sprite changed. (The texture
// cursor is not an input: the cached pieces are drawn without it.)
const LookFrame& cached_unit_sprite(const BattleScene& s, const LookUnit& lu, std::uint32_t pose_version) {
    auto& list = s.sprite_cache->entries;
    if (list.size() <= lu.id) list.resize(static_cast<std::size_t>(lu.id) + 1);
    UnitSpriteCache::Entry& e = list[lu.id];
    std::uint32_t build_bits;
    std::memcpy(&build_bits, &lu.build_left, 4);
    const ViewOptions& o = s.options;
    const bool same = e.valid && e.pose_version == pose_version && e.model == lu.model && e.team_colour == lu.team_colour &&
                      e.type_flags == lu.type_flags && e.structure == lu.structure && e.zbuffer == lu.zbuffer &&
                      e.build_left_bits == build_bits && e.options.shadows == o.shadows && e.options.unit_shadows == o.unit_shadows &&
                      e.options.feature_shadows == o.feature_shadows && e.options.antialias == o.antialias &&
                      e.options.shading == o.shading && e.tables.mix == s.tables.mix && e.tables.shade == s.tables.shade &&
                      e.tables.water == s.tables.water && e.tables.light == s.tables.light;
    if (same) return e.sprite;
    e.sprite = build_unit_sprite(lu, false, 1, s.options, s.tables);
    e.valid = true;
    e.pose_version = pose_version;
    e.model = lu.model;
    e.team_colour = lu.team_colour;
    e.type_flags = lu.type_flags;
    e.structure = lu.structure;
    e.zbuffer = lu.zbuffer;
    e.build_left_bits = build_bits;
    e.options = o;
    e.tables = s.tables;
    return e.sprite;
}

// One unit (the original's 0x45AC20 / 0x458810 / 0x459200): shadow, then either the
// depth-plane composite as one sprite, or the plain sprite followed by the pieces that are
// not cached and the units on board drawn straight onto the frame.
void draw_unit(const BattleScene& s, const UnitInstance& ui, DrawList& out) {
    const sim::Unit& u = *ui.unit;
    if (ui.outline) {
        // The selection outline comes first (under the unit), whatever else is drawn of it.
        const ClipRect vc = s.camera.view_clip();
        const auto& p = ui.outline_xy;
        for (int k = 0; k < 4; ++k) {
            const int n = (k + 1) % 4;
            out.line(p[static_cast<std::size_t>(2 * k)], p[static_cast<std::size_t>(2 * k + 1)], p[static_cast<std::size_t>(2 * n)],
                     p[static_cast<std::size_t>(2 * n + 1)], ui.outline_colour, vc);
        }
    }
    if (u.carrier || !ui.model) return;  // carried units are drawn with their carrier
    if (s.drawn_units) s.drawn_units->push_back(&u);
    const Camera& c = s.camera;
    const ClipRect clip = c.view_clip();
    const LookUnit lu = look_unit(s, ui);
    // The drawn pose's version in the pose cache (taken before the units on board are looked at).
    const bool cached_pose = s.pose_cache && s.sprite_cache && u.id < s.pose_cache->entries.size();
    const std::uint32_t pose_version = cached_pose ? s.pose_cache->entries[u.id].version : 0;
    std::vector<LookUnit> cargo;
    std::vector<const UnitInstance*> cargo_ui;
    for (const sim::Unit* cu = u.cargo_head; cu; cu = cu->cargo_next) {
        if (cu->flags & 0x20000u) continue;  // hidden on board
        for (const UnitInstance& other : s.units)
            if (other.unit == cu && other.model) {
                cargo.push_back(look_unit(s, other));
                cargo_ui.push_back(&other);
            }
    }
    LookFrame own_sprite;
    const LookFrame& sprite = cached_pose ? cached_unit_sprite(s, lu, pose_version)
                                          : (own_sprite = build_unit_sprite(lu, false, 1, s.options, s.tables));
    const bool depth_path = !sprite.depth.empty();
    const int ux = screen_x(c, u.pos[0]);
    const int uy = screen_y(c, u.pos[1], u.pos[2]);
    const int below = s.sea_level - hi16(u.pos[1]);

    // Shadow: 5 pixels right, placed by the terrain height under the unit.
    if (s.options.shadows && s.unit_shadows && !(lu.type_flags & look_type_flags::kNoShadow)) {
        const int sy = hi16(u.pos[2] - (c.scroll_y << 16)) - (ui.ground_height >> 1) + c.origin_y;
        const bool digger = (lu.type_flags & look_type_flags::kDigger) != 0;
        const bool floats = (lu.type_flags & (look_type_flags::kHover | look_type_flags::kFloater)) != 0;
        Sprite8 sh;
        bool have = false;
        if (depth_path && digger) {
            sh = mobile_shadow(sprite, 125);
            have = true;
        } else if (!lu.structure || digger) {
            if (s.options.unit_shadows && !floats) {
                sh = mobile_shadow(sprite, depth_path && below > 0 ? static_cast<std::uint8_t>(below + 50) : -1);
                have = true;
            }
        } else {
            sh = structure_shadow(lu, sprite.image);
            have = true;
        }
        if (have) out.blend(out.keep(std::move(sh)), ux + 5, sy, clip, false);
    }

    if (depth_path) {
        DepthLookInput in;
        in.unit = &lu;
        for (const LookUnit& cu : cargo) in.cargo.push_back(&cu);
        in.own_view = ui.own_view;
        in.sea_level = s.sea_level;
        in.tick = s.game_tick;
        in.options = s.options;
        in.tables = s.tables;
        LookFrame look = compose_depth_look(in, sprite);
        if (ui.translucent) out.blend(out.keep(std::move(look.image)), ux, uy, clip, false);
        else out.blit(out.keep(std::move(look.image)), ux, uy, clip, false);
        return;
    }
    if (ui.translucent) out.blend(out.keep(Sprite8(sprite.image)), ux, uy, clip, false);
    else out.blit(out.keep(Sprite8(sprite.image)), ux, uy, clip, false);

    // Pieces that are not cached, then every visible piece of the units on board, straight
    // onto the frame (texture cursor only while the carrier moves / is active).
    auto direct = [&](const UnitInstance& who, const LookUnit& l, bool all) {
        bool any = false;
        for (const auto& pc : l.pose.pieces)
            if ((pc.flags & sim::pose_flags::kVisible) && (all || !(pc.flags & sim::pose_flags::kCached))) any = true;
        if (!any) return;
        ModelDrawParams mp = who.params;
        mp.all_pieces = all;
        mp.direct_cursor = ui.animate_direct;
        const std::int32_t vx = who.unit->pos[0] - (c.scroll_x << 16), vz = who.unit->pos[2] - (c.scroll_y << 16);
        Model solid = *who.model;
        for (auto& p : solid.pieces) p = solid_piece(p);
        emit_direct(c, clip, out, [&](RasterTarget& t, bool mask) {
            draw_uncached_pieces(t, mask ? solid : *who.model, l.pose, vx, who.unit->pos[1], vz, c.origin_x, c.origin_y, mp);
        });
    };
    direct(ui, lu, false);
    for (std::size_t k = 0; k < cargo.size(); ++k) direct(*cargo_ui[k], cargo[k], true);
}

// A debris piece: its model piece's vertices turned by the piece's angles, drawn straight
// onto the frame around the piece origin (the original's 0x4211D0), culled by that origin.
void draw_debris(const BattleScene& s, const DebrisInstance& d, DrawList& out) {
    const Camera& c = s.camera;
    if (!d.model || !d.pose_model || d.node < 0 || static_cast<std::size_t>(d.node) >= d.model->pieces.size() ||
        static_cast<std::size_t>(d.node) >= d.pose_model->nodes.size())
        return;
    const ClipRect clip = c.view_clip();
    const int sx = screen_x(c, d.pos[0]), sy = screen_y(c, d.pos[1], d.pos[2]);
    if (sx < clip.x0 || sx > clip.x1 || sy < clip.y0 || sy > clip.y1) return;
    const auto& verts = d.pose_model->nodes[static_cast<std::size_t>(d.node)].vertices;
    const std::int16_t angles[3] = {d.turn[2], d.turn[1], d.turn[0]};  // z, y, x
    const std::int32_t vx = d.pos[0] - (c.scroll_x << 16), vz = d.pos[2] - (c.scroll_y << 16);
    std::vector<RasterVertex> pts;
    for (const auto& v : verts) {
        std::int32_t r[3];
        sim::x87::rotate_point_3d(v.data(), r, angles);
        const int x = hi16(static_cast<std::int32_t>(static_cast<std::uint32_t>(vx) + static_cast<std::uint32_t>(r[0]))) + c.origin_x;
        const int z = hi16(static_cast<std::int32_t>(static_cast<std::uint32_t>(vz) - static_cast<std::uint32_t>(r[2])));
        const int y = hi16(static_cast<std::int32_t>(static_cast<std::uint32_t>(d.pos[1]) + static_cast<std::uint32_t>(r[1])));
        pts.push_back({x, z - (y >> 1) + c.origin_y, 0});
    }
    const ModelPiece& piece = d.model->pieces[static_cast<std::size_t>(d.node)];
    const ModelPiece solid = solid_piece(piece);
    ModelDrawParams mp;
    mp.team_colour = d.team_colour;
    emit_direct(c, clip, out, [&](RasterTarget& t, bool mask) { draw_model_piece(t, mask ? solid : piece, pts, mp, true); });
}

// One model node drawn straight onto the frame (the original's 0x46BAE0): each vertex turned
// by `angles` (Math_RotatePointByAngles, the angles as stored), offset by the point
// (view-relative x and z, world y) and projected (x + 128, z - y / 2 + 32 on the integer
// parts); its primitives in order as the direct paths draw them (no depth, no culling).
template <class Vertices>
void draw_points_projected(const BattleScene& s, const ModelPiece& piece, const Vertices& vertices, const std::array<std::int32_t, 3>& rel,
                           const std::int16_t* angles, const ModelDrawParams& mp, DrawList& out) {
    const Camera& c = s.camera;
    std::vector<RasterVertex> pts;
    pts.reserve(vertices.size());
    for (const auto& v : vertices) {
        std::int32_t r[3];
        sim::x87::rotate_point_3d(v.data(), r, angles);
        const int x = hi16(static_cast<std::int32_t>(static_cast<std::uint32_t>(r[0]) + static_cast<std::uint32_t>(rel[0]))) + c.origin_x;
        const int z = hi16(static_cast<std::int32_t>(static_cast<std::uint32_t>(rel[2]) - static_cast<std::uint32_t>(r[2])));
        const int y = hi16(static_cast<std::int32_t>(static_cast<std::uint32_t>(r[1]) + static_cast<std::uint32_t>(rel[1])));
        pts.push_back({x, z - (y >> 1) + c.origin_y, 0});
    }
    const ModelPiece solid = solid_piece(piece);
    emit_direct(c, c.view_clip(), out, [&](RasterTarget& t, bool mask) { draw_model_piece(t, mask ? solid : piece, pts, mp, true); });
}

void draw_node_projected(const BattleScene& s, const ProjectileInstance::Node& n, const std::array<std::int32_t, 3>& rel,
                         const std::array<std::int16_t, 3>& angles, std::uint8_t team_colour, DrawList& out) {
    if (!n.piece || !n.vertices) return;
    ModelDrawParams mp;
    mp.team_colour = team_colour;
    draw_points_projected(s, *n.piece, *n.vertices, rel, angles.data(), mp, out);
}

// The shard of a shattered quad (the original's 0x420B00 → 0x46BAE0 with the shard object made
// by 0x420620 / 0x421700): 8 points, 6 four-sided faces from the index table at 0x502BF8, no
// selection primitive, drawn like a model shot around the effect's point with the effect's
// angles (+0x4C) as stored. Each face copies the quad's colour, texture and animation cursor
// at the shattering (the copy is not advanced: its frame stays), a team texture takes the
// owner's frame.
void draw_shard(const BattleScene& s, const EffectInstance& e, DrawList& out) {
    static const std::uint16_t kFaces[6][4] = {{0, 1, 2, 3}, {2, 1, 6, 5}, {0, 3, 4, 7}, {1, 0, 7, 6}, {3, 2, 5, 4}, {4, 5, 6, 7}};
    if (!e.look) return;
    const std::vector<std::uint16_t> frozen{static_cast<std::uint16_t>(e.look_frame < 0 ? 0 : e.look_frame)};
    ModelPiece piece;
    for (const auto& f : kFaces) {
        ModelPrimitive prim = *e.look;
        prim.indices.assign(f, f + 4);
        prim.cursor = 0;
        piece.primitives.push_back(std::move(prim));
    }
    const Camera& c = s.camera;
    const std::array<std::int32_t, 3> rel = {
        static_cast<std::int32_t>(static_cast<std::uint32_t>(e.pos[0]) - (static_cast<std::uint32_t>(c.scroll_x) << 16)), e.pos[1],
        static_cast<std::int32_t>(static_cast<std::uint32_t>(e.pos[2]) - (static_cast<std::uint32_t>(c.scroll_y) << 16))};
    ModelDrawParams mp;
    mp.team_colour = e.team_colour;
    mp.cursor_frames = &frozen;
    draw_points_projected(s, piece, e.points, rel, e.angle.data(), mp, out);
}

// A shot by its weapon's render type (the original's 0x49BE60). Returns false when the
// original stops drawing the remaining shots (a lens shot outside the view).
bool draw_projectile_typed(const BattleScene& s, const ProjectileInstance& p, DrawList& out, std::uint32_t& rand_state) {
    const Camera& c = s.camera;
    const ClipRect clip = c.view_clip();
    const int x = screen_x(c, p.pos[0]);
    const int y = screen_y(c, p.pos[1], p.pos[2]);
    const std::array<std::int32_t, 3> rel = {
        static_cast<std::int32_t>(static_cast<std::uint32_t>(p.pos[0]) - (static_cast<std::uint32_t>(c.scroll_x) << 16)), p.pos[1],
        static_cast<std::int32_t>(static_cast<std::uint32_t>(p.pos[2]) - (static_cast<std::uint32_t>(c.scroll_y) << 16))};
    // The blob: at the shot's x and its z row less half its ground height word.
    auto blob = [&]() {
        if (p.blob) out.blend(p.blob, x, hi16(rel[2]) - (p.ground >> 1) + c.origin_y, clip, true);
    };
    switch (p.render_type) {
    case 0: {
        const int tx = screen_x(c, p.tail[0]), ty = screen_y(c, p.tail[1], p.tail[2]);
        if (!p.second_line) {
            out.line(x, y, tx, ty, p.colour, clip);
            break;
        }
        // Two lines: the main one between the ends ordered along the longer axis, and one in the
        // second colour beside it (a row above for flat lines; for steep ones from one column left
        // of the first end to one column right of the second).
        int ax, ay, bx, by;
        const int dx = x - tx, dy = y - ty;
        if (std::abs(dy) < std::abs(dx)) {
            if (tx < x) ax = tx, ay = ty, bx = x, by = y;
            else ax = x, ay = y, bx = tx, by = ty;
            out.line(ax, ay - 1, bx, by - 1, p.colour2, clip);
        } else {
            if (ty < y) ax = tx, ay = ty, bx = x, by = y;
            else ax = x, ay = y, bx = tx, by = ty;
            out.line(ax - 1, ay, bx + 1, by, p.colour2, clip);
        }
        out.line(ax, ay, bx, by, p.colour, clip);
        break;
    }
    case 1:
        blob();
        draw_node_projected(s, p.node, rel, p.angles, p.team_colour, out);
        if (p.child.piece) draw_node_projected(s, p.child, rel, p.child_angles, p.team_colour, out);
        break;
    case 3:
    case 6:
        blob();
        draw_node_projected(s, p.node, rel, p.angles, p.team_colour, out);
        break;
    case 2:
        if (x < clip.x0 || x > clip.x1 || y < clip.y0 || y > clip.y1) return false;
        out.lens(x, y, p.lens_key, clip);
        break;
    case 4:
        if (p.frame) {
            blob();
            out.blit(p.frame, x, y, clip, true);
        } else if (p.blob) {
            blob();
        }
        break;
    case 5:
        if (p.frame) out.blend(p.frame, x, y, clip, true);
        break;
    case 7: {
        // Lightning: segments every 5 world units from the tail to the head, each end jittered by
        // up to 5 whole units on every axis with the C runtime rand(); two passes.
        const std::int32_t d[3] = {
            static_cast<std::int32_t>(static_cast<std::uint32_t>(p.pos[0]) - static_cast<std::uint32_t>(p.tail[0])),
            static_cast<std::int32_t>(static_cast<std::uint32_t>(p.pos[1]) - static_cast<std::uint32_t>(p.tail[1])),
            static_cast<std::int32_t>(static_cast<std::uint32_t>(p.pos[2]) - static_cast<std::uint32_t>(p.tail[2]))};
        // x87 at 53-bit precision: ((dx * dx + dy * dy) + dz * dz), square root, truncated.
        const double len2 = (static_cast<double>(d[0]) * d[0] + static_cast<double>(d[1]) * d[1]) + static_cast<double>(d[2]) * d[2];
        const std::int64_t len = static_cast<std::int64_t>(sim::x87::ftol32(std::sqrt(len2)));
        const std::int64_t n = (len * 65536) / 0x50000;
        if (static_cast<std::int32_t>(n) == 0) break;
        const std::int64_t nn = static_cast<std::int32_t>(n);
        std::int32_t step[3];
        for (int k = 0; k < 3; ++k) step[k] = static_cast<std::int32_t>((static_cast<std::int64_t>(d[k]) * 65536) / nn);
        const int segments = static_cast<std::int16_t>(static_cast<std::uint32_t>(n) >> 16);
        auto rnd = [&]() {
            rand_state = rand_state * 214013u + 2531011u;
            const std::int64_t r = static_cast<std::int64_t>((rand_state >> 16) & 0x7FFF);
            return static_cast<std::int32_t>((r * 11) / 0x8000);
        };
        const std::uint8_t col = p.colour;
        for (int pass = 0; pass < 2; ++pass) {
            std::int32_t acc[3] = {p.tail[0], p.tail[1], p.tail[2]};
            std::int32_t prev[3] = {p.tail[0], p.tail[1], p.tail[2]};
            for (int k = 0; k < segments; ++k) {
                std::int32_t cur[3];
                for (int a = 0; a < 3; ++a) {
                    acc[a] = static_cast<std::int32_t>(static_cast<std::uint32_t>(acc[a]) + static_cast<std::uint32_t>(step[a]));
                    cur[a] = acc[a];
                }
                for (int a = 0; a < 3; ++a) {
                    const std::int16_t whole = static_cast<std::int16_t>(static_cast<std::uint32_t>(cur[a]) >> 16);
                    const std::int16_t moved = static_cast<std::int16_t>(whole + static_cast<std::int16_t>(rnd() - 5));
                    cur[a] = static_cast<std::int32_t>((static_cast<std::uint32_t>(static_cast<std::uint16_t>(moved)) << 16) |
                                                       (static_cast<std::uint32_t>(cur[a]) & 0xFFFFu));
                }
                const int sx0 = hi16(prev[0]) - static_cast<std::int16_t>(c.scroll_x) + c.origin_x;
                const int sy0 = hi16(prev[2]) - (hi16(prev[1]) >> 1) - static_cast<std::int16_t>(c.scroll_y) + c.origin_y;
                const int sx1 = hi16(cur[0]) - static_cast<std::int16_t>(c.scroll_x) + c.origin_x;
                const int sy1 = hi16(cur[2]) - (hi16(cur[1]) >> 1) - static_cast<std::int16_t>(c.scroll_y) + c.origin_y;
                out.line(sx0, sy0, sx1, sy1, col, clip);
                for (int a = 0; a < 3; ++a) prev[a] = cur[a];
            }
        }
        break;
    }
    default:
        break;
    }
    return true;
}

void draw_projectile(const BattleScene& s, const ProjectileInstance& p, DrawList& out) {
    const Camera& c = s.camera;
    const ClipRect clip = c.view_clip();
    const int x = screen_x(c, p.pos[0]);
    const int y = screen_y(c, p.pos[1], p.pos[2]);
    switch (p.look) {
    case ProjectileLook::Line:
        out.line(x, y, screen_x(c, p.tail[0]), screen_y(c, p.tail[1], p.tail[2]), p.colour, clip);
        break;
    case ProjectileLook::Sprite:
        if (p.shadow) {
            const int sy = hi16(p.pos[2] - (c.scroll_y << 16)) - (p.ground_height >> 1) + c.origin_y;
            out.blend(p.shadow, x, sy, clip, true);
        }
        if (p.sprite) {
            if (p.sprite_blended) out.blend(p.sprite, x, y, clip, true);
            else out.blit(p.sprite, x, y, clip, true);
        }
        break;
    case ProjectileLook::Model:
        if (p.shadow) {
            const int sy = hi16(p.pos[2] - (c.scroll_y << 16)) - (p.ground_height >> 1) + c.origin_y;
            out.blend(p.shadow, x, sy, clip, true);
        }
        if (p.model && p.pose_model) {
            sim::PoseState pose;
            pose.build(*p.pose_model, {});
            pose.sync(p.pitch, p.heading, p.bank);
            ModelDrawParams mp;
            mp.all_pieces = true;
            out.blit(out.keep(rasterize_model_sprite(*p.model, pose, mp)), x, y, clip, false);
        }
        break;
    }
}

// C division by 32 (truncation) and the matching remainder.
int div32(int v) { return v / 32; }
int mod32(int v) { return v % 32; }

// The dithered fog's out-of-sight cell (the original's 0x4BFF20): within the cell clipped to
// the view, each row sets every other pixel to 0 — those whose x + y + parity is even — the
// unaligned start pixel by pixel, the middle four pixels at a time, the end pixel by pixel.
// The middle runs to the end rounded up to four, so when the clipped end x is 1 or 2 past a
// multiple of four the row's last group also sets one or two pixels past the cell.
void dither_cell(DrawList& out, const ClipRect& clip, int x, int y, int parity, int screen_w, int screen_h) {
    const int cx0 = std::max(x, clip.x0), cy0 = std::max(y, clip.y0);
    const int cx1 = std::min(x + 31, clip.x1), cy1 = std::min(y + 31, clip.y1);
    if (cx0 > cx1 || cy0 > cy1) return;
    const int a = (cx0 + 3) & ~3, b = (cx1 + 3) & ~3;
    const int right = std::max(std::max(a, b) + 4, cx1 + 1);
    Sprite8 sp;
    sp.width = right - cx0;
    sp.height = cy1 - cy0 + 1;
    sp.key = 1;
    sp.pixels.assign(static_cast<std::size_t>(sp.width) * static_cast<std::size_t>(sp.height), 1);
    for (int yy = cy0; yy <= cy1; ++yy) {
        std::uint8_t* row = sp.pixels.data() + static_cast<std::size_t>(yy - cy0) * static_cast<std::size_t>(sp.width);
        auto set = [&](int px) {
            if (px >= cx0 && px < right) row[px - cx0] = 0;
        };
        for (int p = cx0 + ((cx0 + yy + parity) & 1); p < a; p += 2) set(p);
        const bool even = ((yy + parity) & 1) == 0;
        int e = a;
        for (; e < b; e += 4) {
            set(even ? e : e + 1);
            set(even ? e + 2 : e + 3);
        }
        const int limit = e + (cx1 - b);
        for (int p = even ? e : e + 1; p <= limit; p += 2) set(p);
    }
    out.blit(out.keep(std::move(sp)), cx0, cy0, ClipRect{0, 0, screen_w - 1, screen_h - 1}, false);
}

// The dithered fog's edge frame (the original's 0x4B88D0): where the frame has a non-key pixel
// and x + y + parity is even, the screen pixel is set to 0.
void dither_frame(DrawList& out, const ClipRect& clip, const Sprite8& frame, int x, int y, int parity) {
    Sprite8 sp;
    sp.width = frame.width;
    sp.height = frame.height;
    sp.x_offset = frame.x_offset;
    sp.y_offset = frame.y_offset;
    sp.key = 1;
    sp.pixels.assign(frame.pixels.size(), 1);
    const int left = x - frame.x_offset, top = y - frame.y_offset;
    for (int j = 0; j < frame.height; ++j)
        for (int i = 0; i < frame.width; ++i) {
            const bool solid = !frame.has_key || frame.at(i, j) != frame.key;
            if (solid && ((left + i + top + j + parity) & 1) == 0)
                sp.pixels[static_cast<std::size_t>(j) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(i)] = 0;
        }
    out.blit(out.keep(std::move(sp)), x, y, clip, false);
}

// The fog grid over the view and its drawing (the original's 0x4843C0 / 0x4848E0).
void draw_fog(const BattleScene& s, DrawList& out) {
    const FogOfWar& f = *s.fog;
    const Camera& c = s.camera;
    const int gw = c.view_width / 16 / 2 + ((c.view_width & 31) ? 3 : 2);
    const int gh = c.view_height / 16 / 2 + ((c.view_height & 31) ? 3 : 2);
    std::vector<std::array<std::uint8_t, 2>> cells(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh), {0, 0});
    const int x0 = mod32(c.scroll_x) < 16 ? div32(c.scroll_x) - 1 : div32(c.scroll_x);
    const int z0 = mod32(c.scroll_y) < 16 ? div32(c.scroll_y) - 1 : div32(c.scroll_y);
    const bool fog = !f.unseen.empty();
    auto mark = [&](int which, int gx, int gz) {
        // The block's corner bit in the four cells around it.
        const std::pair<int, int> at[4] = {{gx, gz}, {gx - 1, gz}, {gx, gz - 1}, {gx - 1, gz - 1}};
        const std::uint8_t bit[4] = {1, 2, 4, 8};
        for (int k = 0; k < 4; ++k)
            if (static_cast<unsigned>(at[k].first) < static_cast<unsigned>(gw) && static_cast<unsigned>(at[k].second) < static_cast<unsigned>(gh))
                cells[static_cast<std::size_t>(at[k].second * gw + at[k].first)][static_cast<std::size_t>(which)] |= bit[k];
    };
    for (int bz = z0; bz < z0 + gh; ++bz)
        for (int bx = x0; bx < x0 + gw; ++bx) {
            if (static_cast<unsigned>(bx) >= static_cast<unsigned>(f.blocks_x) || static_cast<unsigned>(bz) >= static_cast<unsigned>(f.blocks_z))
                continue;
            const std::size_t at = static_cast<std::size_t>(bz) * static_cast<std::size_t>(f.blocks_x) + static_cast<std::size_t>(bx);
            if (fog && at < f.unseen.size() && f.unseen[at]) mark(1, bx - x0, bz - z0);
            if (at < f.unexplored.size() && f.unexplored[at]) mark(0, bx - x0, bz - z0);
        }
    // Map edges: the corners outside the map copy the ones inside.
    auto fix = [&](int gx, int gz, std::uint8_t if_bit, std::uint8_t set_bit) {
        for (int which = 0; which < 2; ++which) {
            if (which == 1 && !fog) continue;
            auto& v = cells[static_cast<std::size_t>(gz * gw + gx)][static_cast<std::size_t>(which)];
            if (v & if_bit) v |= set_bit;
        }
    };
    if (z0 < 0)
        for (int gx = 0; gx < gw; ++gx) {
            fix(gx, 0, 4, 1);
            fix(gx, 0, 8, 2);
        }
    if (f.blocks_z < z0 + gh && gh >= 2)
        for (int gx = 0; gx < gw; ++gx) {
            fix(gx, gh - 2, 1, 4);
            fix(gx, gh - 2, 2, 8);
        }
    if (x0 < 0)
        for (int gz = 0; gz < gh; ++gz) {
            fix(0, gz, 8, 4);
            fix(0, gz, 2, 1);
        }
    if (f.blocks_x < x0 + gw && gw >= 2)
        for (int gz = 0; gz < gh; ++gz) {
            fix(gw - 2, gz, 4, 8);
            fix(gw - 2, gz, 1, 2);
        }
    // Drawing.
    const ClipRect clip = c.view_clip();
    const int mx = mod32(c.scroll_x), mz = mod32(c.scroll_y);
    const int ox = c.origin_x + (mx > 15 ? 16 : -16) - mx;
    const int oz = c.origin_y + (mz > 15 ? 16 : -16) - mz;
    const int phase = div32(c.scroll_x + 16) + div32(c.scroll_y + 16);
    const int parity = (c.scroll_x + c.scroll_y) & 1;
    out.remap = f.gray_table;
    const Sprite8* black_cell = nullptr;
    const Sprite8* grey_cell = nullptr;
    for (int gz = 0; gz < gh; ++gz)
        for (int gx = 0; gx < gw; ++gx) {
            const auto& v = cells[static_cast<std::size_t>(gz * gw + gx)];
            const int x = ox + gx * 32, y = oz + gz * 32;
            const std::size_t seq = static_cast<std::size_t>((gx + gz + phase) & 3);
            if (v[0] == 0xF) {
                if (!black_cell) {
                    Sprite8 b;
                    b.width = b.height = 32;
                    b.has_key = false;
                    b.pixels.assign(32 * 32, f.fill);
                    black_cell = out.keep(std::move(b));
                }
                out.blit(black_cell, x, y, clip, false);
                continue;
            }
            if (v[1] != 0 && s.dithered_fog) {
                if (v[1] == 0xF) dither_cell(out, clip, x, y, parity, c.screen_width, c.screen_height);
                else if (f.gray[seq] && static_cast<std::size_t>(v[1] - 1) < f.gray[seq]->size())
                    dither_frame(out, clip, (*f.gray[seq])[static_cast<std::size_t>(v[1] - 1)], x, y, parity);
            } else if (v[1] != 0 && f.gray_table) {
                if (v[1] == 0xF) {
                    if (!grey_cell) {
                        Sprite8 g;
                        g.width = g.height = 32;
                        g.key = 1;
                        g.pixels.assign(32 * 32, 0);
                        grey_cell = out.keep(std::move(g));
                    }
                    out.remap_under(grey_cell, x, y, clip, false);
                } else if (f.gray[seq] && static_cast<std::size_t>(v[1] - 1) < f.gray[seq]->size()) {
                    out.remap_under(&(*f.gray[seq])[static_cast<std::size_t>(v[1] - 1)], x, y, clip, true);
                }
            }
            if (v[0] != 0 && f.black[seq] && static_cast<std::size_t>(v[0] - 1) < f.black[seq]->size())
                out.blit(&(*f.black[seq])[static_cast<std::size_t>(v[0] - 1)], x, y, clip, true);
        }
}

} // namespace

std::vector<Sprite8> glow_frames(int which, std::uint32_t seed) {
    static const int kCount[3] = {12, 15, 15}, kFirst[3] = {64, 128, 200}, kLast[3] = {0, 16, 32};
    std::uint32_t r = seed;
    auto rand15 = [&]() {
        r = r * 214013u + 2531011u;
        return static_cast<int>((r >> 16) & 0x7FFF);
    };
    std::vector<Sprite8> out;
    const int w = which < 0 || which > 2 ? 0 : which;
    const int n = kCount[w];
    const int step = w == 0 ? -4 : (kLast[w] - kFirst[w]) / n;
    for (int f = 0, size = kFirst[w]; f < n; ++f, size += step) {
        Sprite8 s;
        s.width = s.height = size;
        const int half = size / 2;
        s.x_offset = s.y_offset = half;
        s.key = 0xFF;
        s.pixels.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0xFF);
        const double c = half;
        for (int y = 0; y < size; ++y) {
            const double dy = c - y;
            const double dy2 = dy * dy * 1.33;
            for (int x = 0; x < size; ++x) {
                const double dx = c - x;
                const int noise = static_cast<int>((static_cast<long long>(rand15()) * 10) / 0x8000);
                const int v = static_cast<int>((std::sqrt(dx * dx + dy2) + noise) / c * 32.0);
                const std::uint8_t level = static_cast<std::uint8_t>(0x20 - v);
                std::uint8_t px = 0xFF;
                if (level < 0x20) px = static_cast<std::uint8_t>(level + 0x4F);
                else if (level < 0x22) px = 0x6E;
                s.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)] = px;
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

DrawList build_draw_list(const BattleScene& s) {
    DrawList out;
    const Camera& c = s.camera;
    out.width = c.screen_width;
    out.height = c.screen_height;
    out.clear_index = s.clear_index;

    draw_terrain(s, out);
    if (s.after_terrain) s.after_terrain(out);

    // Unit rows: 16 world units of z each, starting 16 rows above the view.
    const int row_count = c.view_height / 16 + 32;
    std::vector<std::vector<const UnitInstance*>> rows(static_cast<std::size_t>(row_count));
    for (const UnitInstance& ui : s.units) {
        if (!ui.unit) continue;
        const int r = trunc_div16(hi16(ui.unit->pos[2]) - c.scroll_y) + 16;
        if (r >= 0 && r < row_count) rows[static_cast<std::size_t>(r)].push_back(&ui);
    }
    // Feature cells covered by the rows (and 10 cells either side of the view).
    std::unordered_map<long long, const FeatureInstance*> by_cell;
    for (const FeatureInstance& f : s.features) by_cell[(static_cast<long long>(f.cell_z) << 32) | static_cast<std::uint32_t>(f.cell_x)] = &f;
    const int map_w = s.heights ? s.heights->width() : 1 << 20, map_h = s.heights ? s.heights->height() : 1 << 20;
    int row0 = trunc_div16(c.scroll_y) - 16, rows_n = row_count, skip_rows = 0;
    if (row0 < 0) {
        skip_rows = -row0;
        rows_n += row0;
        row0 = 0;
    }
    if (rows_n + row0 > map_h - 1) rows_n = map_h - row0 - 1;
    int col0 = trunc_div16(c.scroll_x) - 10, cols_n = c.view_width / 16 + 20;
    if (col0 < 0) {
        cols_n += col0;
        col0 = 0;
    }
    if (cols_n + col0 > map_w - 1) cols_n = map_w - col0 - 1;
    auto feature_at = [&](int x, int z) -> const FeatureInstance* {
        auto it = by_cell.find((static_cast<long long>(z) << 32) | static_cast<std::uint32_t>(x));
        return it == by_cell.end() ? nullptr : it->second;
    };
    // Low features first.
    for (int r = 0; r < rows_n; ++r)
        for (int k = 0; k < cols_n; ++k)
            if (const FeatureInstance* f = feature_at(col0 + k, row0 + r))
                if (f->height < 10) draw_feature(s, *f, out);
    // Then, row by row: ground units, then the tall features of that row.
    for (int r = 0; r < rows_n; ++r) {
        const auto& row = rows[static_cast<std::size_t>(r + skip_rows)];
        for (const UnitInstance* ui : row)
            if (ui->ground) draw_unit(s, *ui, out);
        for (int k = 0; k < cols_n; ++k)
            if (const FeatureInstance* f = feature_at(col0 + k, row0 + r))
                if (f->height >= 10) draw_feature(s, *f, out);
    }
    // Nano lathe particles (the effect list 6, the original's 0x473A00): 2 x 2 squares in their
    // colour with the top-left at the particle's screen point.
    for (const NanoDotInstance& d : s.nano) {
        const int x = screen_x(c, d.pos[0]), y = screen_y(c, d.pos[1], d.pos[2]);
        Sprite8 dot;
        dot.width = dot.height = 2;
        dot.key = static_cast<std::uint8_t>(d.colour ^ 1);
        dot.pixels.assign(4, d.colour);
        out.blit(out.keep(std::move(dot)), x, y, c.view_clip(), false);
    }
    {
        std::uint32_t rand_state = s.crt_rand_state;
        for (const ProjectileInstance& p : s.projectiles) {
            if (p.render_type < 0) draw_projectile(s, p, out);
            else if (!draw_projectile_typed(s, p, out, rand_state)) break;
        }
        if (s.crt_rand_after) *s.crt_rand_after = rand_state;
    }
    // World effects (the original's 0x420B00): debris pieces, then every effect's glow, then
    // every effect's shard and art; effects whose point is outside the view are skipped.
    for (const DebrisInstance& d : s.debris) draw_debris(s, d, out);
    const ClipRect view = c.view_clip();
    auto in_view = [&](const EffectInstance& e, int& x, int& y) {
        x = screen_x(c, e.pos[0]);
        y = screen_y(c, e.pos[1], e.pos[2]);
        return x >= view.x0 && x <= view.x1 && y >= view.y0 && y <= view.y1;
    };
    out.light = s.light;
    int ex = 0, ey = 0;
    if (s.light)
        for (const EffectInstance& e : s.effects)
            if (e.glow && in_view(e, ex, ey)) out.lighten(e.glow, ex, ey, view, true);
    for (const EffectInstance& e : s.effects) {
        if (!in_view(e, ex, ey)) continue;
        if (e.shard) draw_shard(s, e, out);
        if (e.art) out.blit(e.art, ex, ey, view, true);
    }
    for (const auto& row : rows)
        for (const UnitInstance* ui : row)
            if (!ui->ground) draw_unit(s, *ui, out);
    if (s.before_fog) s.before_fog(out);
    if (s.fog) draw_fog(s, out);
    return out;
}

} // namespace render
