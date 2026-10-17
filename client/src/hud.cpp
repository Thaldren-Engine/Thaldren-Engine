// The client's frame: the battle view, then the interface art, the minimap and an
// interface layer (bars, text, marks), then the pointer
// (spec: docs/RE_MEMORY.md, "Client").

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

#include "client/canvas.h"
#include "client/game_client.h"
#include "render/battle_view.h"
#include "render/raster.h"
#include "sim/economy.h"
#include "sim/orders.h"
#include "sim/weapon_slot.h"
#include "sim/x87_trig.h"
#include "sim/sight.h"
#include "sim/victory.h"
#include "sim/unit_tick.h"

namespace client {
namespace {

constexpr std::uint32_t kSelected = sim::unit_tick_flags::kSelected;

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}
std::string fmti(const char* f, long long v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

// Nearest-neighbour scaling of a picture into w x h.
render::Sprite8 scaled(const render::Sprite8& src, int w, int h) {
    render::Sprite8 s;
    s.width = w;
    s.height = h;
    s.has_key = false;
    s.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            s.pixels[static_cast<std::size_t>(y * w + x)] = src.at(x * src.width / std::max(w, 1), y * src.height / std::max(h, 1));
    return s;
}

// A player's colour logo stretched into a bar's logo box (the box's corners, rows moved by dy):
// the frame's corners (0, 0), (w, 0), (w, h), (0, h) mapped onto the box's, the texels copied
// (the key too), clipped to the frame.
void draw_logo(render::Image8& o, const std::vector<render::Sprite8>* logos, int colour, const Region& r, int dy) {
    if (!logos || logos->empty()) return;
    const render::Sprite8& s = (*logos)[static_cast<std::size_t>(colour) % logos->size()];
    render::RasterTarget t;
    t.pixels = o.pixels.data();
    t.pitch = o.width;
    t.clip = o.bounds();
    const render::RasterVertex v[4] = {{r.x1, r.y1 + dy, 0}, {r.x2, r.y1 + dy, 0}, {r.x2, r.y2 + dy, 0}, {r.x1, r.y2 + dy, 0}};
    const int uv[4][2] = {{0, 0}, {s.width, 0}, {s.width, s.height}, {0, s.height}};
    render::textured_quad_uv(t, v, uv, s);
}

} // namespace

void GameClient::prepare_drawing() {
    prepared_ = true;
    // The minimap keeps the map's aspect inside a 126 x 126 square, centred.
    const int mw = std::max(1, map_width_px()), mh = std::max(1, map_height_px());
    if (mw < mh) {
        mm_w_ = mw * 126 / mh;
        mm_h_ = 126;
        mm_x_ = (126 - mm_w_) / 2;
        mm_y_ = 0;
    } else {
        mm_w_ = 126;
        mm_h_ = mh * 126 / mw;
        mm_x_ = 0;
        mm_y_ = (126 - mm_h_) / 2;
    }
    if (c_.minimap_picture && c_.minimap_picture->width > 0) minimap_ = scaled(*c_.minimap_picture, mm_w_, mm_h_);
    // The interface layer's transparent index: one no interface drawing uses.
    std::set<int> used;
    for (int k : {0, 4, 10, 12, 14, 15}) used.insert(c_.hud[static_cast<std::size_t>(k)]);
    used.insert(0x53);
    for (const SideLayout& s : c_.sides) {
        used.insert(s.energy_colour);
        used.insert(s.metal_colour);
    }
    auto add_frames = [&](const std::vector<render::Sprite8>* v) {
        if (v)
            for (const auto& f : *v)
                for (std::uint8_t p : f.pixels) used.insert(p);
    };
    add_frames(c_.radar_dots);
    add_frames(c_.team_logos);
    if (c_.radar_dot_hover)
        for (std::uint8_t p : c_.radar_dot_hover->pixels) used.insert(p);
    for (int k = 255; k >= 0; --k)
        if (!used.count(k)) {
            overlay_key_ = static_cast<std::uint8_t>(k);
            break;
        }
}

render::DrawList GameClient::draw() {
    if (!prepared_) prepare_drawing();
    const int w = opt_.width, h = opt_.height;
    render::BattleScene scene;
    scene.camera.screen_width = w;
    scene.camera.screen_height = h;
    scene.camera.view_width = w - 128;
    scene.camera.view_height = h - 64;
    scene.camera.scroll_x = scroll_x_;
    scene.camera.scroll_y = scroll_y_;
    if (c_.fill_scene) c_.fill_scene(scene, opt_.player, frame_counter_);
    if (!opt_.fog) scene.fog = nullptr;
    add_selection_outlines(scene);
    // The units drawn this frame (picking, double clicks, notices).
    on_screen_.clear();
    for (const render::UnitInstance& ui : scene.units) {
        if (!ui.unit) continue;
        const int sx = render::screen_x(scene.camera, ui.unit->pos[0]);
        const int sy = render::screen_y(scene.camera, ui.unit->pos[1], ui.unit->pos[2]);
        if (sx > -64 && sx < w + 64 && sy > -64 && sy < h + 64) on_screen_.push_back({ui.unit->id, sx, sy});
    }
    render::DrawList list = render::build_draw_list(scene);
    const render::ClipRect screen{0, 0, w - 1, h - 1};

    // Interface art: the side panel, the top and bottom bars (repeated across wider screens).
    if (side_) {
        if (side_->panel_side) list.blit(side_->panel_side, side_->panel_side->x_offset, side_->panel_side->y_offset, screen, true);
        // The top bar: its own piece first, then (wider screens) the bottom bar's piece repeated.
        for (int x = 129; side_->panel_top && x < w;) {
            const render::Sprite8* s = x == 129 || !side_->panel_bottom ? side_->panel_top : side_->panel_bottom;
            list.blit(s, x + s->x_offset, s->y_offset, screen, true);
            x += std::max(1, s->width);
        }
        for (int x = 129; side_->panel_bottom && x < w; x += std::max(1, side_->panel_bottom->width))
            list.blit(side_->panel_bottom, x + side_->panel_bottom->x_offset, h - 32 + side_->panel_bottom->y_offset, screen, true);
    }
    // The panel and its buttons.
    if (const Panel* p = current_panel()) {
        if (p->background) list.blit(p->background, p->x + p->background->x_offset, p->y + p->background->y_offset, screen, true);
        for (const Button& btn : p->buttons) {
            const bool held = held_button_ == &btn && left_down_ && mouse_x_ >= btn.x && mouse_x_ < btn.x + btn.w &&
                              mouse_y_ >= btn.y && mouse_y_ < btn.y + btn.h;
            const bool selected_mode = mode_ != Action::None && mode_ == btn.action;
            const bool tab_on = (btn.action == Action::OrdersTab && (!focus_ || !(b_.units[focus_].flags & 0x400000))) ||
                                (btn.action == Action::BuildTab && focus_ && (b_.units[focus_].flags & 0x400000));
            const int f = button_frame(btn, held || selected_mode || tab_on, !button_enabled(btn), button_stage(btn));
            if (f >= 0) {
                const render::Sprite8& s = (*btn.frames)[static_cast<std::size_t>(f)];
                list.blit(&s, btn.x + s.x_offset, btn.y + s.y_offset, screen, true);
            }
        }
    }
    if (!minimap_.pixels.empty()) list.blit(&minimap_, mm_x_, mm_y_, screen, true);

    // The interface layer.
    render::Sprite8 layer;
    {
        render::Image8 o(w, h, overlay_key_);
        std::vector<std::uint8_t> unused;
        draw_overlay(o, unused);
        layer.width = w;
        layer.height = h;
        layer.key = overlay_key_;
        layer.has_key = true;
        layer.pixels = std::move(o.pixels);
    }
    list.blit(list.keep(std::move(layer)), 0, 0, screen, false);

    // The pointer (animated looks step every 2 frames of 30 per second, by the frame counter).
    const CursorKind ck = cursor_kind();
    const auto* look = c_.cursors.looks[static_cast<std::size_t>(ck)];
    if (!look || look->empty()) look = c_.cursors.looks[static_cast<std::size_t>(CursorKind::Normal)];
    if (look && !look->empty() && !pointer_hidden_) {
        const render::Sprite8& s = (*look)[(frame_counter_ / 2) % look->size()];
        list.blit(&s, mouse_x_, mouse_y_, screen, true);
    }
    return list;
}

void GameClient::add_selection_outlines(render::BattleScene& scene) const {
    // Every selected unit (flag 0x10, whoever owns it) of the view gets the outline of its
    // model's box (the root piece's bounds, see the importer) at the box's lowest y: the four
    // corners turned by the unit's three angles, placed like the pick test's corners, joined in
    // interface colour 10. The original draws it in the unit's turn, just before the unit.
    if (!opt_.selection_boxes) return;
    for (render::UnitInstance& ui : scene.units) {
        if (!ui.unit) continue;
        const sim::Unit& u = *ui.unit;
        if (!(u.flags & kSelected) || u.id >= b_.ticks.size()) continue;
        const std::uint16_t t = b_.ticks[u.id].type_index;
        if (t == 0 || t >= c_.types.size()) continue;
        const UnitTypeLook& look = c_.types[t];
        const auto& mn = look.box_min;
        const auto& mx = look.box_max;
        const std::int32_t corners[4][3] = {{mn[0], mn[1], mn[2]}, {mx[0], mn[1], mn[2]}, {mx[0], mn[1], mx[2]}, {mn[0], mn[1], mx[2]}};
        const std::int16_t angles[3] = {u.pitch, static_cast<std::int16_t>(u.heading), u.bank};
        const std::uint32_t ox = static_cast<std::uint32_t>(u.pos[0]) - static_cast<std::uint32_t>(scene.camera.scroll_x) * 0x10000u;
        const std::uint32_t oz = static_cast<std::uint32_t>(u.pos[2]) - static_cast<std::uint32_t>(scene.camera.scroll_y) * 0x10000u;
        for (int k = 0; k < 4; ++k) {
            std::int32_t r[3];
            sim::x87::rotate_point_3d(corners[k], r, angles);
            ui.outline_xy[static_cast<std::size_t>(2 * k)] = static_cast<std::int16_t>((static_cast<std::uint32_t>(r[0]) + ox) >> 16) + 128;
            ui.outline_xy[static_cast<std::size_t>(2 * k + 1)] =
                static_cast<std::int16_t>((oz - static_cast<std::uint32_t>(r[2])) >> 16) -
                (static_cast<std::int16_t>((static_cast<std::uint32_t>(r[1]) + static_cast<std::uint32_t>(u.pos[1])) >> 16) >> 1) + 32;
        }
        ui.outline = true;
        ui.outline_colour = c_.hud[10];
    }
}

void GameClient::draw_overlay(render::Image8& o, std::vector<std::uint8_t>&) {
    draw_unit_marks(o);
    draw_console(o);
    draw_top_bar(o);
    draw_bottom_bar(o);
    draw_minimap(o);
    // Build counts on factory buttons.
    if (const Panel* p = current_panel())
        if (focus_ && side_ && side_->font)
            for (const Button& btn : p->buttons) {
                if (btn.action != Action::BuildUnit || !btn.unit_type) continue;
                const std::int32_t n = build_count(b_.units[focus_], btn.unit_type);
                if (n > 0)
                    draw_text(o, o.bounds(), *side_->font, "+" + std::to_string(n), btn.x + 4, btn.y + 4 + side_->font->raise,
                              c_.hud[15]);
            }
    // Paused sign; the victory and defeat signs once the battle is decided (end flags).
    const int cx = (opt_.width + 128) / 2, cy = opt_.height / 2;
    if (budget_.paused && c_.paused_sign) render::blit(o, o.bounds(), *c_.paused_sign, cx, cy);
    if ((b_.end_game.flags & sim::end_game_flags::kWonMovie) && c_.victory_sign) render::blit(o, o.bounds(), *c_.victory_sign, cx, cy);
    if ((b_.end_game.flags & sim::end_game_flags::kLost) && c_.defeat_sign) render::blit(o, o.bounds(), *c_.defeat_sign, cx, cy);
    draw_clock_and_units(o);
    draw_chat(o);
}

void GameClient::draw_clock_and_units(render::Image8& o) {
    // The game clock (option): "Game Time: hh:mm:ss" of game steps at 130, above the bottom bar.
    if (!opt_.clock || !side_ || !side_->font) return;
    const Font& f = *side_->font;
    const std::uint32_t s = b_.game_tick() / 30;
    char buf[64];
    std::snprintf(buf, sizeof buf, "Game Time: %02u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    draw_text(o, o.bounds(), f, buf, 130, opt_.height - 34 - f.height, c_.hud[15]);
}

void GameClient::draw_chat(render::Image8& o) {
    // The chat line being typed, in the top bar's place while it is open.
    if (!chat_ || !side_ || !side_->font) return;
    const Font& f = *side_->font;
    fill_rect(o, o.bounds(), 128, 0, opt_.width - 1, 31, c_.hud[0]);
    outline_rect(o, o.bounds(), 140, 6, std::min(opt_.width - 1, 500), 25, c_.hud[15]);
    draw_text(o, o.bounds(), f, "> " + chat_text_ + "_", 144, 9 + f.raise, c_.hud[15], -1, 350);
}

void GameClient::draw_unit_marks(render::Image8& o) {
    const render::ClipRect view = view_rect();
    // The drag box: an outer and an inner rectangle.
    if (dragging_) {
        const int sx0 = (drag_start_[0] >> 16) - scroll_x_ + 128;
        const int sy0 = (drag_start_[2] >> 16) - (drag_start_[1] >> 17) - scroll_y_ + 32;
        outline_rect(o, view, sx0, sy0, mouse_x_, mouse_y_, c_.hud[15]);
        const int x0 = std::min(sx0, mouse_x_) + 1, x1 = std::max(sx0, mouse_x_) - 1;
        const int y0 = std::min(sy0, mouse_y_) + 1, y1 = std::max(sy0, mouse_y_) - 1;
        if (x0 <= x1 && y0 <= y1) outline_rect(o, view, x0, y0, x1, y1, c_.hud[0]);
    }
    // Placement: the footprint at the pointer, in the "fits" or "blocked" colour.
    if (mode_ == Action::BuildUnit && in_view(mouse_x_, mouse_y_) && build_type_ < b_.types.size()) {
        const sim::Vec3i p = world_at(mouse_x_, mouse_y_);
        const int fx = b_.types[build_type_].construction.footprint_x, fz = b_.types[build_type_].construction.footprint_z;
        const int cx = ((p[0] >> 16) >> 4) - fx / 2, cz = ((p[2] >> 16) >> 4) - fz / 2;
        const bool ok = structure_fits(b_, build_type_, cx, cz);
        const int hgt = (p[1] >> 16) / 2;
        const int x0 = cx * 16 - scroll_x_ + 128, z0 = cz * 16 - scroll_y_ + 32 - hgt;
        outline_rect(o, view, x0, z0, x0 + fx * 16 - 1, z0 + fz * 16 - 1, c_.hud[ok ? 10 : 4]);
    }
    for (const UnitScreen& s : on_screen_) {
        const sim::Unit& u = b_.units[s.id];
        const sim::UnitTickState& t = b_.ticks[s.id];
        if (t.type_index == 0 || t.owner != opt_.player) continue;
        // (The selection outline is drawn with the unit itself: add_selection_outlines.)
        // Health bar (option, or units in a group) and the group number.
        if ((opt_.damage_bars || u.group != 0) && u.hit_points > 0 && u.type && u.type->max_damage > 0) {
            const int x = s.x, y = s.y + 10;
            fill_rect(o, view, x - 17, y - 2, x + 17, y + 2, c_.hud[0]);
            const int maxd = static_cast<int>(u.type->max_damage);
            const int len = static_cast<int>((static_cast<unsigned>(u.hit_points) << 5) / static_cast<unsigned>(maxd));
            const int third = maxd / 3;
            const std::uint8_t col = u.hit_points > 2 * third ? c_.hud[10] : u.hit_points > third ? c_.hud[14] : c_.hud[12];
            fill_rect(o, view, x - 16, y - 1, x - 16 + len, y + 1, col);
            if (u.group != 0 && side_ && side_->font)
                draw_text(o, view, *side_->font, std::string(1, static_cast<char>('0' + u.group % 10)), x, y + 4,
                          c_.hud[15]);
        }
    }
}

void GameClient::draw_console(render::Image8& o) {
    if (!side_ || !side_->font) return;
    const int lines = std::min<int>(opt_.text_lines, static_cast<int>(console_.size()));
    int y = 52;
    for (int k = static_cast<int>(console_.size()) - lines; k < static_cast<int>(console_.size()); ++k) {
        const Line& l = console_[static_cast<std::size_t>(k)];
        draw_text(o, view_rect(), *side_->font, l.text, 138, y + side_->font->raise, l.highlight ? c_.hud[10] : c_.hud[15]);
        y += side_->font->height;
    }
}

void GameClient::draw_top_bar(render::Image8& o) {
    if (!side_ || !side_->font) return;
    const Font& f = *side_->font;
    const render::ClipRect all = o.bounds();
    const sim::PlayerSlot& me = b_.players[static_cast<std::size_t>(opt_.player)];
    const sim::PlayerEconomy& ec = b_.player_economy[static_cast<std::size_t>(opt_.player)];
    // The viewed player's colour logo in LOGO (right after the bar's art).
    draw_logo(o, c_.team_logos, c_.player_colour[static_cast<std::size_t>(opt_.player) % c_.player_colour.size()], side_->logo, 0);
    // The stored amounts shown move an eighth of the way to the real value each frame.
    const float stored[2] = {me.energy_stored, me.metal_stored};
    const float cap[2] = {ec.energy_storage, ec.metal_storage};
    for (int k = 0; k < 2; ++k) {
        const int cur = static_cast<int>(shown_[static_cast<std::size_t>(k)]);
        const int tgt = static_cast<int>(stored[k]);
        int d = tgt - cur;
        int step = 0;
        if (d < 0) step = d / 8 == 0 ? -1 : d / 8;
        else if (d > 0) step = d / 8 == 0 ? 1 : d / 8;
        float v = static_cast<float>(cur + step);
        if (cap[k] < v) v = cap[k];
        shown_[static_cast<std::size_t>(k)] = v;
    }
    struct Res {
        const Region *bar, *num, *max, *zero, *produced, *consumed;
        std::uint8_t colour;
        float shown, cap, income, usage;
    };
    const Res res[2] = {
        {&side_->energy_bar, &side_->energy_num, &side_->energy_max, &side_->energy_zero, &side_->energy_produced,
         &side_->energy_consumed, side_->energy_colour, shown_[0], cap[0], ec.energy_income, ec.energy_usage},
        {&side_->metal_bar, &side_->metal_num, &side_->metal_max, &side_->metal_zero, &side_->metal_produced,
         &side_->metal_consumed, side_->metal_colour, shown_[1], cap[1], ec.metal_income, ec.metal_usage},
    };
    for (const Res& r : res) {
        if (r.cap > 0.0f) {
            const int x2 = r.bar->x1 + static_cast<int>((r.bar->x2 - r.bar->x1) * std::clamp(r.shown / r.cap, 0.0f, 1.0f));
            if (r.shown > 0.0f) fill_rect(o, all, r.bar->x1, r.bar->y1, x2, r.bar->y2, r.colour);
        }
        draw_text(o, all, f, fmti("%lld", static_cast<long long>(r.shown)), r.num->x1, r.num->y1, c_.hud[15]);
        draw_text(o, all, f, "0", r.zero->x1, r.zero->y1, c_.hud[15]);
        const std::string mx = fmti("%lld", static_cast<long long>(r.cap));
        draw_text(o, all, f, mx, r.max->x1 - f.text_width(mx), r.max->y1, c_.hud[15]);
        const std::string inc = r.income <= 99999.0f ? fmt("%.1f", r.income) : fmti("%lldK", static_cast<long long>(r.income / 1000));
        draw_text(o, all, f, inc, r.produced->x1, r.produced->y1, c_.hud[10]);
        const std::string use = r.usage >= -99999.0f ? fmt("%.1f", r.usage) : fmti("%lldK", static_cast<long long>(r.usage / 1000));
        draw_text(o, all, f, use, r.consumed->x1, r.consumed->y1, c_.hud[12]);
    }
}

void GameClient::draw_bottom_bar(render::Image8& o) {
    if (!side_ || !side_->font) return;
    const Font& f = *side_->font;
    const render::ClipRect all = o.bounds();
    const int dy = opt_.height - 480;  // the side description's rows are for a 480-row screen
    auto centred = [&](const std::string& s, const Region& r) {
        draw_text(o, all, f, s, r.x1 - f.text_width(s) / 2, r.y1 + dy, 0x53);
    };
    auto bar = [&](const Region& r, int value, int max) {
        // Filled for value / max (value clamped to 0..max) in [10], the rest in [4].
        const int v = std::clamp(value, 0, std::max(max, 0));
        const int x2 = max != 0 ? (r.x2 - r.x1) * v / max + r.x1 : r.x1;
        fill_rect(o, all, r.x1, r.y1 + dy, x2, r.y2 + dy, c_.hud[10]);
        if (x2 != r.x2) fill_rect(o, all, x2 + 1, r.y1 + dy, r.x2, r.y2 + dy, c_.hud[4]);
    };
    auto whole = [](float v) { return static_cast<int>(v); };  // the original's float -> int (truncation)

    // A build button under the pointer comes first: "<name>  M:<metal cost> E:<energy cost>" on
    // the NAME line and the type's description on the DESCRIPTION line (not for a CORBUILD).
    if (const Panel* p = current_panel())
        for (const Button& btn : p->buttons) {
            if (mouse_x_ < btn.x || mouse_x_ >= btn.x + btn.w || mouse_y_ < btn.y || mouse_y_ >= btn.y + btn.h) continue;
            if (btn.action != Action::BuildUnit || !btn.unit_type || btn.unit_type >= c_.types.size()) return;
            const UnitTypeLook& look = c_.types[btn.unit_type];
            std::string un = look.unit_name;
            for (char& ch : un) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (un == "CORBUILD") return;
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s  M:%d E:%d", look.name.c_str(), whole(look.cost_metal), whole(look.cost_energy));
            draw_text(o, all, f, buf, side_->name.x1, side_->name.y1 + dy, 0x53);
            draw_text(o, all, f, look.description, side_->description.x1, side_->description.y1 + dy, 0x53);
            return;
        }

    // Else the unit under the pointer (the pointer's pick of this frame; nothing when the pointer
    // is over no unit), else the feature under it.
    const std::uint16_t id = hover_;
    if (!id || b_.ticks[id].type_index == 0) {
        if (id || hover_feature_ < 0 || static_cast<std::size_t>(hover_feature_) >= b_.features.size()) return;
        // A feature: its description and what reclaiming it yields (" M:%d", " E:%d", each only
        // when not zero), left-aligned on the NAME line; flag 0x200: the description alone;
        // flag 0x400: nothing.
        const std::size_t k = static_cast<std::size_t>(hover_feature_);
        const sim::FeatureTypeInfo& ft = b_.features[k];
        const Content::FeatureLook* fl = k < c_.features.size() ? &c_.features[k] : nullptr;
        const std::uint16_t fflags = fl ? fl->flags : 0;
        if (fflags & 0x400) return;
        std::string n = fl ? fl->name : std::string();
        if (!(fflags & 0x200)) {
            char m[32] = "", e[32] = "";
            if (!(ft.metal == 0.0f || ft.metal != ft.metal)) std::snprintf(m, sizeof m, " M:%d", whole(ft.metal));
            if (!(ft.energy == 0.0f || ft.energy != ft.energy)) std::snprintf(e, sizeof e, " E:%d", whole(ft.energy));
            n += std::string(" ") + m + e;
        }
        draw_text(o, all, f, n, side_->name.x1, side_->name.y1 + dy, 0x53);
        return;
    }
    const sim::Unit& u = b_.units[id];
    const sim::UnitTickState& t = b_.ticks[id];
    const bool mine = t.owner == opt_.player;
    // Units the viewed player does not see are an "Unidentified object".
    if (!mine && b_.settings.fog_of_war && !b_.unit_seen_by(opt_.player, u)) {
        centred("Unidentified object", side_->unit_name);
        return;
    }
    centred(t.type_index < c_.types.size() ? c_.types[t.type_index].name : std::string(), side_->unit_name);
    // Health: own units, and others unless their type hides it (type flag 0x4000).
    auto hides_health = [&](const sim::UnitTickState& tt) { return tt.tick_type && (tt.tick_type->type_flags & 0x4000u); };
    if (u.type && (mine || !hides_health(t))) bar(side_->damage_bar, u.hit_points, static_cast<int>(u.type->max_damage));
    // The owner's colour logo in LOGO2.
    draw_logo(o, c_.team_logos, c_.player_colour[static_cast<std::size_t>(t.owner) % c_.player_colour.size()], side_->logo2, dy);
    if (mine) {
        draw_text(o, all, f, fmt("+%.1f", t.metal_make_prev), side_->unit_metal_make.x1, side_->unit_metal_make.y1 + dy, c_.hud[10]);
        draw_text(o, all, f, fmt("+%.0f", t.energy_make_prev), side_->unit_energy_make.x1, side_->unit_energy_make.y1 + dy,
                  c_.hud[10]);
        draw_text(o, all, f, fmt("-%.1f", t.metal_use_prev), side_->unit_metal_use.x1, side_->unit_metal_use.y1 + dy, c_.hud[12]);
        draw_text(o, all, f, fmt("-%.0f", t.energy_use_prev), side_->unit_energy_use.x1, side_->unit_energy_use.y1 + dy,
                  c_.hud[12]);
        // Kills of armed units (unit flag bit 31): "1 kill", "<n> kills", from 5 "<n> kills - Veteran".
        if (t.kill_count > 0 && (u.flags & 0x80000000u)) {
            const std::string k = std::to_string(t.kill_count) + (t.kill_count == 1 ? " kill" : " kills") + (t.kill_count >= 5 ? " - Veteran" : "");
            draw_text(o, all, f, k, side_->damage_bar.x1, side_->damage_bar.y2 + 2 + dy, c_.hud[15]);
        }
        // The unit's status: what its head order does ("Standby", "Moving", "Nanolathing", ...;
        // the text of mission kind 0 without orders), centred on the status line's x1.
        if (!c_.mission_status.empty()) {
            const std::size_t kind = u.orders ? u.orders->kind : 0u;
            centred(c_.mission_status[kind < c_.mission_status.size() ? kind : 0], side_->mission_text);
        }
    }
    // The second line: a stockpile weapon being built (the first secondary order with flag
    // 0x80000: "Weapon" and its progress, param 2 x 100 / the slot's reload time, own units),
    // else the head order's target (own units; when the viewed player sees it).
    int building = 0;
    for (const sim::MissionOrder* so = u.secondary_orders; so; so = so->next)
        if (so->flags & 0x80000u) {
            const int slot = so->param0;
            const sim::WeaponType* wt = slot >= 0 && slot < 3 ? u.weapons[static_cast<std::size_t>(slot)].type : nullptr;
            if (wt && wt->reload_time) building = static_cast<int>(static_cast<std::uint32_t>(so->param2) * 100u) / static_cast<int>(wt->reload_time);
            break;
        }
    if (building == 0) {
        const sim::Unit* target = mine && u.orders ? u.orders->target.unit : nullptr;
        if (!target || b_.ticks[target->id].type_index == 0 || b_.ticks[target->id].type_index >= c_.types.size()) return;
        const sim::UnitTickState& tt = b_.ticks[target->id];
        if (tt.owner != opt_.player && b_.settings.fog_of_war && !b_.unit_seen_by(opt_.player, *target)) return;
        centred(c_.types[tt.type_index].name, side_->unit_name2);
        if (target->type && (tt.owner == opt_.player || !hides_health(tt)))
            bar(side_->damage_bar2, target->hit_points, static_cast<int>(target->type->max_damage));
    } else if (mine) {
        centred("Weapon", side_->unit_name2);
        bar(side_->damage_bar2, std::min(building, 100), 100);
    }
}

void GameClient::draw_minimap(render::Image8& o) {
    const render::ClipRect mm{mm_x_, mm_y_, mm_x_ + mm_w_ - 1, mm_y_ + mm_h_ - 1};
    const int mw = std::max(1, map_width_px()), mh = std::max(1, map_height_px());
    const bool sensors = b_.settings.fog_of_war || b_.settings.explore;
    mm_dots_.clear();
    // Radar dots: shown units without the line of sight options, units marked by radar or
    // sonar, and own units; in the owner's colour.
    for (std::size_t i = 1; i < b_.units.size(); ++i) {
        const sim::UnitTickState& t = b_.ticks[i];
        if (t.type_index == 0) continue;
        const sim::Unit& u = b_.units[i];
        if (sensors && !(u.flags & (sim::sight_unit_flags::kRadar | sim::sight_unit_flags::kSonar)) && t.owner != opt_.player) continue;
        const int x = static_cast<std::int16_t>(static_cast<std::uint32_t>(u.pos[0]) >> 16) * mm_w_ / mw + mm_x_;
        const int y = (static_cast<std::int16_t>(static_cast<std::uint32_t>(u.pos[2]) >> 16) -
                       (static_cast<std::int16_t>(static_cast<std::uint32_t>(u.pos[1]) >> 16) >> 1)) *
                          mm_h_ / mh +
                      mm_y_;
        mm_dots_.push_back({static_cast<std::uint16_t>(i), x, y});
        if (c_.radar_dots && !c_.radar_dots->empty()) {
            const auto& dots = *c_.radar_dots;
            render::blit(o, mm, dots[static_cast<std::size_t>(t.owner) % dots.size()], x, y);
        } else {
            put_pixel(o, mm, x, y, c_.hud[t.owner == opt_.player ? 10 : 12]);
        }
        if (i == hover_ && c_.radar_dot_hover) render::blit(o, mm, *c_.radar_dot_hover, x, y);
        // Selected units show their radar and sonar reach, and their jammers'.
        if ((u.flags & kSelected) && t.owner == opt_.player && t.type_index < b_.types.size()) {
            const sim::SightType& st = b_.types[t.type_index].sight;
            if (st.radar) circle(o, mm, x, y, mm_w_ * st.radar / mw, c_.hud[10]);
            if (st.sonar) circle(o, mm, x, y, mm_w_ * st.sonar / mw, c_.hud[10]);
            if (st.radar_jam) circle(o, mm, x, y, mm_w_ * st.radar_jam / mw, c_.hud[12]);
            if (st.sonar_jam) circle(o, mm, x, y, mm_w_ * st.sonar_jam / mw, c_.hud[12]);
        }
    }
    // Shots the viewed player sees, as single pixels.
    for (std::int32_t k = 0; k < b_.projectiles.count; ++k) {
        const sim::Projectile& p = b_.projectiles.slots[static_cast<std::size_t>(k)];
        if (p.flags & sim::projectile_flags::kDeleted) continue;
        if (b_.settings.fog_of_war && !b_.point_seen_by(opt_.player, p.pos)) continue;
        const int x = (p.pos[0] >> 16) * mm_w_ / mw + mm_x_;
        const int y = ((p.pos[2] >> 16) - (p.pos[1] >> 17)) * mm_h_ / mh + mm_y_;
        put_pixel(o, mm, x, y, c_.hud[14]);
    }
    // The view's rectangle.
    const int x0 = mm_w_ * scroll_x_ / mw + mm_x_, y0 = mm_h_ * scroll_y_ / mh + mm_y_;
    const int x1 = mm_w_ * (opt_.width - 128) / mw - 1 + x0, y1 = mm_h_ * (opt_.height - 64) / mh - 1 + y0;
    outline_rect(o, o.bounds(), x0, y0, x1, y1, c_.hud[14]);
}

} // namespace client
