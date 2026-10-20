// The queued-order view: while Shift is held the frame shows the orders of some of the
// player's units (build boxes, paths, markers, ranges), after the units and effects and before
// the fog (spec: docs/RE_MEMORY.md, "Client", "Queued orders (Shift)").

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "client/canvas.h"
#include "client/game_client.h"
#include "sim/orders.h"
#include "sim/weapon_slot.h"
#include "sim/trig.h"
#include "sim/unit_tick.h"

namespace client {
namespace {

using i16 = std::int16_t;
using i32 = std::int32_t;
using u32 = std::uint32_t;
using Point = std::array<i32, 3>;

constexpr u32 kSelected = sim::unit_tick_flags::kSelected;
constexpr u32 kAlive = 0x10000000u, kDying = 0x4000u;

// Screen position of a world point (whole parts as 16-bit values; half the height up).
int sx(const render::Camera& c, i32 x) { return static_cast<i16>(static_cast<u32>(x) >> 16) - c.scroll_x + 128; }
int sy(const render::Camera& c, i32 y, i32 z) {
    return static_cast<i16>(static_cast<u32>(z) >> 16) - (static_cast<i16>(static_cast<u32>(y) >> 16) >> 1) - c.scroll_y + 32;
}

// The C runtime's float -> int conversion (truncation; out of range: the integer indefinite value).
i32 trunc32(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return static_cast<i32>(0x80000000u);
    return static_cast<i32>(v);
}

struct Ctx {
    render::DrawList& out;
    const render::Camera& cam;
    render::ClipRect clip;
};

}  // namespace

void GameClient::draw_queued_orders(render::DrawList& out, const render::Camera& camera) {
    // Only while Shift is held (polled when the frame is drawn).
    if (!shift_ && !keys_down_[keys::kShift]) return;
    if (opt_.player < 0 || static_cast<std::size_t>(opt_.player) >= b_.players.size()) return;
    Ctx cx{out, camera, camera.view_clip()};
    const std::uint32_t tick = b_.game_tick();
    const auto& look_of = [&](const sim::Unit& u) -> const UnitTypeLook* {
        const std::uint16_t t = u.id < b_.ticks.size() ? b_.ticks[u.id].type_index : 0;
        return t && t < c_.types.size() ? &c_.types[t] : nullptr;
    };
    // The units it is shown for: the one the view follows, the one whose menu is shown and the
    // one under the pointer show everything (with paths); other selected units everything
    // but the paths; and when one of those three units can build, every other unit of the
    // player shows its build boxes (with paths).
    const std::uint16_t special[3] = {tracked_, focus_, hover_};
    bool builder = false;
    for (std::uint16_t id : special)
        if (id && id < b_.units.size())
            if (const UnitTypeLook* l = look_of(b_.units[id])) builder = builder || l->builds;

    auto line = [&](int x0, int y0, int x1, int y1, std::uint8_t colour) { cx.out.line(x0, y0, x1, y1, colour, cx.clip); };

    // A label (0x4C14F0 from the ring routine): the whole text box inside the view or nothing,
    // in the text colour set last, as a keyed sprite drawn now.
    auto label = [&](const char* text, int x, int y) {
        if (!side_ || !side_->font) return;
        const Font& f = *side_->font;
        const int w = f.text_width(text);
        if (x < cx.clip.x0 || y < cx.clip.y0 || x + w > cx.clip.x1 || y + f.height > cx.clip.y1) return;
        const std::uint8_t colour = last_text_colour();
        const std::uint8_t key = colour == 0 ? 255 : 0;
        render::Image8 img(std::max(1, w), std::max(1, f.height), key);
        draw_text(img, img.bounds(), f, text, 0, 0, colour);
        render::Sprite8 sp;
        sp.width = img.width;
        sp.height = img.height;
        sp.key = key;
        sp.has_key = true;
        sp.pixels = std::move(img.pixels);
        cx.out.blit(cx.out.keep(std::move(sp)), x, y, cx.clip, false);
    };
    // A ring on the ground around a point (0x438EA0): r x 2 pi / 8 segments (whole), each point at
    // the terrain's height or the centre's when that is higher; with a label, the text goes 4 rows
    // under the end of segment 3 x `index` (the last segment's end when there is none).
    auto ring = [&](const Point& centre, int r, std::uint8_t colour, const char* text = nullptr, int index = 0) {
        if (r == 0) return;
        const i32 n = trunc32(static_cast<double>(r) * 6.28318530717958 * 0.125);
        int lx = 0, ly = 0, ex = 0, ey = 0;
        if (n == 0) return;  // the original divides by zero here
        if (n < 0) {
            if (text) label(text, 0, 4);
            return;
        }
        const i32 step = 0x10000 / n;
        const i32 mag = static_cast<i32>(static_cast<u32>(r) << 16);
        const i16 cy = static_cast<i16>(static_cast<u32>(centre[1]) >> 16);
        auto point = [&](i32 a, int& x, int& y) {
            const i32 px = centre[0] + sim::trig::mul_sin(static_cast<sim::Angle>(a), mag);
            const i32 pz = centre[2] + sim::trig::mul_cos(static_cast<sim::Angle>(a), mag);
            const i32 ground = b_.height_map().sample(static_cast<i16>(static_cast<u32>(px) >> 16), static_cast<i16>(static_cast<u32>(pz) >> 16));
            const i16 h = cy > ground ? cy : static_cast<i16>(ground);
            x = sx(camera, px);
            y = static_cast<i16>(static_cast<u32>(pz) >> 16) - (h >> 1) - camera.scroll_y + 32;
        };
        i32 a = 0;
        for (i32 k = 0; k <= n; ++k) {
            int x0, y0, x1, y1;
            point(a, x0, y0);
            a += step;
            point(a, x1, y1);
            line(x0, y0, x1, y1, colour);
            ex = x1;
            ey = y1;
            if (k == index * 3) {
                lx = x1;
                ly = y1;
            }
        }
        if (!text) return;
        if (lx == 0 && ly == 0) {
            lx = ex;
            ly = ey;
        }
        label(text, lx, ly + 4);
    };

    // The marker at an order's target (its kind's pointer look, blended, animated by the game
    // step at half its first frame's rate); `at` becomes the target's position.
    auto marker = [&](const sim::MissionOrder& o, Point& at) {
        const sim::Unit* target = o.target.unit;
        if (!target) {
            at = o.pos;
        } else {
            const int owner = o.unit && o.unit->id < b_.ticks.size() ? b_.ticks[o.unit->id].owner : opt_.player;
            const bool seen = b_.unit_seen_by(owner, *target);
            // Where the target was last seen (the original keeps it in the order itself; the
            // client keeps it here so drawing leaves the simulation alone).
            auto it = std::find_if(order_seen_.begin(), order_seen_.end(),
                                   [&](const OrderSeen& e) { return e.order == &o && e.created == o.created_tick; });
            if (!seen && it != order_seen_.end()) {
                at = {static_cast<i32>(static_cast<u32>(static_cast<std::uint16_t>(it->xz[0])) << 16), target->pos[1],
                      static_cast<i32>(static_cast<u32>(static_cast<std::uint16_t>(it->xz[1])) << 16)};
            } else {
                at = target->pos;
                const std::array<i16, 2> xz = {static_cast<i16>(static_cast<u32>(target->pos[0]) >> 16), static_cast<i16>(static_cast<u32>(target->pos[2]) >> 16)};
                if (it != order_seen_.end()) it->xz = xz;
                else order_seen_.push_back({&o, o.created_tick, xz});
            }
        }
        const std::uint8_t m = o.kind < c_.mission_marker.size() ? c_.mission_marker[o.kind] : 0;
        if (!m) return;
        if (opt_.show_ranges && (m == 1 || m == 2) && o.unit) {
            // +showranges at an attack's target: each weapon (slot flag 2) with an area of effect
            // and a coverage, then the type's attack run length, in colour 4 / 12 by the step.
            const std::uint8_t colour = (tick & 1) ? c_.hud[4] : c_.hud[12];
            char text[64];
            for (int k = 0; k < 3; ++k) {
                const sim::WeaponSlot& ws = o.unit->weapons[static_cast<std::size_t>(k)];
                if (!(ws.flags & 2) || !ws.type) continue;
                if (ws.type->area_of_effect != 0) {
                    std::snprintf(text, sizeof text, "weapon %d - area of effect", k);
                    ring(at, ws.type->area_of_effect, colour, text, 0);
                }
                if (ws.type->coverage != 0) {
                    std::snprintf(text, sizeof text, "weapon %d - coverage", k);
                    ring(at, ws.type->coverage, colour, text, 1);
                }
            }
            if (const UnitTypeLook* l = look_of(*o.unit))
                if (l->attack_run_length != 0) ring(at, l->attack_run_length, colour, "attack length", 2);
        }
        if (m >= c_.cursors.by_number.size()) return;
        const auto* seq = c_.cursors.by_number[m];
        const FrameTiming& t = c_.cursors.by_number_timing[m];
        if (!seq || seq->empty() || t.durations.empty() || t.durations[0] == 0) return;
        const std::size_t f = static_cast<std::size_t>((static_cast<std::uint64_t>(tick) / (static_cast<std::uint64_t>(t.durations[0]) << 1)) % seq->size());
        cx.out.blend(&(*seq)[f], sx(camera, at[0]), sy(camera, at[1], at[2]), cx.clip, true);
    };

    // The path from the previous point to the order's target: the marker, then (when asked)
    // dots of the path look every 48 world units, crawling along with the order's age.
    auto path = [&](const sim::MissionOrder& o, Point& at, bool dots) {
        const Point from = at;
        marker(o, at);
        if (!dots) return;
        const i32 age = static_cast<i32>(tick - o.created_tick);
        const i32 dx = static_cast<i32>(static_cast<u32>(at[0]) - static_cast<u32>(from[0]));
        const i32 dy = static_cast<i32>(static_cast<u32>(at[1]) - static_cast<u32>(from[1]));
        const i32 dz = static_cast<i32>(static_cast<u32>(at[2]) - static_cast<u32>(from[2]));
        const double sq = static_cast<double>(dx) * dx + static_cast<double>(dy) * dy + static_cast<double>(dz) * dz;
        const i32 dist = trunc32(std::sqrt(sq));
        if (dist < 0x10000) return;
        const auto* seq = c_.cursors.by_number[21];
        const FrameTiming& t = c_.cursors.by_number_timing[21];
        if (!seq || seq->empty()) return;
        const i32 frames = static_cast<i32>(seq->size());
        i32 off = ((age % 30) * 0x300000) / 30;
        const i32 delay = t.durations.empty() || t.durations[0] < 1 ? 1 : t.durations[0];
        i32 f = (age / delay) % frames;
        while (off < dist) {
            const std::int64_t s = (static_cast<std::int64_t>(off) << 16) / dist;
            const std::int64_t frac = static_cast<i32>(s);
            const i32 px = static_cast<i32>(static_cast<u32>((static_cast<std::int64_t>(dx) * frac) >> 16) + static_cast<u32>(from[0]));
            const i32 py = static_cast<i32>(static_cast<u32>((static_cast<std::int64_t>(dy) * frac) >> 16) + static_cast<u32>(from[1]));
            const i32 pz = static_cast<i32>(static_cast<u32>((static_cast<std::int64_t>(dz) * frac) >> 16) + static_cast<u32>(from[2]));
            if (f >= 0 && f < frames) cx.out.blit(&(*seq)[static_cast<std::size_t>(f)], sx(camera, px), sy(camera, py, pz), cx.clip, true);
            f = (f + 1) % frames;
            off += 0x300000;
        }
    };

    // The box of a structure being built (its type's model bounds around the order's position),
    // two outlines whose sides close in from the edges over the order's first 10 steps; in
    // interface colours 3 and 10 for a selected unit's order, else 1 and 9. `at` becomes the
    // order's position.
    auto box = [&](const sim::MissionOrder& o, Point& at) {
        const std::uint16_t type = static_cast<std::uint16_t>(o.param0);
        if (type == 0 || type >= c_.types.size()) return;
        const UnitTypeLook& l = c_.types[type];
        const auto u16 = [](u32 v) { return static_cast<i16>(v >> 16); };
        const i16 x0 = u16(static_cast<u32>(l.pick_min[0]) + static_cast<u32>(o.pos[0]));
        const i16 y0 = u16(static_cast<u32>(o.pos[1]) + static_cast<u32>(l.pick_min[1]));
        const i16 z0 = u16(static_cast<u32>(l.pick_min[2]) + static_cast<u32>(o.pos[2]));
        const i16 x1 = u16(static_cast<u32>(o.pos[0]) + static_cast<u32>(l.pick_max[0]));
        const i16 z1 = u16(static_cast<u32>(l.pick_max[2]) + static_cast<u32>(o.pos[2]));
        const int X0 = x0 - camera.scroll_x + 128, X1 = x1 - camera.scroll_x + 128;
        const int Y0 = (z0 - (y0 >> 1)) - camera.scroll_y + 32, Y1 = (z1 - (y0 >> 1)) - camera.scroll_y + 32;
        const u32 age = tick - o.created_tick;
        const i32 a = age < 10 ? static_cast<i32>(age) : 10;
        const i32 dx = ((X1 - X0) * a) / 10, dz = ((Y1 - Y0) * a) / 10;
        const bool sel = o.unit && (o.unit->flags & kSelected);
        const std::uint8_t outer = c_.hud[sel ? 3 : 1], inner = c_.hud[sel ? 10 : 9];
        line(X0 + dx - 1, Y0 - 1, X0 + dx - 1, Y1 + 1, outer);
        line(X1 - dx + 1, Y0 - 1, X1 - dx + 1, Y1 + 1, outer);
        line(X0 - 1, Y0 + dz - 1, X1 + 1, Y0 + dz - 1, outer);
        line(X0 - 1, Y1 - dz + 1, X1 + 1, Y1 - dz + 1, outer);
        line(X0 + dx, Y0, X0 + dx, Y1, inner);
        line(X1 - dx, Y0, X1 - dx, Y1, inner);
        line(X0, Y0 + dz, X1, Y0 + dz, inner);
        line(X0, Y1 - dz, X1, Y1 - dz, inner);
        at = o.pos;
    };

    // The unit's own ranges (once per unit): the cloak distance while it is cloaked ([15]); a
    // kamikaze's pulsing blast ring and its kamikaze distance (sight distance without motion)
    // in [12].
    auto ranges = [&](const sim::Unit& u) {
        const UnitTypeLook* l = look_of(u);
        if (!l) return;
        if (opt_.show_ranges) {
            // +showranges (GS+0x391BF): every range of the type in colour 14, each labelled
            // further round its ring, then the weapons' ranges (slots 1 and 3 by slot 1's flag 2,
            // slot 2 by its own, as the original reads them) in colour 4 / 12 by the step.
            int k = 0;
            auto labelled = [&](int r, const char* text) {
                if (r == 0) return;
                ring(u.pos, r, c_.hud[14], text, k);
                ++k;
            };
            labelled(l->min_cloak_distance, "mincloak");
            labelled(l->sight_distance, "sight");
            labelled(l->radar_distance, "radar");
            labelled(l->sonar_distance, "sonar");
            labelled(l->radar_jam, "radarjam");
            labelled(l->sonar_jam, "sonarjam");
            labelled(l->build_distance, "build distance");
            labelled(l->maneuver_leash, "maneuver");
            if (l->kamikaze_distance != 0) ring(u.pos, l->kamikaze_distance, c_.hud[14], "kamikazedistance", k);
            const std::uint8_t colour = (tick & 1) ? c_.hud[4] : c_.hud[12];
            const sim::WeaponType* w0 = u.weapons[0].type;
            const sim::WeaponType* w1 = u.weapons[1].type;
            const sim::WeaponType* w2 = u.weapons[2].type;
            if ((u.weapons[0].flags & 2) && w0 && w0->range != 0) ring(u.pos, w0->range, colour, "weapon1 range", 0);
            if ((u.weapons[1].flags & 2) && w1 && w1->range != 0) ring(u.pos, w1->range, colour, "weapon2 range", 1);
            if ((u.weapons[0].flags & 2) && w2 && w2->range != 0) ring(u.pos, w2->range, colour, "weapon3 range", 2);
            return;
        }
        if (l->min_cloak_distance != 0 && (u.activate_flags & 4)) ring(u.pos, l->min_cloak_distance, c_.hud[15]);
        if (l->kamikaze) {
            const u32 half = static_cast<u32>(l->explosion_area >> 1);
            const u32 pulse = ((tick % 60u) * half * 2u) / 60u;
            u32 r = pulse > 7 ? pulse : 8;
            if (half <= r) r = half;
            ring(u.pos, static_cast<int>(r), c_.hud[12]);
            const bool moves = u.id < b_.ticks.size() && b_.ticks[u.id].has_motion;
            ring(u.pos, moves ? static_cast<int>(l->kamikaze_distance) : static_cast<int>(l->sight_distance), c_.hud[12]);
        }
    };

    const sim::PlayerSlot& me = b_.players[static_cast<std::size_t>(opt_.player)];
    for (int id = std::max(0, me.first_unit); id <= me.last_unit && static_cast<std::size_t>(id) < b_.units.size(); ++id) {
        const sim::Unit& u = b_.units[static_cast<std::size_t>(id)];
        if (!(u.flags & kAlive) || (u.flags & kDying)) continue;
        std::uint8_t mask = 0;
        bool dots = false;
        if (id == tracked_ || id == focus_ || id == hover_) {
            mask = 0x1F;
            dots = true;
        } else if (u.flags & kSelected) {
            mask = 0x1F;
        } else if (builder) {
            mask = 1;
            dots = true;
        } else {
            continue;
        }
        Point prev = u.pos;
        bool own_ranges = false;
        for (const sim::MissionOrder* o = u.orders; o; o = o->next) {
            const std::uint8_t d = (o->kind < c_.mission_draw.size() ? c_.mission_draw[o->kind] : 0) & mask;
            Point at = prev;
            if (d & 1) {
                at = prev;
                box(*o, at);
            }
            if (d & 2) {
                at = prev;
                path(*o, at, dots);
            }
            if (d & 8) {
                at = prev;
                marker(*o, at);
            }
            if ((d & 0x10) && !own_ranges) {
                ranges(u);
                own_ranges = true;
            }
            prev = at;
        }
    }
    if (order_seen_.size() > 4096) order_seen_.erase(order_seen_.begin(), order_seen_.begin() + 2048);
}

// ---- +contour --------------------------------------------------------------------------------

void GameClient::draw_contours(render::DrawList& out, const render::Camera& camera) const {
    // 0x418310 right after the terrain, with the contour interval (0x511DD0) set: the view's
    // cells from (scroll / 16, truncated) across the view's width in cells + 1 (at most the map's
    // width - 1), row after row while a row's cells have a top corner above the screen's bottom
    // (at most the map's height - 1). A cell's corners are its four height points (cell, +x,
    // +x+z, +z) at (16 (x + 8) - scroll x, 16 (z + 2) - height / 2 - scroll y); four triangles
    // meet at their mean (sums + 2, divided by 4 toward zero) with the mean height, heights x 256
    // (0x4181D0); each triangle (0x417F60) gets a line wherever a level L = (top / interval) x
    // interval + offset, less the interval while above the top, ... crosses it, coloured by
    // ((L >> 8) - sea level + 256) >> 4 from the table at 0x501D18.
    const std::int32_t I = opt_.contour_interval, O = opt_.contour_offset;
    if (I == 0) return;
    if (I < 0) return;  // (the original's level loops never end: the game hangs)
    static const std::uint8_t kColours[32] = {0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   111, 110, 109, 108, 107, 106,
                                              88,  87,  86,  85,  84,  83,  82,  81,  80,  255, 255, 255, 255, 255, 255, 255};
    const sim::MapGrid& map = b_.map;
    if (map.width < 2 || map.height < 2 || map.cells.size() < static_cast<std::size_t>(map.width) * static_cast<std::size_t>(map.height)) return;
    const render::ClipRect clip = camera.view_clip();
    const int sea = b_.settings.sea_level;
    auto colour_of = [&](i32 level) -> std::uint8_t {
        const i32 k = (((level >> 8) - sea) + 0x100) >> 4;
        return k >= 0 && k < 32 ? kColours[k] : 0;
    };
    auto tri = [&](i32 xa, i32 ya, i32 ha, i32 xb, i32 yb, i32 hb, i32 xc, i32 yc, i32 hc) {
        // Sorted by height: a <= b <= c.
        if (hc < hb) {
            std::swap(xb, xc);
            std::swap(yb, yc);
            std::swap(hb, hc);
        }
        if (hb < ha) {
            std::swap(xa, xb);
            std::swap(ya, yb);
            std::swap(ha, hb);
        }
        if (hc < hb) {
            std::swap(xb, xc);
            std::swap(yb, yc);
            std::swap(hb, hc);
        }
        i32 L = (hc / I) * I + O;
        while (hc < L) L -= I;
        if (hb < L) {
            const i32 dca = hc - ha, dcb = hc - hb;
            do {
                const i32 t1 = L - ha, t2 = L - hb;
                out.line((xa * (dca - t1) + t1 * xc) / dca, (ya * (dca - t1) + t1 * yc) / dca, (xb * (dcb - t2) + t2 * xc) / dcb,
                         (yb * (dcb - t2) + t2 * yc) / dcb, colour_of(L), clip);
                L -= I;
            } while (hb < L);
        }
        if (ha < L) {
            const i32 dca = hc - ha, dba = hb - ha;
            do {
                const i32 t = L - ha;
                out.line((xa * (dca - t) + t * xc) / dca, (ya * (dca - t) + t * yc) / dca, (xa * (dba - t) + t * xb) / dba,
                         (ya * (dba - t) + t * yb) / dba, colour_of(L), clip);
                L -= I;
            } while (ha < L);
        }
    };
    auto quarter = [](i32 v) { return (v + ((v >> 31) & 3)) >> 2; };
    auto sixteenth = [](i32 v) { return (v + ((v >> 31) & 15)) >> 4; };
    const int sx0 = camera.scroll_x, sy0 = camera.scroll_y;
    const int x0 = sixteenth(sx0);
    int z = sixteenth(sy0);
    const int x1 = std::min((opt_.width - 128) / 16 + 1 + x0, map.width - 1);
    const int last_row = map.height - 1;
    auto height = [&](int x, int zz) -> i32 {
        return map.cells[static_cast<std::size_t>(zz) * static_cast<std::size_t>(map.width) + static_cast<std::size_t>(x)].height;
    };
    if (z >= last_row || x0 < 0 || z < 0) return;
    bool more;
    do {
        more = false;
        for (int x = x0; x < x1; ++x) {
            const i32 bx = (x + 8) * 16, by = (z + 2) * 16;
            const i32 h[4] = {height(x, z), height(x + 1, z), height(x + 1, z + 1), height(x, z + 1)};
            const i32 px[4] = {bx - sx0, bx + 16 - sx0, bx + 16 - sx0, bx - sx0};
            const i32 py[4] = {by - (h[0] >> 1) - sy0, by - (h[1] >> 1) - sy0, by + 16 - (h[2] >> 1) - sy0, by + 16 - (h[3] >> 1) - sy0};
            if (py[0] < opt_.height) more = true;
            const i32 cxm = quarter(px[0] + px[2] + px[3] + 2 + px[1]);
            const i32 cym = quarter(py[1] + py[0] + py[3] + 2 + py[2]);
            const i32 hm = (h[3] + h[0] + h[2] + h[1]) * 0x40;
            for (int k = 0; k < 4; ++k) {
                const int n = (k + 1) & 3;
                tri(px[k], py[k], h[k] << 8, px[n], py[n], h[n] << 8, cxm, cym, hm);
            }
        }
    } while (more && ++z < last_row);
}

}  // namespace client
