#include "sim/ai.h"

#include <cmath>
#include <cstring>

#include "sim/trig.h"
#include "sim/x87.h"

namespace sim {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using Pos = std::array<i32, 3>;

i32 wadd(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
i32 wsub(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
i32 wmul(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) * static_cast<u32>(b)); }
i32 wshl(i32 a, int n) { return static_cast<i32>(static_cast<u32>(a) << n); }
i32 hi16(i32 v) { return static_cast<i16>(static_cast<u32>(v) >> 16); }
// High dword of the 64-bit square (whole world units squared, for 16.16 offsets).
i32 square_hi(i32 v) { return static_cast<i32>(static_cast<u32>(static_cast<std::uint64_t>(static_cast<i64>(v) * v) >> 32)); }
// The low dword of a 64-bit product shifted right by 16 (fixed-point scaling).
i32 scale16(i32 v, i32 s) { return static_cast<i32>(static_cast<u32>(static_cast<std::uint64_t>((static_cast<i64>(v) * s) >> 16))); }
// Length of an integer vector, truncated (x87 at 53 bits: squares and sums rounded, then sqrt).
i32 length3(i32 dx, i32 dy, i32 dz) {
    const double x = dx, y = dy, z = dz;
    return x87::ftol32(std::sqrt((x * x + y * y) + z * z));
}
// C division toward zero by 2 (and the shift-based forms the compiler emits).
i32 half(i32 v) { return v / 2; }

constexpr u32 kAliveFlag = unit_flags::kAlive, kDying = 0x4000u, kStructure = 0x20000000u, kArmed = 0x80000000u;
constexpr u32 kAssignable = 0x20u;  // set at creation, cleared at death
constexpr u32 kBuilderType = 0x40u, kAirBaseType = 0x200u, kFlierType = 0x800u, kDownloadable = 0x20u;
constexpr u32 kCanAttack = 0x10u, kNoBuild = 0x100u, kCanCapture = 0x1000u;
// Intents (sim/targeting.h).
constexpr u8 kMove = 2, kAttack = 3, kPatrol = 9, kBuild = 14;

void set_fault(Ai& ai, const char* what) {
    if (ai.fault.empty()) ai.fault = what;
}

const AiTypeInfo& type_of(Ai& ai, std::size_t type) {
    static const AiTypeInfo kNone{};
    if (type >= ai.types.size()) {
        set_fault(ai, "unit type outside the type table");
        return kNone;
    }
    return ai.types[type];
}
const AiTypeInfo& type_of(Ai& ai, const Unit& u) { return type_of(ai, ai.ticks[u.id].type_index); }

AiTable* table_of(Ai& ai, int side) {
    if (side < 0 || side >= 10 || !ai.state.tables[static_cast<std::size_t>(side)].exists) {
        set_fault(ai, "computer player without a unit table");
        return nullptr;
    }
    return &ai.state.tables[static_cast<std::size_t>(side)];
}

std::vector<Unit*>* group_list(Ai& ai, int player, i32 group) {
    if (player < 0 || player > 10 || group < 0 || group > 9) {
        set_fault(ai, "group outside the player's ten groups");
        return nullptr;
    }
    return &ai.state.groups[static_cast<std::size_t>(player)][static_cast<std::size_t>(group)];
}

// A group's centre: the mean of the whole world units (16.16 result); false when empty.
bool group_centre(const std::vector<Unit*>& list, Pos& out) {
    if (list.empty()) return false;
    const i32 n = static_cast<i32>(list.size());
    i32 sx = 0, sy = 0, sz = 0;
    for (const Unit* u : list) {
        sx = wadd(sx, hi16(u->pos[0]));
        sy = wadd(sy, hi16(u->pos[1]));
        sz = wadd(sz, hi16(u->pos[2]));
    }
    out = {wshl(sx / n, 16), wshl(sy / n, 16), wshl(sz / n, 16)};
    return true;
}

// The original's random draw (sim/rng.h): its argument is a signed int in the original.
u32 rnd(Ai& ai, i32 n) { return ai.rng.next_mod(static_cast<u32>(n)); }

i32 world_width(const Ai& ai) { return ai.env.map_width * 16; }
i32 world_height(const Ai& ai) { return ai.env.map_height * 16; }

// The order routines: per unit, the intent's mission kind, then the general order routine.
void order(Ai& ai, u8 intent, bool queued, Unit& u, Unit* target, const Pos* pos, i32 p0, i32 p1) {
    const u8 kind = ai.systems.resolve(intent, u, target, pos);
    ai.systems.enqueue(kind, queued, u, target, pos, p0, p1);
}

// --- Roles ----------------------------------------------------------------

void run_factories(Ai& ai, AiController& c, AiBehaviour& b) {
    b.next_tick = ai.env.game_tick + 30;
    std::vector<Unit*>* list = group_list(ai, c.player, b.group);
    if (!list) return;
    for (std::size_t i = 0; i < list->size(); ++i) {
        Unit& u = *(*list)[i];
        const u32 f = u.flags;
        if (!(f & kStructure) || !(f & kAliveFlag) || (f & kDying)) continue;
        const AiTypeInfo& t = type_of(ai, u);
        if (t.makes_metal) {
            ai_switch_by_economy(ai, u);
        } else if (!t.build_options.empty() && !u.orders) {
            const u16 type = ai_pick_build_type(ai, c.side, u);
            if (type) ai.systems.build_menu(u, type, type_of(ai, type).weapon_menu_name);
        }
    }
}

// Strike groups hand far units to their reserve and take reserve units close to their
// centre (0x407560). The centre is of whole world units (x and z).
void rebalance(Ai& ai, AiController& c, AiBehaviour& b) {
    const i32 peer_group = b.peer;
    if (peer_group < 0 || peer_group > 9) {
        set_fault(ai, "strike group's partner outside the roles");
        return;
    }
    AiBehaviour& p = c.roles[static_cast<std::size_t>(peer_group)];
    if (&p == &b) return;
    std::vector<Unit*>* own = group_list(ai, c.player, b.group);
    std::vector<Unit*>* other = group_list(ai, c.player, p.group);
    if (!own || !other) return;
    const i32 limit = b.peer_distance;
    if (own->empty()) {
        if (other->empty()) return;
        set_unit_group(ai, *other->front(), b.group);
    }
    i32 sx = 0, sz = 0;
    for (const Unit* u : *own) {
        sx = wadd(sx, hi16(u->pos[0]));
        sz = wadd(sz, hi16(u->pos[2]));
    }
    i32 cx = 0, cz = 0;
    for (;;) {
        const i32 n = static_cast<i32>(own->size());
        cx = sx / n;
        cz = sz / n;
        if (n == 1) break;
        i32 far = 0;
        std::size_t at = own->size();
        for (std::size_t k = 0; k < own->size(); ++k) {
            const i32 dx = wsub(hi16((*own)[k]->pos[0]), cx), dz = wsub(hi16((*own)[k]->pos[2]), cz);
            const i32 d = wadd(wmul(dx, dx), wmul(dz, dz));
            if (d > far) {
                far = d;
                at = k;
            }
        }
        if (far < wmul(limit, n)) break;
        if (at >= own->size()) {
            set_fault(ai, "strike group hand-over past its list");
            return;
        }
        Unit* moved = (*own)[at];
        set_unit_group(ai, *moved, peer_group);
        // The original subtracts whatever the vacated entry now holds: the unit swapped in
        // from the end (or the moved unit itself when it was last).
        const Unit* now = at < own->size() ? (*own)[at] : moved;
        sx = wsub(sx, hi16(now->pos[0]));
        sz = wsub(sz, hi16(now->pos[2]));
    }
    std::vector<Unit*> join;
    for (std::size_t k = 0; k < other->size(); ++k) {
        Unit* u = (*other)[k];
        const i32 dz = wsub(hi16(u->pos[2]), cz), dx = wsub(hi16(u->pos[0]), cx);
        if (wadd(wmul(dx, dx), wmul(dz, dz)) < wmul(limit, static_cast<i32>(own->size()))) join.push_back(u);
    }
    for (Unit* u : join) set_unit_group(ai, *u, b.group);
}

// Centre of the base: armed structures, else unarmed structures, else constructors.
bool base_centre(Ai& ai, AiController& c, Pos& out) {
    for (i32 g : {5, 1, 4}) {
        std::vector<Unit*>* l = group_list(ai, c.player, c.roles[static_cast<std::size_t>(g)].group);
        if (l && group_centre(*l, out)) return true;
    }
    return false;
}

void run_strike(Ai& ai, AiController& c, AiBehaviour& b) {
    b.next_tick = ai.env.game_tick + 300;
    rebalance(ai, c, b);
    if (!ai.fault.empty()) return;
    std::vector<Unit*>* list = group_list(ai, c.player, b.group);
    if (!list || list->empty()) return;
    const i32 n = static_cast<i32>(list->size());
    const bool gather = n <= b.min_size || (!b.attacking && n < b.max_size);
    if (gather) {
        Pos at{};
        if (base_centre(ai, c, at)) {
            b.attacking = false;
            ai_order_group(ai, c.player, b.group, kMove, false, nullptr, &at, 0xA0, 0);
            return;
        }
    }
    b.attacking = true;
    Pos centre{};
    group_centre(*list, centre);
    Unit* target = ai_nearest_enemy(ai, c.player, centre[0], centre[2]);
    if (target) ai_order_group(ai, c.player, b.group, kAttack, false, target, nullptr, 0, 0);
}

void run_reserve(Ai& ai, AiController& c, AiBehaviour& b) {
    b.next_tick = ai.env.game_tick + 150;
    if (b.follows < 0 || b.follows > 9) {
        set_fault(ai, "reserve following a role outside the roles");
        return;
    }
    std::vector<Unit*>* own = group_list(ai, c.player, b.group);
    std::vector<Unit*>* lead = group_list(ai, c.player, c.roles[static_cast<std::size_t>(b.follows)].group);
    if (!own || !lead || own->empty() || lead->empty()) return;
    Pos at{};
    if (group_centre(*lead, at)) ai_order_group(ai, c.player, b.group, kMove, false, nullptr, &at, 0, 0);
}

void run_constructors(Ai& ai, AiController& c, AiBehaviour& b) {
    b.next_tick = ai.env.game_tick + 90;
    AiTable* tb = table_of(ai, c.side);
    std::vector<Unit*>* list = group_list(ai, c.player, b.group);
    if (!tb || !list) return;
    // The base centre and the site, as the original's locals: the centre's height is
    // overwritten along the way, the site's height is never written (an input).
    Pos anchor = tb->anchor;
    Pos site{0, ai.systems.stale_site_height(c.player, ai.env.stale_site_height), 0};
    const u32 hold = ai.targeting.controllers[static_cast<std::size_t>(c.player)].ai_hold_tick;
    for (std::size_t i = 0; i < list->size(); ++i) {
        Unit& u = *(*list)[i];
        const AiTypeInfo& t = type_of(ai, u);
        if (t.build_options.empty()) continue;
        const bool capture = (t.abilities & kCanCapture) != 0;
        if (capture && (tb->builders >= 5 || ai.env.game_tick < hold)) continue;
        if (u.orders && (u.orders->flags & 8u)) continue;
        const u16 type = ai_pick_build_type(ai, c.side, u);
        if (!type) continue;
        const Pos near = u.pos;
        bool ok = ai_find_site(ai, c.side, near, type, site);
        if (!ai.fault.empty()) return;
        if (capture) {
            const i32 reach = wshl((ai.env.scroll_width + ai.env.scroll_height) / 3, 16);
            const i32 d = length3(wsub(site[0], anchor[0]), 0, wsub(site[2], anchor[2]));
            anchor[1] = site[1];
            if (d > reach) ok = false;
        }
        if (ok) order(ai, kBuild, false, u, nullptr, &site, type, 1);
    }
    for (std::size_t i = 0; i < list->size(); ++i) {
        Unit& u = *(*list)[i];
        if (u.orders && !(u.orders->flags & order_flags::kIdle)) continue;
        const AiTypeInfo& t = type_of(ai, u);
        const bool capture = (t.abilities & kCanCapture) != 0;
        if (capture && tb->builders < 5) continue;
        site = anchor;
        if (capture) {
            anchor[1] = u.pos[1];
            i32 ox = wsub(anchor[0], u.pos[0]), oy = 0, oz = wsub(anchor[2], u.pos[2]);
            const i32 d = length3(ox, oy, oz);
            if (d > 0x2800000) {
                const u32 r = rnd(ai, 0x10000);
                ox = -trig::mul_sin(static_cast<Angle>(r), 0x2800000);
                oy = 0;
                oz = -trig::mul_cos(static_cast<Angle>(r), 0x2800000);
            }
            site = {wadd(ox, anchor[0]), wadd(oy, anchor[1]), wadd(oz, anchor[2])};
            order(ai, kMove, false, u, nullptr, &site, 0, 0);
            order(ai, kPatrol, true, u, nullptr, &anchor, 0, 0);
        } else {
            const i32 dx = wsub(anchor[0], u.pos[0]), dy = wsub(anchor[1], u.pos[1]), dz = wsub(anchor[2], u.pos[2]);
            const i32 d = length3(dx, dy, dz);
            if (d < 0x1400000) {
                i32 ox = 0, oy = 0, oz = 0;
                if (d < 0x100000) {
                    const u32 r = rnd(ai, 0x10000);
                    ox = -trig::mul_sin(static_cast<Angle>(r), 0x1400000);
                    oz = -trig::mul_cos(static_cast<Angle>(r), 0x1400000);
                } else {
                    // Scaled to 0x1400000 (20 world units) along the offset.
                    const i32 s = static_cast<i32>(static_cast<u32>(static_cast<std::uint64_t>((static_cast<i64>(0x140) << 32) / d)));
                    ox = scale16(dx, s);
                    oy = scale16(dy, s);
                    oz = scale16(dz, s);
                }
                site = {wadd(ox, u.pos[0]), wadd(u.pos[1], oy), wadd(u.pos[2], oz)};
            }
            order(ai, kPatrol, false, u, nullptr, &site, 0, 0);
        }
    }
}

void run_aircraft(Ai& ai, AiController& c, AiBehaviour& b) {
    const u32 r0 = rnd(ai, 900);
    b.next_tick = ai.env.game_tick + r0 + 30;
    std::vector<Unit*>* list = group_list(ai, c.player, b.group);
    AiTable* tb = table_of(ai, c.side);
    if (!list || !tb) return;
    if (list->size() < 5) {
        const Pos anchor = tb->anchor;
        if (hi16(anchor[0]) != 0 || hi16(anchor[2]) != 0) {
            const i32 n = static_cast<i32>(rnd(ai, 2)) + 2;
            const i32 w8 = world_width(ai) / 8, h8 = world_height(ai) / 8;
            for (i32 k = 0; k < n; ++k) {
                const i32 rx = static_cast<i32>(rnd(ai, w8));
                const i32 rz = static_cast<i32>(rnd(ai, h8));
                const Pos at{wadd(anchor[0], wshl(rx - half(w8), 16)), anchor[1], wadd(anchor[2], wshl(rz - half(h8), 16))};
                ai_order_group(ai, c.player, b.group, k ? kPatrol : kMove, k != 0, nullptr, &at, 0, 0);
            }
            return;
        }
        // No base centre yet: patrol to the nearest enemy of the group's centre. With an empty
        // group the centre is stale stack data, but then nobody receives the order.
        if (list->empty()) return;
        Pos centre{};
        group_centre(*list, centre);
        Unit* e = ai_nearest_enemy(ai, c.player, centre[0], centre[2]);
        if (!e) {
            set_fault(ai, "aircraft ordered to the position of no enemy (original crash)");
            return;
        }
        const Pos at = e->pos;
        ai_order_group(ai, c.player, b.group, kPatrol, true, nullptr, &at, 0, 0);
        return;
    }
    // Five or more: patrol to a random point on a random map edge.
    Pos at{0, 0, 0};
    const i32 W = world_width(ai), H = world_height(ai);
    if (rnd(ai, 2) != 0) {
        at[0] = wshl(static_cast<i32>(rnd(ai, W)), 16);
        at[2] = rnd(ai, 2) != 0 ? 0 : wshl(wadd(H, 0xFFFF), 16);
    } else {
        at[0] = rnd(ai, 2) != 0 ? 0 : wshl(wadd(W, 0xFFFF), 16);
        at[2] = wshl(static_cast<i32>(rnd(ai, H)), 16);
    }
    ai_order_group(ai, c.player, b.group, kPatrol, false, nullptr, &at, 0, 0);
}

void run_hunter(Ai& ai, AiController& c, AiBehaviour& b) {
    const u32 r0 = rnd(ai, 150);
    b.next_tick = ai.env.game_tick + r0 + 30;
    std::vector<Unit*>* list = group_list(ai, c.player, b.group);
    if (!list || list->empty()) return;
    if (rnd(ai, 10) == 0) {
        b.probe = b.best;
        const u32 r = rnd(ai, 0x10000);
        b.step = {-trig::mul_sin(static_cast<Angle>(r), 0x1400000), 0, -trig::mul_cos(static_cast<Angle>(r), 0x1400000)};
    }
    for (int k = 0; k < 3; ++k) b.probe[static_cast<std::size_t>(k)] = wadd(b.probe[static_cast<std::size_t>(k)], b.step[static_cast<std::size_t>(k)]);
    if (ai.systems.point_in_sight(c.player, b.probe)) {
        const i32 s = ai_threat_near(ai, c.side, b.probe, 0xA0);
        const i32 a = static_cast<i32>(rnd(ai, b.best_threat));
        const i32 v = static_cast<i32>(rnd(ai, s));
        if (v > a) {
            b.best_threat = s;
            b.best = b.probe;
        }
    }
    for (std::size_t i = 0; i < list->size(); ++i) {
        Unit& u = *(*list)[i];
        if (!(type_of(ai, u).abilities & kCanAttack)) continue;
        if (!ai.ticks[u.id].has_motion && !ai.systems.weapon_in_range(u, u.pos, b.best, 0)) continue;
        const Pos at = b.best;
        const u8 kind = ai.systems.resolve(kAttack, u, nullptr, &at);
        if (kind) ai.systems.enqueue(kind, false, u, nullptr, &at, 0, 0);
    }
}

// --- Sites ------------------------------------------------------------------

// STL heap of candidate sites by score (the highest first): the original's sift-down
// (prefers the left child on ties) and sift-up.
void heap_adjust(std::vector<AiMetalSite>& v, i32 hole, i32 len, AiMetalSite value) {
    const i32 top = hole;
    i32 child = 2 * hole + 2;
    while (child < len) {
        if (v[static_cast<std::size_t>(child)].score < v[static_cast<std::size_t>(child - 1)].score) --child;
        v[static_cast<std::size_t>(hole)] = v[static_cast<std::size_t>(child)];
        hole = child;
        child = 2 * child + 2;
    }
    if (child == len) {
        v[static_cast<std::size_t>(hole)] = v[static_cast<std::size_t>(child - 1)];
        hole = child - 1;
    }
    i32 parent = (hole - 1) / 2;
    while (hole > top && v[static_cast<std::size_t>(parent)].score < value.score) {
        v[static_cast<std::size_t>(hole)] = v[static_cast<std::size_t>(parent)];
        hole = parent;
        parent = (hole - 1) / 2;
    }
    v[static_cast<std::size_t>(hole)] = value;
}

// Footprint corner cell of a site centred at a 16.16 position.
i16 corner_cell(i32 centre, i16 footprint) { return static_cast<i16>(wadd(wsub(centre, wshl(footprint, 19)), 0x80000) >> 20); }

// The metal spot nearest the centre (and then the richest within 160 more), 0x40A260.
bool best_metal_site(Ai& ai, AiTable& tb, std::uint16_t type, const Pos& centre, i32 max_dist, i16& cell_x, i16& cell_z) {
    if (tb.metal_sites.empty()) return false;
    const AiTypeInfo& t = type_of(ai, type);
    const i16 cx = corner_cell(centre[0], t.footprint_x), cz = corner_cell(centre[2], t.footprint_z);
    const i32 r2 = wmul(max_dist, max_dist);
    std::vector<AiMetalSite> v;
    for (const AiMetalSite& s : tb.metal_sites) {
        const i32 dz = s.cell_z - cz, dx = s.cell_x - cx;
        const i32 d = wadd(wmul(dx, dx), wmul(dz, dz));
        if (d <= r2) {
            AiMetalSite c = s;
            c.score = static_cast<float>(wsub(0, d));
            v.push_back(c);
        }
    }
    if (v.size() >= 2) {
        const i32 len = static_cast<i32>(v.size());
        for (i32 parent = len / 2; parent > 0;) {
            --parent;
            heap_adjust(v, parent, len, v[static_cast<std::size_t>(parent)]);
        }
    }
    i32 best_d = -1, best_metal = 0;
    u32 best_cell = 0;
    while (!v.empty()) {
        const i16 x = static_cast<i16>(v.front().cell_x - static_cast<i16>((t.footprint_x - 3) / 2));
        const i16 z = static_cast<i16>(v.front().cell_z - static_cast<i16>((t.footprint_z - 3) / 2));
        const i32 dx = x - cx, dz = z - cz;
        const i32 d = wadd(wmul(dx, dx), wmul(dz, dz));
        if (best_d >= 0 && d > wadd(best_d, 0xA0)) break;
        if (ai.systems.structure_fits(type, x, z)) {
            const i32 m = ai.systems.last_place_metal();
            if (m > best_metal) {
                best_cell = static_cast<u16>(x) | static_cast<u32>(static_cast<u16>(z)) << 16;
                best_metal = ai.systems.last_place_metal();
                if (best_d == -1) best_d = d;
            }
        }
        // Pop: the last entry is sifted down from the top.
        const AiMetalSite last = v.back();
        const i32 len = static_cast<i32>(v.size()) - 1;
        v.back() = v.front();
        heap_adjust(v, 0, len, last);
        v.pop_back();
    }
    if (best_metal == 0) return false;
    cell_x = static_cast<i16>(best_cell & 0xFFFF);
    cell_z = static_cast<i16>(best_cell >> 16);
    return true;
}

// Thirty random tries on the table's site grid around the centre (0x40A5D0).
bool random_site(Ai& ai, AiTable& tb, std::uint16_t type, const Pos& centre, i32 radius, i16& cell_x, i16& cell_z) {
    const AiTypeInfo& t = type_of(ai, type);
    const i32 metal_limit = wshl(wmul(wmul(ai.env.surface_metal, t.footprint_z), t.footprint_x), 1);
    const bool water = t.min_water_depth >= 0;
    const i16 w = water ? tb.water_w : tb.land_w, h = water ? tb.water_h : tb.land_h;
    const i16 ox = water ? tb.water_dx : tb.land_dx, oz = water ? tb.water_dz : tb.land_dz;
    const i32 margin = water ? tb.water_margin : tb.land_margin;
    for (int k = 0; k < 30; ++k) {
        const i32 dist = wshl(static_cast<i32>(rnd(ai, radius)), 16);
        const u32 a = rnd(ai, 0x10000);
        const i32 x = wadd(-trig::mul_sin(static_cast<Angle>(a), dist), centre[0]);
        const i32 z = wadd(-trig::mul_cos(static_cast<Angle>(a), dist), centre[2]);
        const i16 cx0 = corner_cell(x, t.footprint_x), cz0 = corner_cell(z, t.footprint_z);
        if (w == 0 || h == 0) {
            set_fault(ai, "site grid of size zero (division by zero)");
            return false;
        }
        const u32 rx = rnd(ai, w - margin - t.footprint_x);
        const i16 sx = static_cast<i16>(static_cast<i16>(cx0 / w) * w + ox + static_cast<i32>(rx));
        const u32 rz = rnd(ai, h - margin - t.footprint_z);
        const i16 sz = static_cast<i16>(static_cast<i16>(cz0 / h) * h + oz + static_cast<i32>(rz));
        if (ai.systems.can_place(type, sx, sz) && ai.systems.last_place_metal() <= metal_limit) {
            cell_x = sx;
            cell_z = sz;
            return true;
        }
    }
    return false;
}

void make_site_grid(Ai& ai, i16& w, i16& h, i16& dx, i16& dz, i32& margin, i32 m, i32 wide) {
    margin = m;
    w = static_cast<i16>(static_cast<i32>(rnd(ai, wide)) + margin + 8);
    h = static_cast<i16>(static_cast<i32>(rnd(ai, 3)) + margin + 8);
    dx = static_cast<i16>(static_cast<i32>(rnd(ai, w)) - half(w));
    dz = static_cast<i16>(static_cast<i32>(rnd(ai, h)) - half(h));
}

// Weighted chance of a type as a float sum of x and w scaled by -1/65536, as the original.
float less_scaled(float sum, i32 v, i32 w) {
    return static_cast<float>(static_cast<double>(sum) - static_cast<double>(v) * static_cast<double>(w) * -1.52587890625e-05);
}

}  // namespace

