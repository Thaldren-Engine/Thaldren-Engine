#include "unit_missions_common.h"

#include "sim/trig.h"
#include "sim/weapons.h"
#include "sim/x87_math.h"

namespace sim::um {

UnitMissions& ctx(Missions& m) {
    if (!m.unit_missions) {
        m.fault = "unit mission behaviour without its context";
        static const std::vector<TargetingTypeInfo> no_targeting;
        static const std::vector<MotionType> no_motion;
        static const std::vector<UnitMissionTypeInfo> no_types;
        static const MapGrid no_map;
        static const std::vector<FeatureTypeInfo> no_features;
        static const HeightMap no_heights;
        static const std::vector<std::uint16_t> no_explored;
        static UnitMissionEnvironment env;
        struct Null final : UnitMissionSystems {
            void heap_alloc(u32) override {}
            std::shared_ptr<OrderGoal> make_goal(Unit&, MissionOrder&, const GoalRequest&) override { return nullptr; }
            void attach_goal(Unit&, MissionOrder&, std::shared_ptr<OrderGoal>) override {}
            void adjust_goal(Unit&, OrderGoal&, GoalRequest::Set, i32) override {}
            void set_layer(Unit&, u32) override {}
            void units_in_radius(const Vec&, i32, std::vector<Unit*>&) override {}
            void air_bases_in_radius(int, const Vec&, i32, std::vector<Unit*>&) override {}
            void broadcast_script_call(Unit&, const char*, i32, i32, i32, i32, i32) override {}
            i32 reclaim_site_y() override { return 0; }
            void place_unit(Unit&, const Vec&, u32) override {}
            void teleport_trail(Unit&, const Vec&) override {}
            void remove_feature(i32) override {}
            void wreck_attitude(const MapCell&, Unit&) override {}
        };
        static Null null;
        static UnitMissions none{no_targeting, no_motion, no_types, no_map, no_features, no_heights, no_explored, null, env};
        return none;
    }
    return *m.unit_missions;
}

const MissionTypeInfo& mission_type(Missions& m, const Unit& u) {
    static const MissionTypeInfo none;
    const std::size_t i = tick_of(m, u).type_index;
    return i < m.types.size() ? m.types[i] : none;
}
const TargetingTypeInfo& targeting_type(Missions& m, const Unit& u) {
    static const TargetingTypeInfo none;
    const std::size_t i = tick_of(m, u).type_index;
    const auto& v = ctx(m).targeting;
    return i < v.size() ? v[i] : none;
}
const MotionType& motion_type(Missions& m, const Unit& u) {
    static const MotionType none;
    const std::size_t i = tick_of(m, u).type_index;
    const auto& v = ctx(m).motion;
    return i < v.size() ? v[i] : none;
}
const UnitMissionTypeInfo& extra_type(Missions& m, const Unit& u) {
    static const UnitMissionTypeInfo none;
    const std::size_t i = tick_of(m, u).type_index;
    const auto& v = ctx(m).types;
    return i < v.size() ? v[i] : none;
}

const ConstructionTypeInfo& construction_type(Missions& m, const Unit& u) {
    static const ConstructionTypeInfo none;
    const std::size_t i = tick_of(m, u).type_index;
    if (!m.construction || i >= m.construction->types.size()) return none;
    return m.construction->types[i];
}

bool allied(Missions& m, int player, const Unit& other) {
    const u8 state = owner_of(m, other).state;
    if (player < 0 || static_cast<std::size_t>(player) >= m.allied.size() || state >= 16) {
        m.fault = "alliance byte outside the modelled range";
        return false;
    }
    return m.allied[static_cast<std::size_t>(player)][state] != 0;
}

bool in_category(Missions& m, const CategoryMask& mask, const Unit& target) {
    // The original indexes the mask by the target's type index.
    const u16 bit = tick_of(m, target).type_index;
    if (static_cast<std::size_t>(bit >> 5) >= mask.size()) {
        m.fault = "category bit past the mask";
        return false;
    }
    return (mask[bit >> 5] >> (bit & 31)) & 1u;
}

void set_deadline(Missions& m, MissionOrder& o, u32 ticks) {
    o.wait |= order_wait::kTimer;
    o.wake_tick = game_tick(m) + ticks;
}

void speech(Missions& m, Unit& u, u32 id, SpeechText t) { m.systems.speech(u, id, t); }

void announce(Missions& m, MissionOrder& o, SpeechText text) {
    if (!(o.flags & 0x2000)) return;
    o.flags &= ~0x2000u;
    m.systems.speech(*o.unit, 5, text);
}

MissionOrder* new_order(Missions& m, u8 kind, Unit* target, const i32* pos, i32 p0, i32 p1, i32 p2) {
    if (kind >= mission::kCount) {
        m.fault = "order kind outside the mission table";
        return nullptr;
    }
    MissionOrder* o = m.orders.systems.allocate_order();
    construct_order(m.orders, *o, kind, target, pos, p0, p1, p2);
    return o;
}

void prepend_new(Missions& m, Unit& u, u8 kind, Unit* target, const i32* pos, i32 p0, i32 p1, i32 p2) {
    if (MissionOrder* o = new_order(m, kind, target, pos, p0, p1, p2)) prepend_order(u, *o);
}

void append_new(Missions& m, Unit& u, u8 kind, Unit* target, const i32* pos, i32 p0, i32 p1, i32 p2) {
    if (MissionOrder* o = new_order(m, kind, target, pos, p0, p1, p2)) insert_order(u, *o, nullptr);
}

GoalRequest point_goal(const Vec& pos) {
    GoalRequest g;
    g.kind = GoalRequest::Kind::Point;
    g.pos = pos;
    return g;
}

std::shared_ptr<OrderGoal> make_goal(Missions& m, MissionOrder& o, const GoalRequest& g) {
    return ctx(m).systems.make_goal(*o.unit, o, g);
}
void attach_made(Missions& m, MissionOrder& o, std::shared_ptr<OrderGoal> goal) { ctx(m).systems.attach_goal(*o.unit, o, std::move(goal)); }
void attach(Missions& m, MissionOrder& o, const GoalRequest& g) { attach_made(m, o, make_goal(m, o, g)); }
void set_goal_radius(Missions& m, MissionOrder& o, const std::shared_ptr<OrderGoal>& goal, i32 r) {
    if (goal) ctx(m).systems.adjust_goal(*o.unit, *goal, GoalRequest::Set::Radius, r);
}

void drop_goal(Missions& m, MissionOrder& o) {
    // Without a motion object nothing happens; with one an attached goal is detached and deleted.
    if (has_motion(m, *o.unit) && o.attached) m.systems.drop_goal_object(*o.unit, o);
}

void circle_goal(Missions& m, MissionOrder& o, const Vec& pos, i32 radius) { m.systems.attach_circle_goal(*o.unit, o, pos, radius); }

void ring_goal(Missions& m, MissionOrder& o, const Vec& pos, i32 outer, i32 inner) {
    if (!m.construction) {
        m.fault = "ring goal without the construction systems";
        return;
    }
    m.construction->systems.attach_ring_goal(*o.unit, o, pos, outer, inner);
}

void edge_goal(Missions& m, MissionOrder& o, u32 cell, u32 size) {
    if (!m.construction) {
        m.fault = "edge goal without the construction systems";
        return;
    }
    m.construction->systems.attach_edge_goal(*o.unit, o, cell, size);
}

void UnitList::push(Missions& m, Unit* u) {
    if (v.size() == capacity) {
        // The original's vector: a new block of size + max(size, 1) entries (under 2 entries: + 1).
        const std::size_t n = v.size();
        capacity = n + (n < 2 ? 1 : n);
        ctx(m).systems.heap_alloc(static_cast<u32>(capacity * 4));
    }
    v.push_back(u);
}

Vec ahead(const Vec& from, u16 angle, i32 dist) {
    return {wsub(from[0], trig::mul_sin(angle, dist)), from[1], wsub(from[2], trig::mul_cos(angle, dist))};
}

u16 heading_between(const Vec& a, const Vec& b) {
    return static_cast<u16>(x87::atan2_scaled(wsub(a[0], b[0]), wsub(a[2], b[2])));
}

i32 flat_distance(const Vec& a, const Vec& b) {
    return x87::ftol32(x87::crt_hypot(static_cast<double>(wsub(a[0], b[0])), static_cast<double>(wsub(a[2], b[2]))));
}

i32 snap_axis(i32 v, i16 footprint) {
    const i16 cell = static_cast<i16>(wadd(wsub(v, wshl(footprint, 19)), 0x80000) >> 20);
    return wshl(wadd(footprint, static_cast<i32>(cell) * 2), 19);
}

void aim_slot_at_point(Unit& u, const Vec& pos, int slot) {
    WeaponSlot& s = u.weapons[static_cast<std::size_t>(slot)];
    s.target_x = static_cast<i16>(static_cast<u32>(pos[0]) >> 16);
    s.target_z = static_cast<i16>(static_cast<u32>(pos[2]) >> 16);
    if (s.target_z == kUnitTarget) s.target_z = static_cast<i16>(-0x7FFF);
    u.net_dirty = static_cast<u16>(u.net_dirty & 0x83FFu);
}

Unit* slot_target_unit(Missions& m, Unit& u, int slot) {
    const WeaponSlot& s = u.weapons[static_cast<std::size_t>(slot)];
    if (s.target_z != kUnitTarget || s.target_x == 0) return nullptr;
    if (s.target_x < 0 || static_cast<std::size_t>(s.target_x) >= m.units.size()) {
        m.fault = "weapon target id outside the unit table";
        return nullptr;
    }
    return &m.units[static_cast<std::size_t>(s.target_x)];
}

i32 slot_range(Missions& m, Unit& u, u32 slot) {
    slot &= 0xFF;
    if (slot > 2) {
        m.fault = "weapon slot outside 0..2";
        return 0;
    }
    const WeaponType* w = u.weapons[slot].type;
    if (!w) {
        m.fault = "range of a slot without a weapon";
        return 0;
    }
    return w->range;
}

u8 first_weapon_slot(const Unit& u) {
    if (u.weapons[0].flags & slot_flags::kEnabled) return 0;
    if (u.weapons[1].flags & slot_flags::kEnabled) return 1;
    return static_cast<u8>(u.weapons[2].flags & slot_flags::kEnabled);
}

void script_call(Missions& m, Unit& u, const char* name, bool run_now) {
    if (!u.script) {
        m.fault = "script call on a unit without a script";
        return;
    }
    u.script->call(name, nullptr, run_now);
}

void script_call_args(Missions& m, Unit& u, const char* name, bool run_now, i32 argc, i32 a, i32 b, i32 c, i32 d) {
    if (!u.script) {
        m.fault = "script call on a unit without a script";
        return;
    }
    u.script->call_with_args(u.script->program().find_function(name), nullptr, run_now, argc, a, b, c, d);
}

void script_query(Missions& m, Unit& u, const char* name, i32* a, i32* b, i32* c, i32* d) {
    if (!u.script) {
        m.fault = "script call on a unit without a script";
        return;
    }
    u.script->call_in_out(u.script->program().find_function(name), a, b, c, d);
}

bool can_repair(Missions& m, Unit& unit, Unit* target) {
    if (!target || !(mission_type(m, unit).abilities & kAbilityRepair)) return false;
    if (static_cast<i32>(target->hit_points) == static_cast<i32>(max_damage(*target)) || (target->flags & 3u) == 2) return false;
    const u32 tf = type_flags(m, unit);
    const i32 sea = ctx(m).env.sea_level;
    const i32 top = hi16(target->pos[1]) + (target->type ? hi16(target->type->model_max_y) : 0);
    if ((tf & kTypeCanFly) && !(tf & kTypeAmphibious) && top < sea) return false;
    if (!(tf & kTypeCanFly) && top < sea - targeting_type(m, unit).max_water_depth) return false;
    return true;
}

void leave_carrier(Missions& m, Unit& u, u8 layer) {
    if (!m.construction) {
        m.fault = "transport packet without the construction systems";
        return;
    }
    m.construction->systems.transport_attach(u, nullptr, 0xFF, layer);
}


const MapCell* cell_at(const MapGrid& map, const Vec& pos) {
    const i32 x = pos[0] >> 20, z = pos[2] >> 20;
    if (x < 0 || x >= map.width || z < 0 || z >= map.height) return nullptr;
    return &map.cells[static_cast<std::size_t>(z) * static_cast<std::size_t>(map.width) + static_cast<std::size_t>(x)];
}

u16 resolve_feature(Missions& m, const MapGrid& map, const MapCell& cell) {
    if (cell.feature < 0xFFFB) return cell.feature;
    if (cell.feature != 0xFFFE) return 0xFFFF;
    // A part: the feature's own cell lies back by the row / column offsets (not checked).
    const std::ptrdiff_t at = &cell - map.cells.data();
    const std::ptrdiff_t back = static_cast<std::ptrdiff_t>(cell.feature_aux & 0xFF) * map.width + (cell.feature_aux >> 8);
    if (at - back < 0 || static_cast<std::size_t>(at - back) >= map.cells.size()) {
        m.fault = "feature part pointing outside the map";
        return 0xFFFF;
    }
    return map.cells[static_cast<std::size_t>(at - back)].feature;
}

bool pick_reclaim_sites(Missions& m, const Vec& centre, i32 radius, ReclaimSites& out) {
    UnitMissions& c = ctx(m);
    // Two lists of (x, y, z, amount) records sized from the radius (whole units squared / 256 x 16
    // bytes), the metal list first; the original never checks that they suffice.
    const i32 rw = static_cast<i16>(static_cast<u32>(radius) >> 16);
    const i32 bytes = (rw * rw / 256) * 16;
    c.systems.heap_alloc(static_cast<u32>(bytes));
    c.systems.heap_alloc(static_cast<u32>(bytes));
    const std::size_t capacity = static_cast<std::size_t>(bytes > 0 ? bytes / 16 : 0);
    struct Site { Vec pos; float amount; };
    std::vector<Site> energy, metal;
    const i32 y = c.systems.reclaim_site_y();  // never written by the original: a stale stack word
    const i32 half = radius / 2;
    for (i32 z = wsub(centre[2], half); z <= wadd(centre[2], half); z = wadd(z, 0x300000)) {
        for (i32 x = wsub(centre[0], half); x <= wadd(centre[0], half); x = wadd(x, 0x300000)) {
            const Vec at = {x, y, z};
            const MapCell* cell = cell_at(c.map, at);
            if (!cell) continue;
            const u16 f = resolve_feature(m, c.map, *cell);
            if (f >= 0xFFFB) continue;
            if (f >= c.features.size()) {
                m.fault = "feature type past the table";
                return false;
            }
            const FeatureTypeInfo& ft = c.features[f];
            if (!(ft.flags & 0x80) || !(ft.flags & 0x100)) continue;
            if (!is_zero(ft.energy)) energy.push_back({at, ft.energy});
            if (!is_zero(ft.metal)) metal.push_back({at, ft.metal});
            if (energy.size() > capacity || metal.size() > capacity) {
                m.fault = "reclaim site lists overflow their blocks";
                return false;
            }
        }
    }
    bool any = false;
    auto best_of = [&](const std::vector<Site>& v, bool& found, Vec& pos, float& amount) {
        found = !v.empty();
        if (!found) {
            amount = 0.0f;
            return;
        }
        float best = 0.0f;
        u32 pick = 0;
        for (int k = 0; k < 3; ++k) {
            const u32 r = m.orders.rng.next_mod(static_cast<u32>(v.size()));
            if (!(best >= v[r].amount)) {  // FCOMP: an unordered amount is taken as well
                best = v[r].amount;
                pick = r;
            }
        }
        pos = v[pick].pos;
        amount = v[pick].amount;
        any = true;
    };
    best_of(energy, out.energy_found, out.energy_pos, out.energy_amount);
    best_of(metal, out.metal_found, out.metal_pos, out.metal_amount);
    return any;
}

bool repair_patrol_candidate(Missions& m, const Unit& self, const Unit& c) {
    if (&c == &self) return false;
    if (!allied(m, tick_of(m, self).owner, c)) return false;
    if ((c.flags & 3u) != 1) return false;
    if (!(static_cast<u32>(static_cast<i32>(c.hit_points)) < max_damage(c) || !is_zero(c.build_fraction_left))) return false;
    const UnitTickState& t = tick_of(m, c);
    return t.last_attacker_slot != owner_of(m, self).state || t.last_damage_kind != 5;
}

bool enqueue_repair(Missions& m, Unit& u, Unit& target, bool no_standing_move) {
    const u8 kind = m.systems.resolve_mission(OrderIntent::Repair, u, &target, nullptr);
    if (!kind) return false;
    const u32 mo = u.flags & kMoveOrders;
    if (no_standing_move || (mo != 0 && mo != 0x40000 && mo != 0x80000)) return false;
    if (mo == 0x80000) {
        prepend_new(m, u, kind, &target, nullptr);
        return true;
    }
    // Hold position (sight distance) or maneuver (leash): a move back to where it stands
    // goes behind the repair, which ends when the unit strays that far.
    const u8 back = m.systems.resolve_mission(OrderIntent::Move, u, nullptr, &u.pos);
    prepend_new(m, u, back, nullptr, u.pos.data());
    const i32 leash = mo == 0 ? static_cast<i32>(targeting_type(m, u).sight_distance)
                              : static_cast<i32>(static_cast<u16>(targeting_type(m, u).maneuver_leash));
    MissionOrder* rep = new_order(m, kind, &target, nullptr, 0, 0, leash);
    if (!rep) return false;
    rep->scratch0 = (static_cast<u32>(u.pos[0]) >> 16) | (static_cast<u32>(u.pos[2]) & 0xFFFF0000u);
    prepend_order(u, *rep);
    return true;
}
}  // namespace sim::um

