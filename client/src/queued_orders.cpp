// The queued-order view: while Shift is held the frame shows the orders of some of the
// player's units (build boxes, paths, markers, ranges), after the units and effects and before
// the fog (spec: docs/RE_MEMORY.md, "Client", "Queued orders (Shift)").

#include <algorithm>
#include <cmath>

#include "client/game_client.h"
#include "sim/orders.h"
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

    // A ring on the ground around a point: r x 2 pi / 8 segments (whole), each point at the
    // terrain's height or the centre's when that is higher.
    auto ring = [&](const Point& centre, int r, std::uint8_t colour) {
        if (r == 0) return;
        const i32 n = trunc32(static_cast<double>(r) * 6.28318530717958 * 0.125);
        if (n < 0) return;
        if (n == 0) return;  // the original divides by zero here
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
        }
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
        if (!m || m >= c_.cursors.by_number.size()) return;
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

}  // namespace client