// --- Groups ----------------------------------------------------------------

void set_unit_group(Ai& ai, Unit& unit, std::int32_t group) {
    const int owner = ai.ticks[unit.id].owner;
    const i32 old = static_cast<i32>(unit.group);
    if (old != -1) {
        std::vector<Unit*>* l = group_list(ai, owner, old);
        if (!l) return;
        for (std::size_t k = 0; k < l->size(); ++k)
            if ((*l)[k] == &unit) {
                (*l)[k] = l->back();
                l->pop_back();
                break;
            }
    }
    if (group != -1) {
        std::vector<Unit*>* l = group_list(ai, owner, group);
        if (!l) return;
        l->push_back(&unit);
    }
    unit.group = static_cast<u32>(group);
}

// --- Battle start -------------------------------------------------------------

void ai_reset_profile(Ai& ai, int table) {
    if (table < 0 || table >= 10) return;
    AiTable& tb = ai.state.tables[static_cast<std::size_t>(table)];
    const std::size_t n = ai.types.size();
    tb.anchor_weight.assign(n, 0);
    tb.counts.assign(n, 0);
    tb.weight.assign(n, 100);
    tb.weight_fixed.assign(n, 0);
    tb.limit.assign(n, -1);
    tb.limit_fixed.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const AiTypeInfo& t = ai.types[i];
        i32 w = 0;
        if (t.structure) w += 40;
        if (t.has_build_options) w += 20;
        tb.anchor_weight[i] = static_cast<std::int8_t>(w);
    }
}