namespace sim {

void units_in_radius(const UnitBuckets& b, const std::array<std::int32_t, 3>& pos, std::int32_t radius, std::vector<Unit*>& out) {
    using namespace um;
    auto clamp = [](i32 v, i32 n) { return static_cast<u32>(v) < static_cast<u32>(n) ? v : (v < 0 ? 0 : n - 1); };
    const i32 x0 = clamp(wsub(pos[0], radius) >> 23, b.width), z0 = clamp(wsub(pos[2], radius) >> 23, b.height);
    const i32 x1 = clamp(wadd(pos[0], radius) >> 23, b.width), z1 = clamp(wadd(pos[2], radius) >> 23, b.height);
    const i32 r2 = square_hi(radius);
    for (i32 z = z0; z <= z1; ++z)
        for (i32 x = x0; x <= x1; ++x) {
            const std::size_t at = static_cast<std::size_t>(b.width) * static_cast<std::size_t>(z) + static_cast<std::size_t>(x);
            if (at >= b.heads.size()) continue;
            for (Unit* u = b.heads[at]; u; u = u->cargo_next)
                if (wadd(square_hi(wsub(u->pos[0], pos[0])), square_hi(wsub(u->pos[2], pos[2]))) <= r2) out.push_back(u);
        }
}

bool air_bases_in_radius(const UnitTable& table, const std::vector<UnitTickState>& ticks, const std::array<std::int32_t, 3>& pos,
                         std::int32_t radius, std::vector<Unit*>& out) {
    using namespace um;
    if (!table.exists) return false;
    for (Unit* u : table.air_bases) {
        const UnitTickTypeInfo* t = ticks[u->id].tick_type;
        const u32 tf = t ? t->type_flags : 0;
        if (!(tf & kTypeBuilder) || !(tf & kTypeAirBase) || !(u->activate_flags & unit_flags::kActivated)) continue;
        if (wadd(square_hi(wsub(pos[0], u->pos[0])), square_hi(wsub(pos[2], u->pos[2]))) <= static_cast<i32>(static_cast<u32>(radius) * static_cast<u32>(radius)))
            out.push_back(u);
    }
    return true;
}

void script_attach_unit(Missions& m, Unit& carrier, std::int32_t passenger, std::int32_t piece, std::int32_t mode) {
    const std::uint16_t id = static_cast<std::uint16_t>(passenger);
    if (id == 0) return;
    if (id >= m.units.size()) {
        m.fault = "script attach of a unit id past the table";
        return;
    }
    Unit& c = m.units[id];
    if (!(c.flags & unit_flags::kAlive) || (c.carrier && c.carrier != &carrier)) return;
    if (!m.construction) {
        m.fault = "transport packet without the construction systems";
        return;
    }
    m.construction->systems.transport_attach(c, &carrier, static_cast<std::uint8_t>(piece), static_cast<std::uint8_t>(mode));
}

void script_drop_unit(Missions& m, Unit& carrier, std::int32_t passenger) {
    const std::uint16_t id = static_cast<std::uint16_t>(passenger);
    if (id == 0) return;
    if (id >= m.units.size()) {
        m.fault = "script drop of a unit id past the table";
        return;
    }
    Unit& c = m.units[id];
    if (!(c.flags & unit_flags::kAlive) || c.carrier != &carrier) return;
    if (!m.construction) {
        m.fault = "transport packet without the construction systems";
        return;
    }
    if (!m.construction->systems.can_place(m.orders.ticks[id].type_index, id, c.cell_x, c.cell_z, 1)) return;
    m.construction->systems.transport_attach(c, nullptr, 0xFF, 1);
}

bool unit_mission_kind(std::uint8_t kind) {
    switch (kind) {
    case mission::kAirStrike: case mission::kAirToAir: case mission::kAirToGround: case mission::kAirToGroundHover:
    case mission::kAttackChase: case mission::kAttackKamikaze: case mission::kFollowGround: case mission::kGroundPickup:
    case mission::kGroundUnload: case mission::kRepairPatrol: case mission::kSuppress: case mission::kTeleport:
    case mission::kAirEvade: case mission::kAirFollow: case mission::kAirGetRepaired: case mission::kAirLandIfCan:
    case mission::kAirLanding: case mission::kAirMove: case mission::kAirPatrol: case mission::kAirPickup:
    case mission::kAirRepairPatrol: case mission::kAirSeekAttack: case mission::kAirSeekGuard: case mission::kAirStandby:
    case mission::kAirUnload:
        return true;
    default:
        return false;
    }
}

std::int32_t run_unit_mission(Missions& m, Unit& u, MissionOrder& o, std::uint32_t& event) {
    using namespace um;
    switch (o.kind) {
    case mission::kAirStrike: return air_strike(m, u, o, event);
    case mission::kAirToAir: return air_to_air(m, u, o, event);
    case mission::kAirToGround: return air_to_ground(m, u, o, event);
    case mission::kAirToGroundHover: return air_to_ground_hover(m, u, o, event);
    case mission::kAttackChase: return attack_chase(m, u, o, event);
    case mission::kAttackKamikaze: return attack_kamikaze(m, u, o, event);
    case mission::kFollowGround: return follow_ground(m, u, o, event);
    case mission::kGroundPickup: return ground_pickup(m, u, o, event);
    case mission::kGroundUnload: return ground_unload(m, u, o, event);
    case mission::kRepairPatrol: return repair_patrol(m, u, o, event);
    case mission::kSuppress: return suppress(m, u, o, event);
    case mission::kTeleport: return teleport(m, u, o);
    case mission::kAirEvade: return air_evade(m, u, o, event);
    case mission::kAirFollow: return air_follow(m, u, o, event);
    case mission::kAirGetRepaired: return air_get_repaired(m, u, o);
    case mission::kAirLandIfCan: return air_land_if_can(m, u, o, event);
    case mission::kAirLanding: return air_landing(m, u, o, event);
    case mission::kAirMove: return air_move(m, u, o);
    case mission::kAirPatrol: return air_patrol(m, u, o, event);
    case mission::kAirPickup: return air_pickup(m, u, o, event);
    case mission::kAirRepairPatrol: return air_repair_patrol(m, u, o, event);
    case mission::kAirSeekAttack: return air_seek_attack(m, u, o, event);
    case mission::kAirSeekGuard: return air_seek_guard(m, u, o, event);
    case mission::kAirStandby: return air_standby(m, u, o);
    case mission::kAirUnload: return air_unload(m, u, o, event);
    default:
        m.fault = "not a unit mission kind";
        return mission_result::kClearAll;
    }
}

}  // namespace sim
