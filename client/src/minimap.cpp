// The minimap: the map picture halved into a 126 x 126 square, the viewed player's fog picture
// of it, the radar marks drawn over a copy of that every step, and the view's rectangle
// (spec: docs/RE_MEMORY.md, "Client", "Minimap").

#include <algorithm>

#include "client/game_client.h"
#include "sim/sight.h"
#include "sim/trig.h"
#include "sim/unit_tick.h"

namespace client {
namespace {

using i16 = std::int16_t;
using u32 = std::uint32_t;

constexpr u32 kSelected = sim::unit_tick_flags::kSelected;

i16 whole(std::int32_t v) { return static_cast<i16>(static_cast<u32>(v) >> 16); }

void put(render::Image8& o, int x, int y, std::uint8_t c) {
    if (x >= 0 && y >= 0 && x < o.width && y < o.height) o.row(y)[x] = c;
}

// A circle as 32 straight segments (every 1/32 turn), from the point at angle 0; with
// `dashed` only every other segment, starting with the second when `phase` is 0.
void ring(render::Image8& o, int cx, int cy, int r, std::uint8_t colour, bool dashed, int phase) {
    const render::ClipRect clip = o.bounds();
    int px = cx + r, py = cy;
    int count = phase;
    for (int a = 0x800; a <= 0x10000; a += 0x800) {
        const auto angle = static_cast<sim::Angle>(a & 0xFFFF);
        const int x = sim::trig::mul_cos(angle, r) + cx;
        const int y = sim::trig::mul_sin(angle, r) + cy;
        if (!dashed || (count & 1)) render::draw_line(o, clip, px, py, x, y, colour);
        ++count;
        px = x;
        py = y;
    }
}

}  // namespace

MinimapBox minimap_box(int sw, int sh) {
    MinimapBox b;
    if (sw <= 0 || sh <= 0) return b;
    if (sw < sh) {
        b.w = sw * 126 / sh;
        b.x = (126 - b.w) / 2;
    } else {
        b.h = sh * 126 / sw;
        b.y = (126 - b.h) / 2;
    }
    return b;
}

int GameClient::scroll_width() const { return b_.map.width * 16 - 32; }
int GameClient::scroll_height() const { return b_.map.height * 16 - 128; }

void GameClient::minimap_build_picture() {
    // Each minimap pixel mixes a 2 x 2 block of the source picture's top-left part: the two
    // pixels of each row through the mixing table, then the two results.
    mm_picture_ = render::Image8(std::max(mm_w_, 0), std::max(mm_h_, 0), 0);
    const render::Sprite8* src = c_.minimap_picture;
    if (!src || src->width <= 0) return;
    auto px = [&](int x, int y) -> std::uint8_t {
        return x < src->width && y < src->height ? src->pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(src->width) + static_cast<std::size_t>(x)] : 0;
    };
    const bool mix = c_.blend.size() >= 0x10000;
    for (int y = 0; y < mm_picture_.height; ++y)
        for (int x = 0; x < mm_picture_.width; ++x) {
            const std::uint8_t a = px(2 * x, 2 * y), b = px(2 * x + 1, 2 * y), c = px(2 * x, 2 * y + 1), d = px(2 * x + 1, 2 * y + 1);
            if (!mix) {
                mm_picture_.row(y)[x] = a;
                continue;
            }
            const std::uint8_t top = c_.blend[static_cast<std::size_t>(a) * 256 + b];
            const std::uint8_t bottom = c_.blend[static_cast<std::size_t>(c) * 256 + d];
            mm_picture_.row(y)[x] = c_.blend[static_cast<std::size_t>(top) * 256 + bottom];
        }
}