void ai_create_player(Ai& ai, int player) {
    if (player < 0 || player >= 10) return;
    for (auto& g : ai.state.groups[static_cast<std::size_t>(player)]) g.clear();
    const PlayerSlot& p = ai.players[static_cast<std::size_t>(player)];
    if (p.active && p.controller == 3) return;
    AiController& c = ai.state.controllers[static_cast<std::size_t>(player)];
    c = AiController{};
    c.exists = true;
    c.player = player;
    c.side = p.state;
    c.regroup_countdown = 30;
    auto role = [&](int g, AiRole r) -> AiBehaviour& {
        AiBehaviour& b = c.roles[static_cast<std::size_t>(g)];
        b = AiBehaviour{};
        b.role = r;
        b.group = g;
        return b;
    };
    role(1, AiRole::Factories);
    role(4, AiRole::Constructors);
    role(5, AiRole::Armed);
    AiBehaviour& s2 = role(2, AiRole::Strike);
    s2.min_size = 3;
    s2.max_size = 6;
    s2.peer = 3;
    s2.peer_distance = 20000;
    role(3, AiRole::Reserve).follows = 2;
    AiBehaviour& s6 = role(6, AiRole::Strike);
    s6.min_size = 3;
    s6.max_size = 6;
    s6.peer = 7;
    s6.peer_distance = 50000;
    role(7, AiRole::Reserve).follows = 6;
    role(8, AiRole::Aircraft);
    AiBehaviour& h = role(9, AiRole::Hunter);
    const Pos mid{x87::ftol32(static_cast<double>(half(world_width(ai))) * 65536.0), 0,
                  x87::ftol32(static_cast<double>(half(world_height(ai))) * 65536.0)};
    h.best = mid;
    h.probe = mid;
    h.step = mid;
    h.best_threat = 0;
    // The table, under the player's state index.
    const int ti = p.state;
    if (ti >= 10) {
        set_fault(ai, "unit table index past the table array");
        return;
    }
    AiTable& tb = ai.state.tables[static_cast<std::size_t>(ti)];
    tb = AiTable{};
    tb.exists = true;
    tb.player = ti;
    tb.centre_cell_x = static_cast<i16>(ai.env.map_width / 2);
    tb.centre_cell_z = static_cast<i16>(ai.env.map_height / 2);
    make_site_grid(ai, tb.land_w, tb.land_h, tb.land_dx, tb.land_dz, tb.land_margin, 3, 10);
    make_site_grid(ai, tb.water_w, tb.water_h, tb.water_dx, tb.water_dz, tb.water_margin, 6, 0x14);
    ai_reset_profile(ai, ti);
    tb.threat.assign(ai.types.size(), 0);
    tb.build_scores.assign(ai.types.size(), {0, 0, 0});
    ai_rescore(ai, ti);
}

