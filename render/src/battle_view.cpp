#include "render/battle_view.h"

#include <algorithm>
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

void draw_feature(const BattleScene& s, const FeatureInstance& f, DrawList& out) {
    const Camera& c = s.camera;
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
    out.blit(out.keep(std::move(sprite)), ux, uy, clip, false);

    // Pieces that are not cached are drawn straight onto the frame each time.
    bool any = false;
    for (const auto& pc : pose.pieces)
        if ((pc.flags & sim::pose_flags::kVisible) && !(pc.flags & sim::pose_flags::kCached)) any = true;
    if (!any) return;
    // Rasterise into a frame-sized scratch restricted to the view clip, then
    // keep only the written pixels as a sprite (key = an index never written).
    Image8 scratch(c.screen_width, c.screen_height, 0);
    Image8 mask(c.screen_width, c.screen_height, 0);
    RasterTarget t;
    t.pixels = scratch.pixels.data();
    t.pitch = scratch.width;
    t.clip = clip;
    // Draw twice: once for colours, once into a mask with every texel/colour forced to 1.
    const std::int32_t vx = u.pos[0] - (c.scroll_x << 16), vz = u.pos[2] - (c.scroll_y << 16);
    draw_uncached_pieces(t, *ui.model, pose, vx, u.pos[1], vz, c.origin_x, c.origin_y, ui.params);
    {
        Model solid = *ui.model;
        for (auto& p : solid.pieces)
            for (auto& prim : p.primitives) {
                if (!prim.flat && prim.indices.size() != 4) continue;
                if (!prim.flat && !prim.texture) continue;
                prim.flat = true;
                prim.colour = 1;
            }
        RasterTarget m = t;
        m.pixels = mask.pixels.data();
        draw_uncached_pieces(m, solid, pose, vx, u.pos[1], vz, c.origin_x, c.origin_y, ui.params);
    }
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

} // namespace

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
    for (const auto& row : rows)
        for (const UnitInstance* ui : row)
            if (!ui->ground) draw_unit(s, *ui, out);
    return out;
}

} // namespace render