void GameClient::minimap_composite() {
    // Per pixel, the 32-pixel block of the whole map it falls in (rows and columns scaled from
    // the map's block counts): unexplored by the viewed player -> interface colour 0; explored
    // but out of its sight -> the picture's pixel greyed; in sight -> the pixel.
    mm_mapped_ = mm_picture_;
    mm_mapped_valid_ = true;
    const int view = viewed_player();
    const int bw = b_.map.width / 2, bh = b_.map.height / 2;
    const std::vector<std::uint8_t>* counts = nullptr;
    if (view >= 0 && static_cast<std::size_t>(view) < b_.sight.players.size()) counts = &b_.sight.players[static_cast<std::size_t>(view)].counts;
    const int w = mm_mapped_.width, h = mm_mapped_.height;
    for (int y = 0; y < h; ++y) {
        const int row = y * bh / h;
        for (int x = 0; x < w; ++x) {
            const std::size_t idx = static_cast<std::size_t>(row * bw + x * bw / w);
            std::uint8_t& p = mm_mapped_.row(y)[x];
            const bool explored = idx < b_.explored.size() && view >= 0 && view < 16 && (b_.explored[idx] & (1u << view));
            if (!explored) p = c_.hud[0];
            else if (!counts || idx >= counts->size() || (*counts)[idx] == 0) p = c_.minimap_grey[p];
        }
    }
}

void GameClient::minimap_redraw() {
    // The fog picture with the marks: the radar dots of the units shown (in their owner's
    // colour; damaged units blink), the hovered unit's ring, the selected units' radar, sonar
    // and jammer reaches, the interceptors' coverage, then the shots the viewed player sees.
    if (!mm_mapped_valid_) minimap_composite();
    mm_final_ = mm_mapped_;
    render::Image8& o = mm_final_;
    mm_dots_.clear();
    const int sw = std::max(1, scroll_width()), sh = std::max(1, scroll_height());
    const int view = viewed_player();
    const bool blink = ((mm_blink_calls_ / 8) & 1) != 0;
    const bool all = opt_.radar_everything || (b_.sight.flags & (sim::sight_flags::kReveal | sim::sight_flags::kFog)) == 0;
    auto colour_of = [&](int slot) -> std::size_t { return slot >= 0 && slot < 10 ? c_.player_colour[static_cast<std::size_t>(slot)] : 0; };
    for (std::size_t i = 1; i < b_.units.size(); ++i) {
        const sim::UnitTickState& t = b_.ticks[i];
        if (t.type_index == 0) continue;
        const sim::Unit& u = b_.units[i];
        if (!all && !(u.flags & (sim::sight_unit_flags::kRadar | sim::sight_unit_flags::kSonar)) && t.owner != view) continue;
        const int x = whole(u.pos[0]) * mm_w_ / sw;
        const int y = (whole(u.pos[2]) - (whole(u.pos[1]) >> 1)) * mm_h_ / sh;
        if ((t.damaged_timer == 0 || blink) && c_.radar_dots && !c_.radar_dots->empty()) {
            const auto& dots = *c_.radar_dots;
            render::blit(o, o.bounds(), dots[std::min(colour_of(t.owner), dots.size() - 1)], x, y);
        }
        if (u.id == hover_ && c_.radar_dot_hover) render::blit(o, o.bounds(), *c_.radar_dot_hover, x, y);
        if ((u.flags & kSelected) && t.type_index < b_.types.size()) {
            const sim::BattleUnitType& bt = b_.types[t.type_index];
            if ((u.activate_flags & 1) || !(bt.mission.abilities & 4u)) {
                const sim::SightType& st = bt.sight;
                if (st.radar) ring(o, x, y, mm_w_ * st.radar / sw, c_.hud[10], false, 0);
                if (st.sonar) ring(o, x, y, mm_w_ * st.sonar / sw, c_.hud[10], false, 0);
                if (st.radar_jam) ring(o, x, y, mm_w_ * st.radar_jam / sw, c_.hud[12], false, 0);
                if (st.sonar_jam) ring(o, x, y, mm_w_ * st.sonar_jam / sw, c_.hud[12], false, 0);
            }
            if (bt.tick.type_flags & (1u << 29)) {
                // Interceptors: their coverage less 512, solid while nothing is stockpiled.
                for (const sim::WeaponSlot& s : u.weapons) {
                    if (!s.type || !(s.type->flags & (1u << 30))) continue;
                    const int r = mm_w_ * (s.type->coverage - 0x200) / sw;
                    ring(o, x, y, r, c_.hud[15], s.stockpile != 0, blink ? 1 : 0);
                }
            }
        }
        mm_dots_.push_back({u.id, mm_x_ + x, mm_y_ + y});
    }
    // Shots: a dot of interface colour 14 (nuclear ones the nuke mark in the owner's colour),
    // where the viewed player sees the ground point under them (its sight with line of sight,
    // else its explored map) or for its own nukes.
    const bool fog = (b_.sight.flags & sim::sight_flags::kFog) != 0;
    const sim::PlayerSight* ps = view >= 0 && static_cast<std::size_t>(view) < b_.sight.players.size() ? &b_.sight.players[static_cast<std::size_t>(view)] : nullptr;
    for (std::int32_t k = 0; k < b_.projectiles.count; ++k) {
        const sim::Projectile& p = b_.projectiles.slots[static_cast<std::size_t>(k)];
        if (!p.weapon) continue;
        const int wz = whole(p.pos[2]) - (whole(p.pos[1]) >> 1);
        const int x = mm_w_ * whole(p.pos[0]) / sw;
        const int y = mm_h_ * wz / sh;
        const u32 bx = static_cast<u32>(whole(p.pos[0]) >> 5), by = static_cast<u32>(wz >> 5);
        bool seen = false;
        if (ps && bx < static_cast<u32>(ps->width) && by < static_cast<u32>(ps->height)) {
            const std::size_t idx = static_cast<std::size_t>(by) * static_cast<std::size_t>(ps->width) + bx;
            if (fog) seen = idx < ps->counts.size() && ps->counts[idx] != 0;
            else seen = idx < b_.explored.size() && view < 16 && (b_.explored[idx] & (1u << view));
        }
        const u32 wf = p.weapon->flags;
        if ((wf & 0x60000000u) == 0) {
            if (wf & 0x40u) continue;
            if (seen || p.owner_slot == view) put(o, x, y, c_.hud[14]);
        } else {
            const bool own = p.owner && p.owner->id < b_.ticks.size() && b_.ticks[p.owner->id].owner == view;
            if ((seen || own) && c_.nuke_dots && !c_.nuke_dots->empty()) {
                const auto& n = *c_.nuke_dots;
                render::blit(o, o.bounds(), n[std::min(colour_of(p.owner_slot), n.size() - 1)], x, y);
            }
        }
    }
}