void ai_collect_metal_sites(Ai& ai) {
    for (int s = 0; s < 10; ++s) {
        if (!ai.state.controllers[static_cast<std::size_t>(s)].exists) continue;
        AiTable* tb = table_of(ai, s);
        if (!tb) return;
        tb->metal_sites.clear();
        for (i32 z = 0; z < ai.map.height; ++z)
            for (i32 x = 0; x < ai.map.width; ++x) {
                const MapCell& cell = ai.map.cells[static_cast<std::size_t>(z) * static_cast<std::size_t>(ai.map.width) + static_cast<std::size_t>(x)];
                if (cell.feature >= 0xFFFB) continue;
                if (cell.feature >= ai.features.size()) {
                    set_fault(ai, "feature type outside the feature table");
                    return;
                }
                const FeatureTypeInfo& f = ai.features[cell.feature];
                if (!(f.metal < 0.0f || f.metal > 0.0f)) continue;
                if (!(f.flags & 0x200u)) continue;
                tb->metal_sites.push_back({static_cast<i16>(x), static_cast<i16>(z), f.metal});
            }
    }
}

// --- Profile ------------------------------------------------------------------

void ai_profile_plan(Ai& ai, const std::vector<std::string>& words) {
    auto same = [](const std::string& a, const char* b) {
        std::size_t i = 0;
        for (; i < a.size() && b[i]; ++i) {
            const auto la = static_cast<unsigned char>(a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i]);
            const auto lb = static_cast<unsigned char>(b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i]);
            if (la != lb) return false;
        }
        return i == a.size() && !b[i];
    };
    ai.state.profile_gate = false;
    for (std::size_t i = 1; i < words.size(); ++i) {
        if (same(words[1], "any")) ai.state.profile_gate = true;  // (the original tests the first word each time)
        if (ai.env.difficulty == 0 && same(words[i], "easy")) ai.state.profile_gate = true;
        if (ai.env.difficulty == 1 && same(words[i], "medium")) ai.state.profile_gate = true;
        if (ai.env.difficulty == 2 && same(words[i], "hard")) ai.state.profile_gate = true;
    }
}

