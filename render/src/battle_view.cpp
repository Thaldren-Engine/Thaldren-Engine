#include "render/battle_view.h"

#include <algorithm>
#include <cmath>

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

void draw_feature(const BattleScene& s, const FeatureInstance& f, DrawList& out) {
    const Camera& c = s.camera;
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
    if (f.shadow && s.feature_shadows) {
        if (f.shadow_blended) out.blend(f.shadow, x, y, clip, true);
        else out.blit(f.shadow, x, y, clip, true);
    }
    if (f.image) {
        if (f.image_blended) out.blend(f.image, x, y, clip, true);
        else out.blit(f.image, x, y, clip, true);
    }
}

void draw_unit(const BattleScene& s, const UnitInstance& ui, DrawList& out) {
    const sim::Unit& u = *ui.unit;
    if (!ui.model || u.carrier) return;  // carried units are drawn by their carrier (not modelled yet)
    const Camera& c = s.camera;
    const ClipRect clip = c.view_clip();
    const sim::PoseState pose = pose_for_drawing(u.pose, u.pitch, u.heading, u.bank);
    Sprite8 sprite = rasterize_model_sprite(*ui.model, pose, ui.params);
    const int ux = screen_x(c, u.pos[0]);
    const int uy = screen_y(c, u.pos[1], u.pos[2]);
    if (ui.shadow && s.unit_shadows) {
        // Shadow: the sprite's shape in colour 0, mixed with the frame, 5 pixels
        // right and placed by the ground height instead of the unit's height.
        Sprite8 sh = sprite;
        for (auto& p : sh.pixels)
            if (p != sh.key) p = 0;
        const int sy = hi16(u.pos[2] - (c.scroll_y << 16)) - (ui.ground_height >> 1) + c.origin_y;
        out.blend(out.keep(std::move(sh)), ux + 5, sy, clip, false);
    }
    if (ui.translucent)
        out.blend(out.keep(std::move(sprite)), ux, uy, clip, false);
    else
        out.blit(out.keep(std::move(sprite)), ux, uy, clip, false);

    // Pieces that are not cached are drawn straight onto the frame each time.
    bool any = false;
    for (const auto& pc : pose.pieces)
        if ((pc.flags & sim::pose_flags::kVisible) && !(pc.flags & sim::pose_flags::kCached)) any = true;
    if (!any) return;
    const std::int32_t vx = u.pos[0] - (c.scroll_x << 16), vz = u.pos[2] - (c.scroll_y << 16);
    Model solid = *ui.model;
    for (auto& p : solid.pieces) p = solid_piece(p);
    emit_direct(c, clip, out, [&](RasterTarget& t, bool mask) {
        draw_uncached_pieces(t, mask ? solid : *ui.model, pose, vx, u.pos[1], vz, c.origin_x, c.origin_y, ui.params);
    });
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

// The shard of a shattered quad: a thin box of 8 points turned by its spin angles, faces in
// the quad's colour or texture (approximation of the original's 0x46BAE0, not yet specified).
void draw_shard(const BattleScene& s, const EffectInstance& e, DrawList& out) {
    static const std::uint16_t kFaces[6][4] = {{0, 1, 2, 3}, {2, 1, 6, 5}, {0, 3, 4, 7}, {1, 0, 7, 6}, {3, 2, 5, 4}, {4, 5, 6, 7}};
    if (!e.look) return;
    ModelPiece piece;
    for (const auto& f : kFaces) {
        ModelPrimitive prim = *e.look;
        prim.indices.assign(f, f + 4);
        piece.primitives.push_back(std::move(prim));
    }
    Model m;
    m.pieces.push_back(std::move(piece));
    sim::PoseModel pm;
    pm.nodes.emplace_back();
    pm.root = 0;
    const std::int16_t angles[3] = {e.angle[2], e.angle[1], e.angle[0]};
    for (const auto& p : e.points) {
        std::int32_t r[3];
        sim::x87::rotate_point_3d(p.data(), r, angles);
        pm.nodes[0].vertices.push_back({r[0], r[1], r[2]});
    }
    draw_model_at(s, m, pm, e.pos, 0, 0, 0, out);
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
            if (v[1] != 0 && f.gray_table) {
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
    for (const ProjectileInstance& p : s.projectiles) draw_projectile(s, p, out);
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
    if (s.fog) draw_fog(s, out);
    return out;
}

} // namespace render