void GameClient::minimap_player_stage(int player) {
    mm_hooked_ = true;
    if (!prepared_) prepare_drawing();
    if (player == viewed_player()) minimap_redraw();
    if (minimap_probe) minimap_probe(0, player);
}

void GameClient::minimap_sensor_turn(int player) {
    mm_hooked_ = true;
    if (!prepared_) prepare_drawing();
    if (player == viewed_player()) minimap_composite();
    if (minimap_probe) minimap_probe(1, player);
}

void GameClient::minimap_step_end() {
    ++mm_blink_calls_;
    if (minimap_probe) minimap_probe(2, -1);
}

void GameClient::minimap_sight_rebuilt() {
    if (!prepared_) return;  // (the first drawing composes it)
    minimap_composite();
    minimap_redraw();
}

void GameClient::draw_minimap_onto(render::Image8& o) {
    if (!prepared_) prepare_drawing();
    for (int y = 0; y < mm_final_.height; ++y)
        for (int x = 0; x < mm_final_.width; ++x) put(o, mm_x_ + x, mm_y_ + y, mm_final_.row(y)[x]);
    draw_minimap(o);
}

void GameClient::draw_minimap(render::Image8& o) {
    // The view's rectangle: the scroll position and the view's size in whole 16-pixel cells,
    // scaled like the picture, in interface colour 14 over the whole screen.
    const int sw = std::max(1, scroll_width()), sh = std::max(1, scroll_height());
    const int x0 = mm_w_ * scroll_x_ / sw + mm_x_, y0 = mm_h_ * scroll_y_ / sh + mm_y_;
    const int x1 = mm_w_ * ((opt_.width - 128) / 16 * 16) / sw - 1 + x0;
    const int y1 = mm_h_ * ((opt_.height - 64) / 16 * 16) / sh - 1 + y0;
    const render::ClipRect all = o.bounds();
    const std::uint8_t c = c_.hud[14];
    render::draw_line(o, all, x0, y0, x1, y0, c);
    render::draw_line(o, all, x1, y0, x1, y1, c);
    render::draw_line(o, all, x0, y1, x1, y1, c);
    render::draw_line(o, all, x0, y0, x0, y1, c);
}

}  // namespace client