void ai_profile_force(Ai& ai) { ai.state.profile_gate = true; }

void ai_profile_weight(Ai& ai, const CategoryMask& mask, float factor, bool fixed) {
    if (!ai.state.profile_gate) return;
    for (int s = 0; s < 10; ++s) {
        if (!ai.state.controllers[static_cast<std::size_t>(s)].exists) continue;
        AiTable* tb = table_of(ai, s);
        if (!tb) return;
        for (std::size_t i = 1; i < ai.types.size() && i < tb->weight.size(); ++i) {
            if (!((mask[(i >> 5) & 15] >> (i & 31)) & 1u) || tb->weight_fixed[i] != 0) continue;
            const i32 v = x87::ftol32(static_cast<double>(tb->weight[i]) * static_cast<double>(factor));
            const i32 c = v <= 0 ? 0 : v;
            tb->weight[i] = static_cast<u8>(c < 100 ? c : 100);
            if (fixed) tb->weight_fixed[i] = 1;
        }
    }
}

void ai_profile_limit(Ai& ai, const CategoryMask& mask, std::int32_t limit, bool fixed) {
    if (!ai.state.profile_gate) return;
    for (int s = 0; s < 10; ++s) {
        const PlayerSlot& p = ai.players[static_cast<std::size_t>(s)];
        if (!p.active || p.controller != 2) continue;
        AiTable* tb = table_of(ai, s);
        if (!tb) return;
        for (std::size_t i = 1; i < ai.types.size() && i < tb->limit.size(); ++i) {
            if (!((mask[(i >> 5) & 15] >> (i & 31)) & 1u) || tb->limit_fixed[i] != 0) continue;
            tb->limit[i] = limit;
            if (fixed) tb->limit_fixed[i] = 1;
        }
    }
}

