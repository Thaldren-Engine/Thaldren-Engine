#include "sim/motion.h"

#include <cmath>

#include "sim/trig.h"
#include "sim/x87.h"
#include "sim/x87_math.h"
#include "sim/x87_trig.h"

namespace sim {
namespace {

using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using Vec3 = std::array<i32, 3>;

constexpr double kInv65536 = 1.0 / 65536.0;

i32 add(i32 a, i32 b) { return wrap_add(a, b); }
i32 sub(i32 a, i32 b) { return wrap_sub(a, b); }
i32 neg(i32 a) { return static_cast<i32>(0u - static_cast<u32>(a)); }
i32 lo32(i64 v) { return static_cast<i32>(static_cast<u32>(static_cast<u64>(v))); }
i32 shl16(i32 v) { return static_cast<i32>(static_cast<u32>(v) << 16); }
// High 32 bits of the square (the original's quick squared distances).
i32 square_hi(i32 v) { return static_cast<i32>((static_cast<i64>(v) * v) >> 32); }
// 16.16 product kept to 32 bits.
i32 mul16(i32 a, i32 b) { return lo32((static_cast<i64>(a) * b) >> 16); }
i32 hypot_trunc(i32 dx, i32 dz) { return x87::ftol32(x87::crt_hypot(static_cast<double>(dx), static_cast<double>(dz))); }
// Heading from `from` toward `to`.
u16 heading_to(const Vec3& from, const Vec3& to) {
    return static_cast<u16>(x87::atan2_scaled(sub(from[0], to[0]), sub(from[2], to[2])));
}
// Packed waypoint (whole world units) <-> 16.16.
i32 point_x(u32 p) { return shl16(static_cast<i16>(p & 0xFFFF)); }
i32 point_z(u32 p) { return shl16(static_cast<i16>(p >> 16)); }
u32 pack_point(i32 x, i32 z) { return (static_cast<u32>(x) >> 16) | (static_cast<u32>(z) & 0xFFFF0000u); }
u32 pack_cell(i32 x, i32 z) { return (static_cast<u32>(x) & 0xFFFFu) | (static_cast<u32>(z) << 16); }
// Centre (16.16) of a footprint whose corner is at `cell`.
i32 footprint_centre(i16 footprint, i32 cell) { return static_cast<i32>(static_cast<u32>(footprint + cell * 2) << 19); }
i32 cells(i32 radius) { return radius / 16; }  // C division

bool plays(const PlayerSlot& p) { return p.active && (p.controller == 1 || p.controller == 2); }

// Cruise height above the terrain of the unit's bucket (or above the sea).
i32 cruise_height(MotionContext& c, const Unit& u) {
    const MotionType& t = c.type(u);
    if (t.type_flags & motion_flags::kTypeSeaAltitude) return shl16(static_cast<i32>(c.env.sea_level) + t.cruise_altitude);
    return shl16(t.cruise_altitude + static_cast<i32>(c.systems.bucket_height(u)));
}

void link_unit(MotionContext& c, UnitLink& l, Unit* u) {
    if (l.unit) {
        UnitLink** at = &l.unit->referrers;
        while (*at && *at != &l) at = &(*at)->next;
        if (*at) *at = l.next;
        l.unit = nullptr;
        l.next = nullptr;
    }
    if (u && c.ticks[u->id].type_index != 0) {
        l.unit = u;
        l.next = u->referrers;
        u->referrers = &l;
    } else {
        l.unit = nullptr;
        l.next = nullptr;
    }
}

void unlink(UnitLink& l) {
    if (!l.unit) return;
    UnitLink** at = &l.unit->referrers;
    while (*at && *at != &l) at = &(*at)->next;
    if (*at) *at = l.next;
    l.unit = nullptr;
    l.next = nullptr;
}

// Turn toward `want` (a heading difference) by at most the turn rate.
void turn_unit(Unit& u, MotionState& m, u16 diff, u16 rate) {
    if (diff == 0) {
        m.turn = 0;
        return;
    }
    const i32 d = static_cast<i16>(diff);
    const i32 r = rate;
    if (d >= r) m.turn = static_cast<i16>(rate);
    else if (d <= -r) m.turn = static_cast<i16>(-r);
    else m.turn = static_cast<i16>(diff);
    u.heading = static_cast<u16>(u.heading + static_cast<u16>(m.turn));
    u.flags |= motion_flags::kResettle;
}

void delete_order_goal(MotionContext& c, MissionOrder& o) {
    if (o.goal) {
        o.goal->release();
        c.systems.free_object(o.goal->address);
    }
    o.goal.reset();
    o.attached = 0;
}

template <class G>
std::shared_ptr<G> new_goal(MotionContext& c, MissionOrder& order, u32 size) {
    auto g = std::make_shared<G>();
    g->address = c.systems.allocate_object(size);
    g->order = &order;
    g->owner = order.unit;
    return g;
}

}  // namespace

std::int16_t footprint_cell(std::int32_t pos, std::int16_t footprint) {
    const u32 v = static_cast<u32>(pos) - (static_cast<u32>(static_cast<i32>(footprint)) << 19) + 0x80000u;
    return static_cast<i16>(static_cast<i32>(v) >> 20);
}

std::uint32_t motion_stale_word(const UnitTickEnvironment& env) {
    // The step's last callee saves the unit pass's EBX at the word's depth. The
    // pass keeps the live-count address there, except that its water damage
    // test loads 30 (the tick divisor) whenever water damage is on.
    if (env.water_damage_enabled && env.water_damage != 0) return 30;
    return env.addresses.game_state + 0x14353u;
}

// ---------------------------------------------------------------------------
// Order goals.

void OrderGoal::report(std::uint32_t events) const {
    if (order) order->events |= events;
}

bool PointGoal::position(MotionContext& c, std::array<std::int32_t, 3>& out) {
    namespace pg = point_goal;
    if ((flags & pg::kFollow) && !(flags & pg::kFace)) {
        Unit* a = link.unit;
        if (!a || c.systems.off_map(*a)) return false;
        const Vec3 off = piece_local_offset(*a, piece);
        pos = {add(a->pos[0], off[0]), add(a->pos[1], off[1]), add(a->pos[2], off[2])};
        if (flags & pg::kOffset) {
            u16 h = a->heading;
            if (flags & pg::kHeading) h = static_cast<u16>(h + static_cast<u16>(goal_heading));
            pos[0] = add(pos[0], trig::mul_sin(h, offset));
            pos[2] = add(pos[2], trig::mul_cos(h, offset));
        }
        pos[1] = add(pos[1], shl16(altitude));
    } else if (!(flags & pg::kAltitude)) {
        pos[1] = cruise_height(c, *owner);
    }
    if (pos[1] > 0x1FF0000) pos[1] = 0x1FF0000;
    out = pos;
    return true;
}

bool PointGoal::reached(MotionContext& c, Unit& u) {
    namespace pg = point_goal;
    // The original leaves its buffer uninitialised when position() writes
    // nothing (a followed unit that is gone or off the map); the goal's last
    // position stands in for it.
    Vec3 p = pos;
    position(c, p);
    const double dist = x87::crt_hypot(static_cast<double>(sub(u.pos[0], p[0])), static_cast<double>(sub(u.pos[2], p[2]))) * kInv65536;
    if (flags & pg::kRadius) return !(dist >= static_cast<double>(radius));
    if (dist > 0.5) return false;
    if ((flags & pg::kFollow) && !link.unit) return false;
    if ((flags & pg::kMatchHeading) && (!link.unit || u.heading != link.unit->heading)) return false;
    if (flags & pg::kAltitude) {
        const i32 d = sub(u.pos[1], p[1]);
        const i32 a = d < 0 ? neg(d) : d;  // |INT_MIN| stays negative
        if (a > 0x10000) return false;
    }
    return true;
}

bool PointGoal::heading(MotionContext&, std::uint16_t& out) {
    namespace pg = point_goal;
    if ((flags & (pg::kMatchHeading | pg::kFace)) && link.unit) {
        if (flags & pg::kOffset) out = heading_to(owner->pos, link.unit->pos);
        else if (flags & pg::kHeading) out = static_cast<u16>(goal_heading);
        else out = link.unit->heading;
        return true;
    }
    if (flags & pg::kHeading) {
        out = static_cast<u16>(goal_heading);
        return true;
    }
    return false;
}

bool PointGoal::keep_when_reached() const { return (flags & point_goal::kFollow) && link.unit; }

void PointGoal::release() { unlink(link); }

void PointGoal::set_altitude(MotionContext& c, std::int32_t alt) {
    flags = static_cast<u16>(flags | point_goal::kAltitude);
    altitude = static_cast<i16>(alt);
    if (!(flags & point_goal::kGround)) return;
    const i16 x = static_cast<i16>(pos[0] >> 16), z = static_cast<i16>(pos[2] >> 16);
    i32 h = c.env.sea_level;
    if (c.map.sample(x, z) > h) h = c.map.sample(x, z);
    pos[1] = shl16(add(h, alt));
    if (pos[1] > 0x1FF0000) pos[1] = 0x1FF0000;
}

std::shared_ptr<PointGoal> make_point_goal(MotionContext& c, MissionOrder& order, const std::array<std::int32_t, 3>& pos) {
    auto g = new_goal<PointGoal>(c, order, 0x36);
    g->pos = pos;
    g->piece = -1;
    g->flags = point_goal::kGround;
    return g;
}

std::shared_ptr<PointGoal> make_follow_goal(MotionContext& c, MissionOrder& order, Unit* target) {
    auto g = new_goal<PointGoal>(c, order, 0x36);
    link_unit(c, g->link, target);
    g->link.goal = g.get();
    g->pos = g->owner->pos;
    g->piece = -1;
    if (!g->link.unit) c.fault = "motion: follow goal without a target (the original reads through a null pointer)";
    if (!g->link.unit || !(c.type(*g->link.unit).type_flags & motion_flags::kTypeFlies)) {  // the target's type decides
        g->flags = point_goal::kFollow;
        return g;
    }
    g->flags = point_goal::kFollow | point_goal::kOffset | point_goal::kMatchHeading;
    const WeaponType* w = g->owner->weapons[0].type;
    const i32 range = w ? w->range : 0;
    g->offset = range ? shl16(range) : 0x640000;
    return g;
}

std::shared_ptr<PointGoal> make_dock_goal(MotionContext& c, MissionOrder& order, Unit* target, std::int16_t piece) {
    auto g = new_goal<PointGoal>(c, order, 0x36);
    g->piece = piece;
    g->flags = point_goal::kFollow | point_goal::kMatchHeading;
    g->pos = g->owner->pos;
    link_unit(c, g->link, target);
    g->link.goal = g.get();
    return g;
}

std::shared_ptr<PointGoal> make_face_goal(MotionContext& c, MissionOrder& order, Unit* target, const std::array<std::int32_t, 3>& pos) {
    auto g = new_goal<PointGoal>(c, order, 0x36);
    link_unit(c, g->link, target);
    g->link.goal = g.get();
    g->pos = pos;
    g->piece = -1;
    g->flags = point_goal::kFollow | point_goal::kOffset | point_goal::kGround | point_goal::kFace;
    return g;
}

bool ManoeuvreGoal::position(MotionContext& c, std::array<std::int32_t, 3>& out) {
    out = pos;
    pos[0] = add(pos[0], vel[0]);
    pos[2] = add(pos[2], vel[2]);
    const u16 now = heading_to(Vec3{0, 0, 0}, vel);
    if ((flags & 1) && now != target_heading) {
        const i32 d = static_cast<i16>(static_cast<u16>(target_heading - now));
        const i32 rate = c.type(*owner).turn_rate >> 3;
        const i32 t = d >= rate ? rate : d <= -rate ? -rate : d;
        i32 x = vel[0], z = vel[2];
        x87::rotate_point_2d(static_cast<i16>(-t), x, z);
        vel[0] = x;
        vel[2] = z;
        vel[1] = 0;
    }
    return true;
}

bool ManoeuvreGoal::heading(MotionContext&, std::uint16_t& out) {
    out = heading_to(owner->pos, pos);
    return true;
}

bool ManoeuvreGoal::reached(MotionContext&, Unit& u) {
    const double dist = x87::crt_hypot(static_cast<double>(sub(u.pos[0], pos[0])), static_cast<double>(sub(u.pos[2], pos[2]))) * kInv65536;
    if (!(dist >= 48.0)) return true;
    return (flags & 1) && heading_to(Vec3{0, 0, 0}, vel) == target_heading;
}

std::shared_ptr<ManoeuvreGoal> make_manoeuvre_goal(MotionContext& c, MissionOrder& order, const std::array<std::int32_t, 3>& pos,
                                                   const std::array<std::int32_t, 3>& vel) {
    auto g = new_goal<ManoeuvreGoal>(c, order, 0x2C);
    g->pos = pos;
    g->vel = vel;
    return g;
}

bool CircleGoal::contains_cell(std::int32_t x, std::int32_t z) const {
    const i32 dx = sub(x, cell_x), dz = sub(z, cell_z);
    return add(wrap_mul(dx, dx), wrap_mul(dz, dz)) <= radius_cells_sq;
}
bool CircleGoal::reached(MotionContext&, Unit& u) { return contains_cell(u.cell_x, u.cell_z); }
void CircleGoal::goal_cells(std::vector<std::uint32_t>& out) const { out.assign(1, pack_cell(cell_x, cell_z)); }
std::int32_t CircleGoal::distance(std::int32_t x, std::int32_t z) const {
    i32 ax = sub(x, cell_x), az = sub(z, cell_z);
    ax = ax < 0 ? neg(ax) : ax;
    az = az < 0 ? neg(az) : az;
    const i32 d = az < ax ? add(wrap_mul(az, 7), wrap_mul(ax, 18)) : add(wrap_mul(ax, 7), wrap_mul(az, 18));
    return d < radius ? 0 : sub(d, radius);
}
bool CircleGoal::position(MotionContext&, std::array<std::int32_t, 3>& out) {
    const Unit& u = *order->unit;
    out[0] = footprint_centre(u.footprint_x, cell_x);
    out[2] = footprint_centre(u.footprint_z, cell_z);
    return true;
}

std::shared_ptr<CircleGoal> make_circle_goal(MotionContext& c, MissionOrder& order, std::int32_t x, std::int32_t z, std::int32_t radius) {
    auto g = new_goal<CircleGoal>(c, order, 0x14);
    g->cell_x = footprint_cell(x, order.unit->footprint_x);
    g->cell_z = footprint_cell(z, order.unit->footprint_z);
    g->radius = radius;
    g->radius_cells_sq = wrap_mul(cells(radius), cells(radius));
    return g;
}

bool RingGoal::contains_cell(std::int32_t x, std::int32_t z) const {
    const i32 dx = sub(x, cell_x), dz = sub(z, cell_z);
    const i32 d = add(wrap_mul(dx, dx), wrap_mul(dz, dz));
    return d <= outer_cells_sq && inner_cells_sq <= d;
}
bool RingGoal::reached(MotionContext&, Unit& u) { return contains_cell(u.cell_x, u.cell_z); }
void RingGoal::goal_cells(std::vector<std::uint32_t>& out) const {
    const i32 sum = add(outer, inner);
    out.assign(1, pack_cell(cell_x, static_cast<i16>(cell_z + static_cast<i16>(sum / 32))));
}
std::int32_t RingGoal::distance(std::int32_t x, std::int32_t z) const {
    i32 ax = sub(x, cell_x), az = sub(z, cell_z);
    ax = ax < 0 ? neg(ax) : ax;
    az = az < 0 ? neg(az) : az;
    i32 lo = ax, hi = az;
    if (az < ax) {
        lo = az;
        hi = ax;
    }
    const i32 d = add(wrap_mul(lo, 7), wrap_mul(hi, 18));
    if (d > outer) return sub(d, outer);
    return inner <= d ? 0 : sub(inner, d);
}
bool RingGoal::position(MotionContext&, std::array<std::int32_t, 3>& out) {
    const Unit& u = *order->unit;
    out[0] = footprint_centre(u.footprint_x, cell_x);
    out[2] = footprint_centre(u.footprint_z, cell_z);
    const u16 h = heading_to(u.pos, out);
    const i32 mid = shl16(add(outer, inner) / 2);
    out[0] = add(out[0], trig::mul_sin(h, mid));
    out[2] = add(out[2], trig::mul_cos(h, mid));
    return true;
}

std::shared_ptr<RingGoal> make_ring_goal(MotionContext& c, MissionOrder& order, std::int32_t x, std::int32_t z, std::int32_t outer,
                                         std::int32_t inner) {
    auto g = new_goal<RingGoal>(c, order, 0x1C);
    g->cell_x = footprint_cell(x, order.unit->footprint_x);
    g->cell_z = footprint_cell(z, order.unit->footprint_z);
    g->outer = outer;
    g->inner = inner;
    g->outer_cells_sq = wrap_mul(cells(outer), cells(outer));
    g->inner_cells_sq = wrap_mul(cells(inner), cells(inner));
    return g;
}

bool EdgeGoal::contains_cell(std::int32_t x, std::int32_t z) const {
    bool check_x;  // falls through to the x-range test
    if (x == min_x || x == max_x) {
        if (min_z <= z) {
            if (z <= max_z) return true;
            check_x = z == min_z || z == max_z;
        } else {
            check_x = z == max_z;
        }
    } else {
        check_x = z == min_z || z == max_z;
    }
    return check_x && min_x <= x && x <= max_x;
}
bool EdgeGoal::reached(MotionContext&, Unit& u) { return contains_cell(u.cell_x, u.cell_z); }
void EdgeGoal::goal_cells(std::vector<std::uint32_t>& out) const {
    out.clear();
    for (i32 x = min_x; x <= max_x; ++x) {
        out.push_back(pack_cell(x, min_z));
        out.push_back(pack_cell(x, max_z));
    }
    for (i32 z = min_z + 1; z <= max_z - 1; ++z) {
        out.push_back(pack_cell(min_x, z));
        out.push_back(pack_cell(max_x, z));
    }
}
std::int32_t EdgeGoal::distance(std::int32_t x, std::int32_t z) const {
    i32 dx;
    if (x < min_x) {
        dx = min_x - x;
    } else if (x <= max_x) {
        if (z < min_z) return (min_z - z) * 16;
        if (max_z < z) return (z - max_z) * 16;
        i32 ex = x - min_x;
        if (max_x - x <= x - min_x) ex = max_x - x;
        i32 ez = max_z - z;
        if (z - min_z < max_z - z) ez = z - min_z;
        return (ex < ez ? ex : ez) << 4;
    } else {
        dx = x - max_x;
    }
    i32 dz;
    if (z < min_z) dz = min_z - z;
    else if (z <= max_z) return dx << 4;
    else dz = z - max_z;
    return dz < dx ? dx * 16 + dz * 6 : dz * 16 + dx * 6;
}
bool EdgeGoal::position(MotionContext&, std::array<std::int32_t, 3>& out) {
    const Unit& u = *order->unit;
    out[0] = footprint_centre(u.footprint_x, static_cast<i16>(add(min_x, max_x) / 2));
    out[2] = footprint_centre(u.footprint_z, static_cast<i16>(max_z));
    return true;
}

std::shared_ptr<EdgeGoal> make_edge_goal(MotionContext& c, MissionOrder& order, std::uint32_t cell, std::uint32_t size) {
    auto g = new_goal<EdgeGoal>(c, order, 0x18);
    const i32 cx = static_cast<i16>(cell & 0xFFFF), cz = static_cast<i16>(cell >> 16);
    g->min_x = cx - order.unit->footprint_x;
    g->min_z = cz - order.unit->footprint_z;
    g->max_x = static_cast<i16>(size & 0xFFFF) + cx;
    g->max_z = static_cast<i16>(size >> 16) + cz;
    return g;
}

// ---------------------------------------------------------------------------
// Motion goals.

void MotionGoal::attach(MotionContext&, OrderGoal* goal) {
    if (attached) attached->report(goal_events::kReplaced);
    attached = goal;
}

void RouteGoal::attach(MotionContext& c, OrderGoal* goal) {
    c.systems.route_search_forget(*this);
    if (attached) attached->report(goal_events::kReplaced);
    flags = static_cast<std::uint8_t>(flags & ~route_flags::kHasRoute);
    attached = goal;
    if (!goal) {
        flags = static_cast<std::uint8_t>(flags & ~route_flags::kWantsRoute);
    } else {
        flags |= route_flags::kWantsRoute;
        if (count >= 3) {
            const u32 last = points[static_cast<std::size_t>(count - 1)];
            if (goal->contains_cell(static_cast<i16>(last & 0xFFFF) >> 4, static_cast<i16>(last >> 16) >> 4))
                flags = static_cast<std::uint8_t>((flags & ~route_flags::kWantsRoute) | route_flags::kHasRoute);
        }
        Vec3 out{};
        if (!(flags & route_flags::kHasRoute) && goal->position(c, out)) {
            if (count >= 3) {
                // The current route still serves if its end is much nearer the goal than the unit is.
                const u32 last = points[static_cast<std::size_t>(count - 1)];
                const i32 d_unit = hypot_trunc(sub(unit->pos[0], out[0]), sub(unit->pos[2], out[2]));
                const i32 d_end = hypot_trunc(sub(point_x(last), out[0]), sub(point_z(last), out[2]));
                if (add(d_end, d_end) < d_unit) flags |= route_flags::kHasRoute;
            }
            if (!(flags & route_flags::kHasRoute)) {
                const MissionOrder* head = unit->orders;
                if (head && !(head->flags & order_flags::kLastOrder)) {
                    // Until the search answers, head straight for the goal.
                    count = 2;
                    points[0] = pack_point(unit->pos[0], unit->pos[2]);
                    points[1] = pack_point(out[0], out[2]);
                    flags |= route_flags::kHasRoute;
                }
            }
        }
    }
    if (search_tick <= c.env.game_tick - 10u) search_tick = 0;
    flags |= route_flags::kChanged;
}

void RouteGoal::step(MotionContext& c) {
    if (OrderGoal* g = attached) {
        if (g->reached(c, *unit)) {
            g->report(goal_events::kReached);
            if (!g->keep_when_reached()) attach(c, nullptr);
        }
    }
    if (count >= 2) {
        // Close to the next waypoint: it becomes the start of the remaining route.
        const i32 dx = static_cast<i16>(unit->pos[0] >> 16) - static_cast<i16>(points[1] & 0xFFFF);
        const i32 dz = static_cast<i16>(unit->pos[2] >> 16) - static_cast<i16>(points[1] >> 16);
        if (add(wrap_mul(dx, dx), wrap_mul(dz, dz)) <= 0x19) {
            for (i32 k = 1; k < count; ++k) points[static_cast<std::size_t>(k - 1)] = points[static_cast<std::size_t>(k)];
            if (--count < 2) flags = static_cast<std::uint8_t>(flags & ~route_flags::kHasRoute);
            flags |= route_flags::kChanged;
        }
    }
    if (attached && ((c.motion(*unit).flags & motion_flags::kBlocked) || count < 2)) flags |= route_flags::kWantsRoute;
}

void RouteGoal::route_points(MotionContext& c, std::array<std::array<std::int32_t, 3>, 3>& out, int n) {
    for (int i = 0; i < n; ++i) {
        const i32 k = i < count ? i : count - 1;
        // With no waypoints the original reads the goal's unit pointer as one.
        const u32 p = k >= 0 ? points[static_cast<std::size_t>(k)] : c.env.addresses.unit(unit->id);
        out[static_cast<std::size_t>(i)] = {point_x(p), 0, point_z(p)};
    }
}

bool RouteGoal::wants_route(MotionContext& c) {
    if ((flags & route_flags::kWantsRoute) && search_tick + 60u <= c.env.game_tick) {
        search_tick = c.env.game_tick;
        return true;
    }
    return false;
}

void RouteGoal::set_route(MotionContext& c, const std::uint32_t* p, std::int32_t n) {
    if (n == 0) {
        if (attached && !attached->reached(c, *unit)) attached->report(goal_events::kNoRoute);
        flags = static_cast<std::uint8_t>((flags & ~(route_flags::kHasRoute | route_flags::kWantsRoute)) | route_flags::kChanged);
        return;
    }
    if (n >= 20) n = 20;
    count = n;
    for (i32 k = 0; k < n; ++k) points[static_cast<std::size_t>(k)] = p[k];
    flags = static_cast<std::uint8_t>(((flags | route_flags::kHasRoute) & ~route_flags::kWantsRoute) | route_flags::kChanged);
}

void FlightGoal::attach(MotionContext& c, OrderGoal* goal) {
    MotionGoal::attach(c, goal);
    if (local) net_flags |= 1;
}

void FlightGoal::step(MotionContext& c) {
    if (local && (c.motion(*unit).flags & motion_flags::kLayer) != ((net_flags >> 1) & 3)) net_flags |= 1;
    OrderGoal* g = attached;
    if (!g) return;
    const Vec3 old = pos;
    g->position(c, pos);
    delta = {sub(pos[0], old[0]), sub(pos[1], old[1]), sub(pos[2], old[2])};
    const i32 d = hypot_trunc(sub(unit->pos[0], pos[0]), sub(unit->pos[2], pos[2]));
    if (d > 0xA00000) pos[1] = cruise_height(c, *unit);
    if (d > 0x1400000 || (!g->heading(c, target_heading) && d > 0x100000)) target_heading = heading_to(unit->pos, pos);
    if (g->reached(c, *unit)) {
        g->report(goal_events::kReached);
        if (!g->keep_when_reached()) attach(c, nullptr);
    }
}

void FlightGoal::flight_target(std::array<std::int32_t, 3>& p, std::array<std::int32_t, 3>& d, std::uint16_t& h) {
    p = pos;
    d = delta;
    h = target_heading;
}

void RemoteRouteGoal::route_points(MotionContext& c, std::array<std::array<std::int32_t, 3>, 3>& out, int n) {
    for (int i = 0; i < n; ++i) {
        const i32 k = i < count ? i : count - 1;
        const u32 p = k >= 0 ? points[static_cast<std::size_t>(k)] : c.env.addresses.unit(unit->id);
        out[static_cast<std::size_t>(i)] = {point_x(p), 0, point_z(p)};
    }
}

// ---------------------------------------------------------------------------
// The motion object.

void create_motion(MotionContext& c, Unit& u) {
    UnitTickState& t = c.tick(u);
    MotionState& m = t.motion;
    const MotionType& ty = c.type(u);
    t.has_motion = true;
    t.velocity = {0, 0, 0};
    m.speed = 0;
    m.turn = 0;
    m.claim_tick = 0;
    m.tilt = {0, 0, 0};
    m.flags = static_cast<std::uint8_t>((m.flags & 0xF9) | 1);
    m.has_move_class = ty.has_move_class;
    const PlayerSlot& owner = c.players[static_cast<std::size_t>(t.owner)];
    const bool remote = owner.active && owner.controller == 3;
    const bool flies = (ty.type_flags & motion_flags::kTypeFlies) != 0;
    if (!remote && !flies) {
        auto g = std::make_shared<RouteGoal>();
        g->address = c.systems.allocate_object(0x65);
        g->unit = &u;
        g->count = 0;
        g->flags = static_cast<std::uint8_t>((g->flags & 0xFC) | route_flags::kChanged);
        g->search_tick = 0;
        m.goal = g;
    } else if (!remote) {
        auto g = std::make_shared<FlightGoal>();
        g->address = c.systems.allocate_object(0x28);
        g->unit = &u;
        g->pos = u.pos;
        g->target_heading = u.heading;
        g->net_flags = static_cast<std::uint8_t>((g->net_flags & 0xF9) | 1);
        m.goal = g;
    } else if (!flies) {
        auto g = std::make_shared<RemoteRouteGoal>();
        g->address = c.systems.allocate_object(0x1C);
        g->unit = &u;
        m.goal = g;
    } else {
        auto g = std::make_shared<FlightGoal>();
        g->address = c.systems.allocate_object(0x27);
        g->unit = &u;
        g->pos = u.pos;
        g->target_heading = u.heading;
        g->local = false;
        m.goal = g;
    }
}

void destroy_motion(MotionContext& c, Unit& u) {
    UnitTickState& t = c.tick(u);
    if (MotionGoal* g = t.motion.goal.get()) {
        if (dynamic_cast<RouteGoal*>(g)) c.systems.route_search_forget(*g);
        c.systems.free_object(g->address);
    }
    t.motion.goal.reset();
    t.has_motion = false;
}

void accelerate(MotionContext& c, Unit& u, std::int32_t delta) {
    static constexpr std::int8_t kSlopeFactor[11] = {25, 55, 70, 85, 100, 100, 75, 50, 25, 20, 15};  // percent by bank
    MotionState& m = c.motion(u);
    const MotionType& ty = c.type(u);
    m.speed = add(m.speed, delta);
    if (m.speed < 0) m.speed = 0;
    i32 slope = u.bank >> 11;
    if (slope < -5) slope = -5;
    if (slope > 5) slope = 5;
    const i32 f = kSlopeFactor[slope + 5];
    const i32 scaled = lo32((static_cast<i64>(f << 16) * ty.max_velocity) >> 16);
    i32 max = lo32((static_cast<i64>(scaled) << 16) / 0x640000);
    if (static_cast<i16>(u.pos[1] >> 16) < static_cast<i16>(c.env.sea_level) &&
        !(ty.type_flags & (motion_flags::kTypeFloats | motion_flags::kTypeHover)))
        max = lo32((static_cast<i64>(max) * 0x8000) >> 16);  // half speed under water
    if (m.speed > max) m.speed = max;
    auto& v = c.tick(u).velocity;
    v = {neg(trig::mul_sin(u.heading, m.speed)), 0, neg(trig::mul_cos(u.heading, m.speed))};
}

void steer_ground(MotionContext& c, Unit& u) {
    MotionState& m = c.motion(u);
    const MotionType& ty = c.type(u);
    if (!m.goal->has_route()) {
        m.turn = 0;
        accelerate(c, u, neg(ty.brake_rate));
        return;
    }
    // The previous waypoint, the next one and the one after.
    std::array<Vec3, 3> p{};
    m.goal->route_points(c, p, 3);
    i32 tx = p[1][0], tz = p[1][2];
    const i32 d_next = hypot_trunc(sub(tx, u.pos[0]), sub(tz, u.pos[2]));
    if (d_next > 0x500000) {
        // Far from the next waypoint: aim at the leg 80 units short of it (pulls the unit onto the leg).
        const i32 sx = sub(p[1][0], p[0][0]), sz = sub(p[1][2], p[0][2]);
        const i32 leg = hypot_trunc(sx, sz);
        if (leg >= 0x10000) {
            const i32 ux = lo32((static_cast<i64>(sx) << 16) / leg);
            const i32 uz = lo32((static_cast<i64>(sz) << 16) / leg);
            i32 back = sub(d_next, 0x500000);
            if (back > leg) back = leg;
            tx = sub(tx, mul16(ux, back));
            tz = sub(tz, mul16(uz, back));
        }
    }
    const i32 dx = sub(tx, u.pos[0]), dz = sub(tz, u.pos[2]);
    const i32 near_next = add(square_hi(dx), square_hi(dz));
    const Vec3 target{tx, 0, tz};
    const u16 diff = static_cast<u16>(heading_to(u.pos, target) - u.heading);
    const i32 turn_left = static_cast<i16>(diff);
    const i32 abs_turn = turn_left < 0 ? -turn_left : turn_left;
    const i32 dx2 = sub(p[2][0], u.pos[0]), dz2 = sub(p[2][2], u.pos[2]);
    const i32 near_after = add(square_hi(dx2), square_hi(dz2));
    turn_unit(u, m, diff, ty.turn_rate);
    // Brake when the turn still to make, or the stopping distance, is too long
    // for the distance left.
    if (ty.turn_rate == 0 || add(ty.brake_rate, ty.brake_rate) == 0) {
        c.fault = "motion: division by zero (turn rate or brake rate 0)";
        return;
    }
    const i32 turn_time = lo32((static_cast<i64>(abs_turn & 0xFFFF) * m.speed) / ty.turn_rate);
    const i32 speed_sq = lo32((static_cast<i64>(m.speed) * m.speed) >> 16);
    const i32 stop = lo32((static_cast<i64>(speed_sq) << 16) / add(ty.brake_rate, ty.brake_rate));
    const i32 stop_sq = square_hi(stop);
    const i32 turn_sq4 = static_cast<i32>(static_cast<u32>(square_hi(turn_time)) << 2);
    const i32 delta = (near_next > turn_sq4 && near_after > stop_sq) ? ty.acceleration : neg(ty.brake_rate);
    accelerate(c, u, delta);
}

void lean(MotionContext& c, Unit& u, const std::array<std::int32_t, 3>& impulse) {
    MotionState& m = c.motion(u);
    const MotionType& ty = c.type(u);
    for (int k = 0; k < 3; ++k) m.tilt[static_cast<std::size_t>(k)] = lo32((static_cast<i64>(m.tilt[static_cast<std::size_t>(k)]) * 0xF333) >> 16);
    for (int k = 0; k < 3; ++k) m.tilt[static_cast<std::size_t>(k)] = add(m.tilt[static_cast<std::size_t>(k)], impulse[static_cast<std::size_t>(k)]);
    i32 x = m.tilt[0], z = m.tilt[2];
    x87::rotate_point_2d(static_cast<i16>(u.heading), x, z);
    const i32 down = lo32((static_cast<i64>(c.env.gravity) << 16) / 0xCCD);
    u.pitch = static_cast<i16>(x87::atan2_scaled(lo32((static_cast<i64>(ty.bank_scale) * neg(x)) >> 16), down));
    u.bank = static_cast<i16>(x87::atan2_scaled(lo32((static_cast<i64>(ty.pitch_scale) * neg(x)) >> 16), down));
}

void steer_air(MotionContext& c, Unit& u) {
    MotionState& m = c.motion(u);
    auto& v = c.tick(u).velocity;
    if ((m.flags & motion_flags::kLayer) != 2) {
        v = {0, 0, 0};
        m.speed = 0;
        m.turn = 0;
        return;
    }
    const MotionType& ty = c.type(u);
    const Vec3 old = v;
    // A goal without a flight target leaves these uninitialised in the original.
    Vec3 goal_pos{}, goal_delta{};
    u16 goal_heading = 0;
    m.goal->flight_target(goal_pos, goal_delta, goal_heading);
    if (ty.max_velocity == 0) {
        c.fault = "motion: division by zero (max velocity 0)";
        return;
    }
    const float accel = static_cast<float>(static_cast<double>(ty.acceleration) * kInv65536);
    // Drag: the acceleration exactly balances it at full speed.
    const i32 damp = sub(0x10000, lo32((static_cast<i64>(ty.acceleration) << 16) / ty.max_velocity));
    for (auto& k : v) k = mul16(k, damp);
    const float horizontal = static_cast<float>(x87::crt_hypot(static_cast<double>(v[0]), static_cast<double>(v[2])) * kInv65536);
    const float brake = static_cast<float>(static_cast<double>(ty.brake_rate) * kInv65536);
    if (horizontal > brake) {
        // Above the brake rate: turn the velocity toward the heading.
        const i32 keep = x87::ftol32(static_cast<double>(brake) / static_cast<double>(horizontal) * 65536.0);
        v[0] = mul16(v[0], keep);
        v[2] = mul16(v[2], keep);
        const i32 forward = x87::ftol32((static_cast<double>(horizontal) - static_cast<double>(brake)) * 65536.0);
        v[0] = sub(v[0], trig::mul_sin(u.heading, forward));
        v[2] = sub(v[2], trig::mul_cos(u.heading, forward));
    }
    const i32 dx = sub(u.pos[0], goal_pos[0]), dy = sub(u.pos[1], goal_pos[1]), dz = sub(u.pos[2], goal_pos[2]);
    const i32 rvx = sub(v[0], goal_delta[0]), rvz = sub(v[2], goal_delta[2]);
    if (!c.systems.off_map(u)) {
        const i32 limit = (m.speed & ~3) < 0x40000 ? 0x10000 : (m.speed >> 2);
        if (dy <= neg(limit)) v[1] = limit;
        else if (dy >= limit) v[1] = neg(limit);
        else v[1] = neg(dy);
    }
    double dist = x87::crt_hypot(static_cast<double>(dx), static_cast<double>(dz)) * kInv65536;
    turn_unit(u, m, static_cast<u16>(goal_heading - u.heading), ty.turn_rate);
    if (!(dist >= 8.0)) dist = 8.0;
    // Steer the velocity relative to the moving target toward a closing speed
    // that falls with the distance (the speed at which the acceleration stops it there).
    const double k = -std::sqrt((static_cast<double>(accel) + static_cast<double>(accel)) / dist);
    const double ax = k * dx * kInv65536 - rvx * kInv65536;
    const double az = dz * k * kInv65536 - rvz * kInv65536;
    float ax_f = static_cast<float>(ax);
    float az_f = static_cast<float>(az);
    const double h = x87::crt_hypot(ax, az);
    double ax_s = ax_f;
    if (h > static_cast<double>(accel)) {
        const double s = static_cast<double>(accel) / h;
        az_f = static_cast<float>(static_cast<double>(az_f) * s);
        ax_s = static_cast<double>(ax_f) * s;
    }
    v[0] = add(v[0], x87::ftol32(ax_s * 65536.0));
    v[2] = add(v[2], x87::ftol32(static_cast<double>(az_f) * 65536.0));
    const double vx = v[0], vy = v[1], vz = v[2];
    m.speed = x87::ftol32(std::sqrt((vx * vx + vy * vy) + vz * vz));
    lean(c, u, {sub(v[0], old[0]), sub(v[1], old[1]), sub(v[2], old[2])});
}

void place_unit(MotionContext& c, Unit& u, const std::array<std::int32_t, 3>& pos, std::uint32_t layer) {
    const i16 cx = footprint_cell(pos[0], u.footprint_x), cz = footprint_cell(pos[2], u.footprint_z);
    if (cx == u.cell_x && cz == u.cell_z && layer == (u.flags & motion_flags::kLayerMask)) {
        u.pos = pos;
    } else {
        c.systems.clear_footprint(u);
        u.pos = pos;
        u.cell_x = cx;
        u.cell_z = cz;
        u.flags = (layer & motion_flags::kLayerMask) | (u.flags & ~motion_flags::kLayerMask);
        c.systems.claim_footprint(u);
        c.systems.update_sight(u);
    }
    u.flags |= motion_flags::kResettle;
}

void move_unit(MotionContext& c, Unit& u) {
    UnitTickState& t = c.tick(u);
    MotionState& m = t.motion;
    auto& v = t.velocity;
    const MotionType& ty = c.type(u);
    if (Unit* carrier = u.carrier) {
        // Carried: sit on the carrier's piece and move with the carrier.
        const Vec3 off = piece_local_offset(*carrier, u.transport_piece);
        Vec3 p{add(carrier->pos[0], off[0]), add(carrier->pos[1], off[1]), add(carrier->pos[2], off[2])};
        if (ty.type_flags & motion_flags::kTypeFloats) {
            const i32 waterline = static_cast<i32>((static_cast<u32>(ty.waterline) * 0xFFFFu + c.env.sea_level) << 16);
            if (p[1] <= waterline) p[1] = waterline;
        }
        place_unit(c, u, p, m.flags & motion_flags::kLayer);
        const auto& pieces = carrier->pose.pieces;
        std::array<i16, 3> turn{};  // the original reads out of range pieces anyway
        if (u.transport_piece >= 0 && static_cast<std::size_t>(u.transport_piece) < pieces.size())
            turn = pieces[static_cast<std::size_t>(u.transport_piece)].turn;
        u.pitch = static_cast<i16>(carrier->pitch + turn[2]);
        u.heading = static_cast<u16>(carrier->heading + static_cast<u16>(turn[1]));
        u.bank = static_cast<i16>(carrier->bank + turn[0]);
        const UnitTickState& ct = c.tick(*carrier);
        if (ct.has_motion) {
            m.speed = ct.motion.speed;
            v = ct.velocity;
        } else {
            m.speed = 0;
            v = {0, 0, 0};
        }
        u.flags &= ~motion_flags::kResettle;
        return;
    }
    Vec3 n{add(u.pos[0], v[0]), add(u.pos[1], v[1]), add(u.pos[2], v[2])};
    const u32 layer = m.flags & motion_flags::kLayer;
    if (n == u.pos && layer == (u.flags & motion_flags::kLayerMask)) return;
    m.move_tick = c.env.game_tick;
    const i16 cx = footprint_cell(n[0], u.footprint_x), cz = footprint_cell(n[2], u.footprint_z);
    if (cx == u.cell_x && cz == u.cell_z && layer == (u.flags & motion_flags::kLayerMask)) {
        u.pos = n;
        u.flags |= motion_flags::kResettle;
        return;
    }
    // Into new cells: the occupancy grid must allow it (only the owner's simulation asks).
    if (plays(c.players[static_cast<std::size_t>(t.owner)]))
        m.flags = static_cast<std::uint8_t>((m.flags & ~motion_flags::kBlocked) | (c.systems.can_place(u, cx, cz, layer) ? 0 : motion_flags::kBlocked));
    if (m.flags & motion_flags::kBlocked) {
        // Refused: stay within half a cell of the current footprint centre, at most half speed.
        const i32 wx = footprint_centre(u.footprint_x, u.cell_x), wz = footprint_centre(u.footprint_z, u.cell_z);
        if (n[0] > add(wx, 0x7FFFF)) n[0] = add(wx, 0x7FFFF);
        else if (n[0] < sub(wx, 0x7FFFF)) n[0] = sub(wx, 0x7FFFF);
        if (n[2] > add(wz, 0x7FFFF)) n[2] = add(wz, 0x7FFFF);
        else if (n[2] < sub(wz, 0x7FFFF)) n[2] = sub(wz, 0x7FFFF);
        const i32 half = ty.max_velocity / 2;
        if (m.speed > half) {
            m.speed = half;
            v = {neg(trig::mul_sin(u.heading, half)), 0, neg(trig::mul_cos(u.heading, half))};
        }
        u.pos = n;
        u.flags |= motion_flags::kResettle;
        return;
    }
    c.systems.clear_footprint(u);
    u.pos = n;
    u.cell_x = cx;
    u.cell_z = cz;
    u.flags = layer | (u.flags & ~motion_flags::kLayerMask);
    c.systems.claim_footprint(u);
    u.flags |= motion_flags::kResettle;
    c.systems.update_sight(u);
}

void notify_move_rate(MotionContext& c, Unit& u) {
    const MotionState& m = c.motion(u);
    const MotionType& ty = c.type(u);
    u32 rate = 0;
    if (!(m.flags & motion_flags::kBlocked) && !u.carrier && (m.speed != 0 || m.turn != 0))
        rate = m.speed <= ty.move_rate1 ? 1u : (m.speed > ty.move_rate2 ? 3u : 2u);
    if (rate == ((u.flags >> 2) & 3)) return;
    if (u.script) {
        if (rate == 0) u.script->call("StopMoving", nullptr, true);
        else if (!(u.flags & motion_flags::kMoveRateMask)) u.script->call("StartMoving", nullptr, true);
        if (rate == 1) u.script->call("MoveRate1", nullptr, true);
        else if (rate == 2) u.script->call("MoveRate2", nullptr, true);
        else if (rate == 3) u.script->call("MoveRate3", nullptr, true);
    }
    u.flags = (rate << 2) | (u.flags & ~motion_flags::kMoveRateMask);
}

void notify_water_occupancy(MotionContext& c, Unit& u) {
    const MotionType& ty = c.type(u);
    const i32 sea = c.env.sea_level;
    const i32 y = static_cast<i16>(u.pos[1] >> 16);
    const u32 layer = u.flags & motion_flags::kLayerMask;
    i32 v;
    if (layer != 1 && layer != 2) {
        v = 0;
    } else if (y > sea) {
        v = 4;  // above the water
    } else {
        v = u.water_occupancy;
        if (y - sea > -5) v = 1;                  // in shallow water
        if (static_cast<i32>(ty.waterline) + y == sea) v = 2;  // floating at its waterline
        if (ty.model_height + y < sea) v = 3;     // submerged
    }
    if (u.water_occupancy == v) return;
    if (u.script) u.script->call_with_args(u.script->program().find_function("setSFXoccupy"), nullptr, true, 1, v, 0, 0, 0);
    u.water_occupancy = v;
}

void run_motion(MotionContext& c, Unit& u) {
    MotionState& m = c.motion(u);
    m.goal->step(c);
    if (c.type(u).type_flags & motion_flags::kTypeFlies) steer_air(c, u);
    else steer_ground(c, u);
    if (!c.fault.empty()) return;
    move_unit(c, u);
    notify_move_rate(c, u);
    notify_water_occupancy(c, u);
}

void set_motion_layer(MotionContext& c, Unit& u, std::uint32_t layer) {
    MotionState& m = c.motion(u);
    if (static_cast<std::uint32_t>(m.flags & motion_flags::kLayer) == layer) return;
    if (layer == 1) {
        c.tick(u).velocity = {0, 0, 0};
        m.speed = 0;
        lean(c, u, {0, 0, 0});
        c.systems.set_activated(u, false);
    } else {
        c.systems.set_activated(u, true);
    }
    m.flags = static_cast<std::uint8_t>((m.flags & ~motion_flags::kLayer) | (layer & motion_flags::kLayer));
}

void attach_order_goal(MotionContext& c, MissionOrder& order, std::shared_ptr<OrderGoal> goal) {
    Unit& u = *order.unit;
    if (!c.tick(u).has_motion) return;  // a new goal is then never freed (the original leaks it)
    MotionGoal& mg = *c.motion(u).goal;
    if (order.attached) {
        mg.attach(c, nullptr);
        delete_order_goal(c, order);
    }
    if (goal) {
        order.events &= ~goal_events::kAll;
        mg.attach(c, goal.get());
        order.attached = goal->address;
        order.goal = std::move(goal);
    }
}

void attach_circle_goal(MotionContext& c, MissionOrder& order, const std::array<std::int32_t, 3>& pos, std::int32_t radius) {
    Unit& u = *order.unit;
    if (c.type(u).type_flags & motion_flags::kTypeFlies) {
        if (c.tick(u).has_motion && order.attached) {
            c.motion(u).goal->attach(c, nullptr);
            delete_order_goal(c, order);
        }
        return;
    }
    attach_order_goal(c, order, make_circle_goal(c, order, pos[0], pos[2], radius));
}

void attach_ring_goal(MotionContext& c, MissionOrder& order, const std::array<std::int32_t, 3>& pos, std::int32_t outer,
                      std::int32_t inner) {
    Unit& u = *order.unit;
    if (c.type(u).type_flags & motion_flags::kTypeFlies) {
        if (c.tick(u).has_motion && order.attached) {
            c.motion(u).goal->attach(c, nullptr);
            delete_order_goal(c, order);
        }
        return;
    }
    attach_order_goal(c, order, make_ring_goal(c, order, pos[0], pos[2], outer, inner));
}

void attach_edge_goal(MotionContext& c, MissionOrder& order, std::uint32_t cell, std::uint32_t size) {
    Unit& u = *order.unit;
    if (c.type(u).type_flags & motion_flags::kTypeFlies) {
        if (c.tick(u).has_motion && order.attached) {
            c.motion(u).goal->attach(c, nullptr);
            delete_order_goal(c, order);
        }
        return;
    }
    attach_order_goal(c, order, make_edge_goal(c, order, cell, size));
}

void release_order_goal(MotionContext& c, Unit& u, MissionOrder& order) {
    MotionGoal* mg = c.motion(u).goal.get();
    if (mg && mg->attached && order.attached && mg->attached == order.goal.get()) mg->attach(c, nullptr);
    if (order.attached) delete_order_goal(c, order);
}

} // namespace sim