// --- Unit table ---------------------------------------------------------------

void ai_table_begin(Ai& ai, int table) {
    if (table < 0 || table >= 10) return;
    AiTable& tb = ai.state.tables[static_cast<std::size_t>(table)];
    tb.air_bases.clear();
    tb.builders = 0;
    for (auto& n : tb.counts) n = 0;
    tb.sum_x = tb.sum_y = tb.sum_z = tb.sum_w = 0.0f;
}

void ai_table_own_unit(Ai& ai, int table, Unit& u) {
    if (table < 0 || table >= 10) return;
    AiTable& tb = ai.state.tables[static_cast<std::size_t>(table)];
    const std::size_t type = ai.ticks[u.id].type_index;
    if (type >= tb.counts.size()) {
        set_fault(ai, "unit type outside the table");
        return;
    }
    tb.counts[type] = static_cast<std::int16_t>(tb.counts[type] + 1);
    const AiTypeInfo& t = type_of(ai, type);
    if (t.has_build_options) ++tb.builders;
    if ((t.type_flags & kBuilderType) && (t.type_flags & kAirBaseType) && (u.activate_flags & unit_flags::kActivated))
        tb.air_bases.push_back(&u);
    const i32 w = tb.anchor_weight[type];
    tb.sum_w = static_cast<float>(static_cast<double>(w) + static_cast<double>(tb.sum_w));
    tb.sum_x = less_scaled(tb.sum_x, u.pos[0], w);
    tb.sum_y = less_scaled(tb.sum_y, u.pos[1], w);
    tb.sum_z = less_scaled(tb.sum_z, u.pos[2], w);
}

void ai_table_end(Ai& ai, int table) {
    if (table < 0 || table >= 10) return;
    AiTable& tb = ai.state.tables[static_cast<std::size_t>(table)];
    if (tb.sum_w < 0.0f || tb.sum_w > 0.0f) {
        tb.sum_x = static_cast<float>(static_cast<double>(tb.sum_x) / static_cast<double>(tb.sum_w));
        tb.sum_y = static_cast<float>(static_cast<double>(tb.sum_y) / static_cast<double>(tb.sum_w));
        tb.sum_z = static_cast<float>(static_cast<double>(tb.sum_z) / static_cast<double>(tb.sum_w));
    }
    tb.anchor = {x87::ftol32(static_cast<double>(tb.sum_x) * 65536.0), x87::ftol32(static_cast<double>(tb.sum_y) * 65536.0),
                 x87::ftol32(static_cast<double>(tb.sum_z) * 65536.0)};
}

void ai_rescore(Ai& ai, int table) {
    if (table < 0 || table >= 10 || !ai.state.tables[static_cast<std::size_t>(table)].exists) {
        set_fault(ai, "type scores of a missing table");
        return;
    }
    AiTable& tb = ai.state.tables[static_cast<std::size_t>(table)];
    const std::size_t n = ai.types.size();
    tb.threat.resize(n, 0);
    tb.build_scores.resize(n, {0, 0, 0});
    if (tb.counts.size() < n) tb.counts.resize(n, 0);
    const PlayerSlot& owner = ai.players[static_cast<std::size_t>(tb.player)];
    // The type's energy rate: its use, else minus its wind or tidal output (0x488F30).
    auto energy_rate = [&](const AiTypeInfo& t) -> double {
        if (t.energy_use < 0.0f || t.energy_use > 0.0f) return t.energy_use;
        if (t.wind_generator > 0.0f) return -(static_cast<double>(ai.env.wind_fraction) * static_cast<double>(t.wind_generator));
        if (t.tidal_generator > 0.0f) return -(static_cast<double>(ai.env.tidal_strength) * static_cast<double>(t.tidal_generator));
        return 0.0;
    };
    auto is_set = [](float f) { return f < 0.0f || f > 0.0f; };
    for (std::size_t i = 1; i < n; ++i) {
        const AiTypeInfo& t = ai.types[i];
        // Threat: production and cost, then weapons.
        i32 base = is_set(t.extracts_metal) ? 11 : 1;
        if (t.makes_metal) base += 10;
        if (!(energy_rate(t) >= 0.0)) base += 10;
        const i32 c1 = x87::ftol32(static_cast<double>(base) - static_cast<double>(t.build_cost_metal) * static_cast<double>(-0.01f));
        const i32 cost = x87::ftol32(static_cast<double>(c1) - static_cast<double>(t.build_cost_energy) * static_cast<double>(-0.002f));
        i32 arms = (t.abilities & kCanAttack) ? 11 : 1;
        for (const WeaponType* w : t.weapons) {
            if (!w) {
                set_fault(ai, "unit type with a missing weapon definition");
                return;
            }
            if (w->index) arms = arms + w->range / 100 + static_cast<i32>(w->default_damage) / 40 + 5;
        }
        const i32 arms_c = arms > 100 ? 100 : (arms < -100 ? -100 : arms);
        const i32 sum = cost + static_cast<std::int8_t>(arms_c);
        tb.threat[i] = static_cast<std::int8_t>(sum > 100 ? 100 : (sum < -100 ? -100 : sum));
        // Build desire.
        const i32 have = tb.counts[i];
        i32 want = (t.abilities & kCanAttack) ? 21 : 1;
        if ((t.type_flags & kBuilderType) && have < 3) want += 30;
        if (!(energy_rate(t) >= 0.0)) want += 50;
        if (is_set(t.extracts_metal)) want += 50;
        if (t.makes_metal) want += 25;
        if (t.type_flags & kFlierType) want += 40;
        if (t.sonar_distance) want += 15;
        if (t.radar_distance) want += 5;
        float make = t.energy_make > 0.0f ? t.energy_make : 0.0f;
        if (!(make < 30.0f) && !std::isnan(make)) make = 30.0f;
        i32 d = x87::ftol32(static_cast<double>(want) + static_cast<double>(make));
        if (have == 0) d = wshl(d, 2);
        if (have == 1) d = wadd(d, d);
        if (t.min_water_depth >= 0) d = wmul(d, 3);
        if (owner.alive_count > static_cast<u16>(ai.env.units_per_player >> 1)) d = wadd(d, tb.threat[i] / 2);
        if (t.abilities & kNoBuild) d = 0;
        if (t.type_flags & 0x1000000u) d = 0;
        if (is_set(t.wind_generator) && ai.env.wind_max_speed < ai.env.wind_denominator / 2) d = 0;
        if (d >= 100) d = 100;
        tb.build_scores[i][0] = static_cast<std::int8_t>(d);
        // Energy: cheap producers score high.
        const double ex = static_cast<double>(static_cast<float>(static_cast<double>(t.build_cost_energy) * static_cast<double>(-0.0025f)));
        double ev = ex - energy_rate(t) * 5.0;
        if (!(ev <= 100.0)) ev = 100.0;
        if (!(ev >= 0.0) && !std::isnan(ev)) ev = 0.0;
        tb.build_scores[i][2] = static_cast<std::int8_t>(x87::ftol32(ev));
        // Metal: extractors and makers, minus cost.
        const i32 m = is_set(t.extracts_metal) ? 100 : 0;
        const float mq = static_cast<float>(static_cast<double>(t.build_cost_metal) * static_cast<double>(-0.02f) + (t.makes_metal ? 25.0 : 0.0));
        double mv = static_cast<double>(m) + static_cast<double>(mq);
        if (!(mv <= 100.0)) mv = 100.0;
        if (!(mv >= 0.0) && !std::isnan(mv)) mv = 0.0;
        tb.build_scores[i][1] = static_cast<std::int8_t>(x87::ftol32(mv));
    }
}

// --- The controller -----------------------------------------------------------

void ai_assign_groups(Ai& ai, int player) {
    const PlayerSlot& p = ai.players[static_cast<std::size_t>(player)];
    for (int id = p.first_unit; id <= p.last_unit; ++id) {
        if (id < 0 || static_cast<std::size_t>(id) >= ai.units.size()) {
            set_fault(ai, "player range past the unit table");
            return;
        }
        Unit& u = ai.units[static_cast<std::size_t>(id)];
        u32 f = u.flags;
        if (!(f & kAssignable)) continue;
        const AiTypeInfo& t = type_of(ai, u);
        f = (t.abilities & kCanCapture) ? ((f & 0xFFF7FFFFu) | 0x40000u) : ((f & 0xFFFBFFFFu) | 0x80000u);
        u.flags = f;
        u.flags = (f & 0xFFEFFFFFu) | 0x200000u;
        if (u.group != 0) continue;
        const u32 g = u.flags;
        i32 to;
        if (g & kStructure) {
            to = (g & kArmed) ? 5 : 1;
        } else if (t.type_flags & kBuilderType) {
            to = 4;
        } else if (t.type_flags & kFlierType) {
            to = 8;
        } else if (t.min_water_depth > 0) {
            to = 7;
        } else if (g & kArmed) {
            to = 3;
        } else {
            continue;
        }
        set_unit_group(ai, u, to);
        if (!ai.fault.empty()) return;
    }
}

void ai_run_role(Ai& ai, int player, int group) {
    AiController& c = ai.state.controllers[static_cast<std::size_t>(player)];
    AiBehaviour& b = c.roles[static_cast<std::size_t>(group)];
    switch (b.role) {
    case AiRole::Factories: run_factories(ai, c, b); break;
    case AiRole::Strike: run_strike(ai, c, b); break;
    case AiRole::Reserve: run_reserve(ai, c, b); break;
    case AiRole::Constructors: run_constructors(ai, c, b); break;
    case AiRole::Armed: break;  // armed structures: nothing (its timer is never advanced)
    case AiRole::Aircraft: run_aircraft(ai, c, b); break;
    case AiRole::Hunter: run_hunter(ai, c, b); break;
    case AiRole::None: break;
    }
}

void ai_controller_step(Ai& ai, int player) {
    if (player < 0 || player >= 10) return;
    AiController& c = ai.state.controllers[static_cast<std::size_t>(player)];
    if (!c.exists) {
        set_fault(ai, "computer player without a controller");
        return;
    }
    const PlayerSlot& p = ai.players[static_cast<std::size_t>(player)];
    if (!p.active || p.controller != 2) return;
    if (--c.regroup_countdown < 1) {
        c.regroup_countdown = 30;
        ai_assign_groups(ai, player);
    }
    for (int g = 0; g < 10; ++g) {
        if (!ai.fault.empty()) return;
        const AiBehaviour& b = c.roles[static_cast<std::size_t>(g)];
        if (b.role == AiRole::None || b.next_tick > ai.env.game_tick) continue;
        ai_run_role(ai, player, g);
    }
}

void ai_switch_by_economy(Ai& ai, Unit& u) {
    const int owner = ai.ticks[u.id].owner;
    const PlayerSlot& p = ai.players[static_cast<std::size_t>(owner)];
    const PlayerEconomy& e = ai.economy[static_cast<std::size_t>(owner)];
    const double m2 = static_cast<double>(p.metal_stored) + static_cast<double>(p.metal_stored);
    const double en = static_cast<double>(p.energy_stored);
    if (!(m2 < en) && !std::isnan(m2) && !std::isnan(en)) {  // (an unordered compare goes on)
        ai.systems.set_activated(u, false);
        return;
    }
    const double net = static_cast<double>(e.energy_income) - static_cast<double>(e.energy_usage);
    if (net > 0.0 && rnd(ai, 5) != 0) ai.systems.set_activated(u, true);
}

std::uint16_t ai_pick_build_type(Ai& ai, int side, Unit& builder) {
    const AiTypeInfo& bt = type_of(ai, builder);
    u16 chosen = 0;
    i32 total = 0;
    for (std::size_t k = 0; k < bt.build_options.size(); ++k) {
        const u16 option = bt.build_options[k];
        const i32 s = ai_build_score(ai, side, option);
        if (s > 0) {
            total = wadd(total, s);
            if (static_cast<i32>(rnd(ai, total)) < s) chosen = option;
        }
    }
    if (chosen && type_of(ai, chosen).side != bt.side) chosen = 0;
    return chosen;
}

std::int32_t ai_build_score(Ai& ai, int side, std::uint16_t type) {
    if (side < 0 || side >= 10) return 0;
    const PlayerSlot& p = ai.players[static_cast<std::size_t>(side)];
    const PlayerEconomy& e = ai.economy[static_cast<std::size_t>(side)];
    AiTable* tb = table_of(ai, side);
    if (!tb) return 0;
    if (!(p.energy_stored >= 50.0f)) return 0;
    if (!(p.metal_stored >= 25.0f)) return 0;
    if (ai.env.provider == 1 && (type_of(ai, type).type_flags & kDownloadable)) return 0;
    i32 es = x87::ftol32(e.energy_storage);
    if (es > 1000) es = 1000;
    i32 ms = x87::ftol32(e.metal_storage);
    if (ms > 500) ms = 500;
    auto room = [](i32 cap, float stored, double scale) {
        const double v = (static_cast<double>(cap) - static_cast<double>(stored)) * scale;
        return x87::ftol32(v < 0.0 ? 0.0 : v);
    };
    i32 energy = room(es, p.energy_stored, 0.125);
    i32 metal = room(ms, p.metal_stored, 0.25);
    if (!(static_cast<double>(e.energy_income) - static_cast<double>(e.energy_usage) >= 1.0)) energy += 20;
    if (!(static_cast<double>(e.metal_income) - static_cast<double>(e.metal_usage) >= 1.0)) metal += 20;
    if (!(e.energy_income >= 50.0f)) energy += 100;
    else if (!(e.energy_income >= 200.0f)) energy += 10;
    if (!(e.metal_income >= 3.0f)) metal += 100;
    else if (!(e.metal_income >= 5.0f)) metal += 20;
    i32 mw = metal <= 0 ? 0 : metal;
    if (mw >= 100) mw = 100;
    const i32 ed = energy - mw;
    const i32 ew = ed <= 0 ? 0 : (ed < 100 ? ed : 100);
    const i32 rest = 100 - mw - ew;
    const i32 gw = rest <= 0 ? 0 : rest;
    // The profile limit.
    if (type == 0 || type >= ai.types.size()) return 0;
    const i32 lim = tb->limit[type];
    if (lim != -1 && !(tb->counts[type] < lim)) return 0;
    const auto& sc = tb->build_scores[type];
    const i32 blend = wadd(wadd(wmul(gw, sc[0]), wmul(sc[1], mw)), wmul(sc[2], ew));
    return wmul(blend, tb->weight[type]) / 10000;
}

bool ai_find_site(Ai& ai, int side, const std::array<std::int32_t, 3>& near, std::uint16_t type, std::array<std::int32_t, 3>& out) {
    AiTable* tb = table_of(ai, side);
    if (!tb) return false;
    const i32 W = world_width(ai), H = world_height(ai);
    const i32 most = W > H ? W : H;
    if (tb->search_radius < most) tb->search_radius += 0xA0;
    const i32 dx = wsub(tb->anchor[0], near[0]), dy = wsub(tb->anchor[1], near[1]), dz = wsub(tb->anchor[2], near[2]);
    const i32 r16 = wshl(tb->search_radius, 16);
    const i32 d = length3(dx, dy, dz);
    Pos centre;
    if (r16 >= d) {
        centre = tb->anchor;
    } else if (d == 0) {
        set_fault(ai, "site search scaled by a zero distance (division by zero)");
        return false;
    } else {
        // Towards the base centre from `near`, at the search radius.
        const i32 s = static_cast<i32>(static_cast<u32>(static_cast<std::uint64_t>((static_cast<i64>(r16) << 16) / d)));
        centre = {wadd(near[0], scale16(dx, s)), wadd(near[1], scale16(dy, s)), wadd(near[2], scale16(dz, s))};
    }
    const AiTypeInfo& t = type_of(ai, type);
    i16 cx = 0, cz = 0;
    bool ok;
    if ((t.extracts_metal < 0.0f || t.extracts_metal > 0.0f) && ai.env.surface_metal < static_cast<i32>(rnd(ai, 0xFF)))
        ok = best_metal_site(ai, *tb, type, centre, wshl(tb->search_radius, 2), cx, cz);
    else
        ok = random_site(ai, *tb, type, centre, tb->search_radius, cx, cz);
    if (!ok) return false;
    out[0] = wshl(t.footprint_x + cx * 2, 19);
    out[2] = wshl(t.footprint_z + cz * 2, 19);
    tb->search_radius = 0;
    return true;
}

Unit* ai_nearest_enemy(Ai& ai, int player, std::int32_t x, std::int32_t z) {
    const PlayerSlot& me = ai.players[static_cast<std::size_t>(player)];
    (void)me;
    const auto& mine = ai.allied[static_cast<std::size_t>(player)];
    i32 best = 0x7FFFFFFF;
    Unit* found = nullptr;
    for (int s = 0; s < 10; ++s) {
        const PlayerSlot& p = ai.players[static_cast<std::size_t>(s)];
        if (!p.active || p.controller < 1 || p.controller > 3 || p.state == 10) continue;
        if (p.state >= mine.size() || mine[p.state] != 0) continue;
        for (int id = p.first_unit; id <= p.last_unit; ++id) {
            if (id < 0 || static_cast<std::size_t>(id) >= ai.units.size()) break;
            Unit& u = ai.units[static_cast<std::size_t>(id)];
            const u32 f = u.flags;
            if (!(f & kAliveFlag) || (f & 3u) == 2u || (f & 0x8000u) || (u.activate_flags & unit_flags::kCloaked)) continue;
            const i32 d = wadd(square_hi(wsub(x, u.pos[0])), square_hi(wsub(z, u.pos[2])));
            if (d < best) {
                best = d;
                found = &u;
            }
        }
    }
    return found;
}

void ai_order_group(Ai& ai, int player, std::int32_t group, std::uint8_t intent, bool queued, Unit* target,
                    const std::array<std::int32_t, 3>* pos, std::int32_t param0, std::int32_t param1) {
    const PlayerSlot& p = ai.players[static_cast<std::size_t>(player)];
    for (int id = p.first_unit; id <= p.last_unit; ++id) {
        if (id < 0 || static_cast<std::size_t>(id) >= ai.units.size()) break;
        Unit& u = ai.units[static_cast<std::size_t>(id)];
        if (ai.ticks[static_cast<std::size_t>(id)].type_index == 0 || static_cast<i32>(u.group) != group) continue;
        order(ai, intent, queued, u, target, pos, param0, param1);
    }
}

std::int32_t ai_threat_near(Ai& ai, int side, const std::array<std::int32_t, 3>& pos, std::int32_t radius) {
    AiTable* tb = table_of(ai, side);
    if (!tb) return 0;
    const UnitTable& seen = ai.targeting.tables[static_cast<std::size_t>(side)];
    const i32 r2 = wmul(radius, radius);
    i32 sum = 0;
    for (Unit* u : seen.seen) {
        const i32 d = wadd(square_hi(wsub(pos[0], u->pos[0])), square_hi(wsub(pos[2], u->pos[2])));
        if (d > r2) continue;
        const std::size_t type = ai.ticks[u->id].type_index;
        sum = wadd(sum, type < tb->threat.size() ? tb->threat[type] : 0);
    }
    return sum;
}

}  // namespace sim
