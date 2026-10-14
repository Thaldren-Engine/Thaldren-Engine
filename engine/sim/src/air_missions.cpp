// Aircraft mission behaviours (see docs/RE_MEMORY.md, "Aircraft and transports").
#include "unit_missions_common.h"

#include <cmath>

#include "sim/trig.h"
#include "sim/weapons.h"
#include "sim/x87.h"
#include "sim/x87_math.h"

namespace sim::um {
using Set = GoalRequest::Set;

i32 cruise_altitude(Missions& m, const Unit& u) { return motion_type(m, u).cruise_altitude; }

// The common start of aircraft orders: the weapons follow the order again, a carried
// aircraft leaves its transport, it activates, and a landed one climbs to half its cruise
// altitude above where it stands.
void take_off(Missions& m, Unit& u, MissionOrder& o) {
    unrelease_weapon_slot(u, 3);
    if (u.carrier) leave_carrier(m, u, 2);
    unit_set_activate(u, m.world, unit_flags::kActivated, true);
    if (motion_layer(m, u) == 1) {
        ctx(m).systems.set_layer(u, 2);
        attach(m, o, point_goal(u.pos).set(Set::Altitude, cruise_altitude(m, u) / 2));
        o.wait |= 0xE0;
    }
}

// An aircraft that strayed off the map heads back: 800 world units from where it is toward
// the middle of the visible map.
std::int32_t head_back_onto_map(Missions& m, Unit& u, MissionOrder& o) {
    const UnitMissionEnvironment& e = ctx(m).env;
    const Vec centre = {wshl(e.map_width / 2, 16), 0, wshl(e.map_height / 2, 16)};
    const Vec to = ahead(u.pos, heading_between(u.pos, centre), 0x3200000);
    o.wait |= 0xE0;
    attach(m, o, point_goal(to).set(Set::Radius, 0x80));
    return mission_result::kContinue;
}

// --- VTOL_Move (55) -------------------------------------------------------
// Take off, then fly to the order's position (snapped to the footprint grid).
std::int32_t air_move(Missions& m, Unit& u, MissionOrder& o) {
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        take_off(m, u, o);
        return mission_result::kAdvance;
    case 1: {
        announce(m, o);
        release_weapon_targets(u, 3);
        o.pos[0] = snap_axis(o.pos[0], u.footprint_x);
        o.pos[2] = snap_axis(o.pos[2], u.footprint_z);
        attach(m, o, point_goal(o.pos));
        o.wait = 0xE0;
        return mission_result::kAdvance;
    }
    case 2:
        if (!o.next) speech(m, u, 6);
        return mission_result::kFinished;
    default:
        return mission_result::kClearAll;
    }
}

// --- VTOL_Standby (64) ----------------------------------------------------
// The idle state of aircraft: attack what fire-at-will finds; a flying one either circles
// the spot it was given the order at (when it carries something) or lands.
std::int32_t air_standby(Missions& m, Unit& u, MissionOrder& o) {
    switch (o.step) {
    case 0: {
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        unrelease_weapon_slot(u, 3);
        o.wait |= order_wait::kClearWeapons;
        set_deadline(m, o, 1);
        // Where the order's unit is now (whole x and z).
        const Unit& ou = *o.unit;
        o.scratch0 = (static_cast<u32>(ou.pos[0]) >> 16) | (static_cast<u32>(ou.pos[2]) & 0xFFFF0000u);
        return mission_result::kAdvance;
    }
    case 1: {
        Unit* t = m.systems.fire_at_will_target(u);
        if (t && m.systems.issue_attack_order(u, *t, false)) {
            o.wait = 0;
            o.step = 0;
            return mission_result::kSnooze;
        }
        return mission_result::kAdvance;
    }
    case 2: {
        if (!flies(m, u) || (u.flags & 3u) != 2) {
            o.wait |= order_wait::kClearWeapons;
            set_deadline(m, o, m.orders.rng.next_mod(30) + 30);
            o.step = 1;
            return mission_result::kContinue;
        }
        if (u.cargo_head) {
            const u16 a = static_cast<u16>(m.orders.rng.next_mod(0x10000));
            const Vec home = {wshl(static_cast<i16>(o.scratch0 & 0xFFFF), 16), 0, wshl(static_cast<i16>(o.scratch0 >> 16), 16)};
            const i32 r = wshl(static_cast<i32>(m.orders.rng.next_mod(0x20)) + 8, 16);
            attach(m, o, point_goal(ahead(home, a, r)).set(Set::Altitude, cruise_altitude(m, u)));
            set_deadline(m, o, m.orders.rng.next_mod(15) + 30);
            o.step = 1;
            return mission_result::kContinue;
        }
        prepend_new(m, u, mission::kAirLandIfCan, nullptr, o.pos.data());
        return mission_result::kFinished;
    }
    default:
        return mission_result::kClearAll;
    }
}

// --- VTOL_LandIfCan (52) --------------------------------------------------
// Land where the aircraft is, or on a free spot found by random tries in widening squares;
// failing that, circle the order's position (param0 the circling angle).
std::int32_t air_land_if_can(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (o.next) return mission_result::kFinished;
    if (event & 0x40) return mission_result::kFinished;
    if (u.bucket == kOffMapBucket) return head_back_onto_map(m, u, o);
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        if (o.pos[0] == 0 && o.pos[2] == 0 && o.pos[1] == 0) o.pos = u.pos;
        o.param0 = static_cast<i32>(m.orders.rng.next_mod(0x10000));
        o.param1 = o.param0 & 1;
        take_off(m, u, o);
        return mission_result::kAdvance;
    case 1: {
        if (can_land_at(m, u, u.pos)) {
            script_call_now(m, u, "EndTransport");
            // Down to the ground (or the water surface): an altitude of 0 above land, the
            // (negative) depth below the sea level over water.
            const i32 sea = ctx(m).env.sea_level;
            const HeightMap& hm = ctx(m).heights;
            auto sample = [&]() { return hm.sample(static_cast<i16>(u.pos[0] >> 16), static_cast<i16>(u.pos[2] >> 16)); };
            const i32 h = sample() > sea ? sample() : sea;
            const i32 alt = h > sea ? 0 : sample() - sea;
            attach(m, o, point_goal(u.pos).set(Set::Altitude, alt));
            o.wait = 0xE0;
            unit_set_activate(u, m.world, unit_flags::kActivated, false);
            return mission_result::kAdvance;
        }
        for (i32 n = 0x81, half = 0x40; n < 0x201; n += 0x20, half += 0x10) {
            Vec c = u.pos;
            c[0] = wadd(c[0], wshl(static_cast<i32>(m.orders.rng.next_mod(static_cast<u32>(n))) - half, 16));
            c[2] = wadd(c[2], wshl(static_cast<i32>(m.orders.rng.next_mod(static_cast<u32>(n))) - half, 16));
            c[0] = snap_axis(c[0], u.footprint_x);
            c[2] = snap_axis(c[2], u.footprint_z);
            if (can_land_at(m, u, c)) {
                attach(m, o, point_goal(c));
                o.wait = 0xE0;
                return mission_result::kContinue;
            }
        }
        if (event & 0xE0) o.param0 = wadd(o.param0, -0x5555);
        Vec p = ahead(o.pos, static_cast<u16>(o.param0), 0xA00000);
        attach(m, o, point_goal(p).set(Set::Radius, 0x40));
        o.wait |= 0xE0;
        return mission_result::kContinue;
    }
    case 2:
        if (!(event & 0x20)) return 8;
        ctx(m).systems.set_layer(u, 1);
        return mission_result::kFinished;
    default:
        return mission_result::kClearAll;
    }
}


// The followed unit was attacked (event 0x10 from the retargeting): go after the attacker
// unless it is an ally or a no-chase type; if the attack order is refused, released weapon
// slots without a better target point at it. True when an attack order was given.
bool answer_attacker(Missions& m, Unit& u, MissionOrder& o, Unit& followed, u32 event) {
    Unit* att = tick_of(m, followed).last_attacker;
    if (!att || allied(m, tick_of(m, *att).owner, u) || !(event & 0x10) || in_category(m, targeting_type(m, u).no_chase, *att))
        return false;
    if (m.systems.issue_attack_order(u, *att, true)) {
        o.wait = 0;
        return true;
    }
    if (!(u.flags & kFireOrders)) return false;
    for (int k = 0; k < 3; ++k) {
        const WeaponSlot& s = u.weapons[static_cast<std::size_t>(k)];
        if (!(s.flags & slot_flags::kEnabled) || !(s.flags & slot_flags::kReleased)) continue;
        if (!s.type) {
            m.fault = "weapon slot without a weapon type";
            return false;
        }
        if (s.type->flags & weapon_flags::kCommandFire) continue;
        Unit* t = slot_target_unit(m, u, k);
        if (!t || !weapon_reaches_unit(m, u, *t, k) || in_category(m, targeting_type(m, u).bad_target[static_cast<std::size_t>(k)], *t))
            aim_weapon_at_unit(u, *att, k);
    }
    return false;
}

// --- VTOL_Follow (49) -----------------------------------------------------
// Guard a unit from the air: answer its attackers, repair it or help what it builds (as
// the aircraft variants of those orders), else circle it. The resolved kind of a helping
// order lives in the low byte of the event argument (the secondary list's stale word).
std::int32_t air_follow(Missions& m, Unit& u, MissionOrder& o, u32& event) {
    Unit* followed = target_of(o);
    if (!followed || (event & 0x48)) {
        if (!o.next) append_new(m, u, mission::kAirSeekGuard, followed, o.pos.data());
        return mission_result::kFinished;
    }
    if (u.bucket == kOffMapBucket) return head_back_onto_map(m, u, o);
    o.pos = followed->pos;
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        announce(m, o, SpeechText::Guarding);
        take_off(m, u, o);
        o.param0 = static_cast<i32>(m.orders.rng.next_mod(0x10000));
        o.param1 = o.param0 & 1;
        return mission_result::kAdvance;
    case 1:
        release_weapon_targets(u, 3);
        return mission_result::kAdvance;
    case 2:
        break;
    default:
        return mission_result::kClearAll;
    }
    if (answer_attacker(m, u, o, *followed, event)) return mission_result::kSnooze;
    if (can_repair(m, u, followed)) {
        const u8 kind = m.systems.resolve_mission(OrderIntent::Repair, u, followed, nullptr);
        if (kind) {
            drop_goal(m, o);
            prepend_new(m, u, kind, target_of(o), nullptr);
            o.wait = 0;
            return mission_result::kSnooze;
        }
    }
    MissionOrder* head = followed->orders;
    if (head && head->kind != mission::kReady && (type_flags(m, u) & kTypeBuilder) && can_repair(m, u, head->target.unit) &&
        (type_flags(m, *followed) & kTypeBuilder) && (head->flags & 0x100000) && &u != head->target.unit) {
        const bool building = head->kind == mission::kMobileBuild || head->kind == mission::kBuildingBuild ||
                              head->kind == mission::kAirMobileBuild;
        const bool has_object = ((head->flags & 0x200) && head->target.unit) || (head->flags & 0x400);
        bool help = false;
        if (building) {
            if (head->target.unit) {
                drop_goal(m, o);
                event = (event & ~0xFFu) | mission::kAirHelpBuild;
                help = true;
            }
        } else if (has_object) {
            u8 kind = head->kind;
            if (kind == mission::kRepairUnit) kind = mission::kAirRepairUnit;
            if (kind == mission::kReclaim) kind = mission::kAirReclaim;
            if (kind == mission::kReclaimUnit) kind = mission::kAirReclaimUnit;
            if (kind == mission::kHelpBuild) kind = mission::kAirHelpBuild;
            event = (event & ~0xFFu) | kind;
            drop_goal(m, o);
            help = true;
        }
        if (help) {
            MissionOrder* h = followed->orders;
            prepend_new(m, u, static_cast<u8>(event & 0xFF), h->target.unit, h->pos.data());
            o.wait = 0;
            return mission_result::kSnooze;
        }
    }
    // Circle the followed unit at its weapon range (+160) or 320 world units.
    if (event & 0xE0) o.param0 = wadd(o.param0, wsub(-0x4000, static_cast<i32>(m.orders.rng.next_mod(0x2000))));
    i32 r = 0x1400000;
    if (u.flags & kHasWeapons) r = wshl(wadd(slot_range(m, u, 0), 0xA0), 16);
    attach(m, o, point_goal(ahead(followed->pos, static_cast<u16>(o.param0), r)).set(Set::Radius, 0x80));
    set_deadline(m, o, 30);
    o.wait |= 0xF8;
    return mission_result::kContinue;
}

// A badly damaged aircraft looks for its player's activated air bases within 3840 world
// units and, if there are any, goes to land on a random one (a VTOL_Landing order in
// front). True when it did.
bool seek_air_base(Missions& m, Unit& u, MissionOrder& o) {
    std::vector<Unit*> bases;
    ctx(m).systems.air_bases_in_radius(owner_of(m, u).state, u.pos, 0xF00, bases);
    if (bases.empty()) return false;
    drop_goal(m, o);
    Unit* pad = bases[m.orders.rng.next_mod(static_cast<u32>(bases.size()))];
    prepend_new(m, u, mission::kAirLanding, pad, nullptr);
    o.wait = 0;
    return true;
}

// --- VTOL_SeekAttack (62) -------------------------------------------------
// Attack the order's target, or circle the order's position at weapon range (+160)
// attacking whatever fire-at-will finds; badly damaged, land at an air base.
std::int32_t air_seek_attack(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (event & 0x40) return mission_result::kFinished;
    if (u.bucket == kOffMapBucket) return head_back_onto_map(m, u, o);
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        if (Unit* t = target_of(o)) {
            if (m.systems.issue_attack_order(u, *t, false)) {
                o.wait = 0;
                return mission_result::kRestart;
            }
            return mission_result::kAdvance;
        }
        if (o.pos[0] == 0 && o.pos[2] == 0 && o.pos[1] == 0) o.pos = u.pos;
        o.param0 = static_cast<i32>(m.orders.rng.next_mod(0x10000));
        o.param1 = o.param0 & 1;
        take_off(m, u, o);
        return mission_result::kAdvance;
    case 1: {
        release_weapon_targets(u, 3);
        if (badly_damaged(u) && seek_air_base(m, u, o)) return mission_result::kRestart;
        Unit* t = m.systems.fire_at_will_target(u);
        if (t && m.systems.issue_attack_order(u, *t, false)) return mission_result::kFinished;
        if (event & 0xE0) o.param0 = wadd(o.param0, wsub(-0x5555, static_cast<i32>(m.orders.rng.next_mod(0x2000))));
        const i32 r = wshl(wadd(slot_range(m, u, 0), 0xA0), 16);
        attach(m, o, point_goal(ahead(o.pos, static_cast<u16>(o.param0), r)).set(Set::Radius, 0x80));
        set_deadline(m, o, m.orders.rng.next_mod(30) + 30);
        o.wait |= 0xE0;
        return mission_result::kContinue;
    }
    default:
        return mission_result::kClearAll;
    }
}

// --- VTOL_SeekGuard (63) --------------------------------------------------
// Guard the first allied ground unit in sight, else circle the order's position;
// badly damaged, land at an air base.
std::int32_t air_seek_guard(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (event & 0x40) return mission_result::kFinished;
    if (u.bucket == kOffMapBucket) return head_back_onto_map(m, u, o);
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        if (o.pos[0] == 0 && o.pos[2] == 0 && o.pos[1] == 0) o.pos = u.pos;
        o.param0 = static_cast<i32>(m.orders.rng.next_mod(0x10000));
        o.param1 = o.param0 & 1;
        return mission_result::kAdvance;
    case 1: {
        if (badly_damaged(u) && seek_air_base(m, u, o)) return mission_result::kRestart;
        // Allied ground units within sight distance (the unit itself excluded).
        std::vector<Unit*> near;
        ctx(m).systems.units_in_radius(u.pos, wshl(targeting_type(m, u).sight_distance, 16), near);
        UnitList guard;
        for (Unit* c : near)
            if (allied(m, tick_of(m, *c).owner, u) && !(type_flags(m, *c) & kTypeCanFly) && c != &u) guard.push(m, c);
        if (!guard.v.empty()) {
            drop_goal(m, o);
            const u8 kind = m.systems.resolve_mission(OrderIntent::Guard, u, guard.v[0], nullptr);
            prepend_new(m, u, kind, guard.v[0], nullptr);
            o.wait = 0;
            return mission_result::kSnooze;
        }
        if (event & 0xE0) o.param0 = wadd(o.param0, wsub(-0x4000, static_cast<i32>(m.orders.rng.next_mod(0x2000))));
        const i32 r = wshl(wadd(slot_range(m, u, 0), 0xA0), 16);
        attach(m, o, point_goal(ahead(o.pos, static_cast<u16>(o.param0), r)).set(Set::Radius, 0x80));
        set_deadline(m, o, 30);
        o.wait |= 0xF8;
        return mission_result::kContinue;
    }
    default:
        return mission_result::kClearAll;
    }
}

// --- VTOL_Patrol (56) -----------------------------------------------------
// Fly toward the order's position (the legs take turns as with ground patrols),
// attacking what fire-at-will finds; badly damaged, land at an air base.
std::int32_t air_patrol(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        ensure_patrol_return(m, u, o);
        announce(m, o, SpeechText::Patrolling);
        take_off(m, u, o);
        release_weapon_targets(u, 3);
        return mission_result::kAdvance;
    case 1:
        o.events &= 0xFFFFFF1Fu;
        return mission_result::kAdvance;
    case 2: {
        if (event & 0xE0) return mission_result::kRotate;
        const Vec p = ahead(o.pos, heading_between(u.pos, o.pos), 0x1400000);
        attach(m, o, point_goal(p).set(Set::Radius, 0x150));
        o.wait |= 0xE0;
        if (badly_damaged(u) && seek_air_base(m, u, o)) return mission_result::kRestart;
        Unit* t = m.systems.fire_at_will_target(u);
        if (t && m.systems.issue_attack_order(u, *t, false)) {
            o.wait = 0;
            return mission_result::kSnooze;
        }
        set_deadline(m, o, 30);
        return mission_result::kContinue;
    }
    default:
        return mission_result::kClearAll;
    }
}

// --- VTOL_Pickup (57) -----------------------------------------------------
// An air transport picks up the order's target: fly over it, ask the script which piece
// carries it (QueryTransport), lower onto it (BeginTransport with the cargo's height),
// attach it and climb back to cruise altitude.
std::int32_t air_pickup(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    Unit* cargo = target_of(o);
    if (!cargo || (event & 0x10048)) {
        speech(m, u, 7, SpeechText::TransportFailed);
        return 8;
    }
    const i32 top = wadd(cargo->type ? cargo->type->model_max_y : 0, cargo->pos[1]);
    if (top <= wshl(ctx(m).env.sea_level, 16)) {
        speech(m, u, 7, SpeechText::TransportFailed);
        return 8;
    }
    if (u.cargo_head) return 8;
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        if (cargo->footprint_x > static_cast<i16>(targeting_type(m, u).transport_size)) {
            speech(m, u, 7, SpeechText::TooHeavy);
            return 8;
        }
        announce(m, o, SpeechText::Loading);
        take_off(m, u, o);
        return mission_result::kAdvance;
    case 1: {
        GoalRequest g;
        g.kind = GoalRequest::Kind::Follow;
        g.target = cargo;
        g.set(Set::Altitude, cruise_altitude(m, u)).set(Set::Radius, 0x30);
        attach(m, o, g);
        o.wait = 0x100E8;
        return mission_result::kAdvance;
    }
    case 2:
        announce(m, o, SpeechText::PreparingTransport);
        o.param0 = -1;
        script_query(m, u, "QueryTransport", &o.param0);
        o.wait = 0x100E8;
        return mission_result::kAdvance;
    case 3: {
        const i32 h = cargo->type ? cargo->type->model_max_y : 0;
        script_call_args(m, u, "BeginTransport", true, 1, h);
        ctx(m).systems.broadcast_script_call(u, "BeginTransport", 1, h, 0, 0, 0);
        const auto off = piece_local_offset(u, o.param0);
        GoalRequest g;
        g.kind = GoalRequest::Kind::Dock;
        g.target = cargo;
        g.piece = -1;
        g.set(Set::Altitude, -hi16(off[1]));
        attach(m, o, g);
        o.wait = 0x100EA;
        return mission_result::kAdvance;
    }
    case 4: {
        if (event & 0x42) {
            script_call(m, u, "EndTransport", false);
            return 8;
        }
        if (!m.construction) {
            m.fault = "transport packet without the construction systems";
            return mission_result::kClearAll;
        }
        m.construction->systems.transport_attach(*cargo, &u, static_cast<u8>(o.param0), 0);
        speech(m, u, 0xC);
        // A climb to cruise altitude that is never attached (the original leaks it).
        GoalRequest g = point_goal(u.pos);
        g.set(Set::Altitude, cruise_altitude(m, u));
        make_goal(m, o, g);
        o.wait |= 0xE0;
        return mission_result::kAdvance;
    }
    case 5:
        return mission_result::kFinished;
    default:
        return mission_result::kClearAll;
    }
}

// The cargo's footprint fits at the order's position (0x47DB70, ground layer).
bool cargo_fits(Missions& m, const Unit& cargo, const Vec& pos) {
    if (!m.construction) {
        m.fault = "placement check without the construction systems";
        return false;
    }
    const i16 cx = static_cast<i16>(wadd(wsub(pos[0], wshl(cargo.footprint_x, 19)), 0x80000) >> 20);
    const i16 cz = static_cast<i16>(wadd(wsub(pos[2], wshl(cargo.footprint_z, 19)), 0x80000) >> 20);
    return m.construction->systems.can_place(tick_of(m, cargo).type_index, 0, cx, cz, 1);
}

// --- VTOL_Unload (65) -----------------------------------------------------
// An air transport drops its first cargo at the order's position: fly there at cruise
// altitude, descend to the cargo's height if the spot is free, release it and climb.
std::int32_t air_unload(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (!u.cargo_head) return mission_result::kFinished;
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        announce(m, o, SpeechText::Unloading);
        relink_unit(m.orders, o.target, u.cargo_head);
        attach(m, o, point_goal(o.pos).set(Set::Altitude, cruise_altitude(m, u)).set(Set::Radius, 0x140));
        o.wait = 0xE8;
        return mission_result::kAdvance;
    case 1:
    case 2: {
        if (o.step == 2 && (event & 0x40)) return mission_result::kFinishOrIdle;
        Unit* cargo = target_of(o);
        if (!cargo) {
            m.fault = "unload without a cargo link (the original reads through a null pointer)";
            return mission_result::kClearAll;
        }
        if (!cargo_fits(m, *cargo, o.pos)) {
            speech(m, u, 7, SpeechText::UnableToUnload);
            return mission_result::kFinishOrIdle;
        }
        if (o.step == 1) {
            const Unit& first = *u.cargo_head;
            attach(m, o, point_goal(o.pos).set(Set::Altitude, first.type ? hi16(first.type->model_max_y) : 0));
            o.wait = 0xE8;
            return mission_result::kAdvance;
        }
        script_call(m, u, "EndTransport", false);
        if (!m.construction) return mission_result::kClearAll;
        m.construction->systems.transport_attach(*u.cargo_head, nullptr, 0xFF, 1);
        attach(m, o, point_goal(u.pos).set(Set::Altitude, cruise_altitude(m, u)));
        o.wait = 0xE0;
        return mission_result::kAdvance;
    }
    case 3:
        speech(m, u, 0xD);
        return mission_result::kFinished;
    default:
        return mission_result::kClearAll;
    }
}

// A landing pad piece of `pad` is free (0x47E570): the pad is not carried itself and none of
// its cargo sits on that piece.
bool pad_free(const Unit& pad, i32 piece) {
    if (pad.carrier) return false;
    for (const Unit* c = pad.cargo_head; c; c = c->cargo_next)
        if (static_cast<i32>(static_cast<std::int8_t>(c->transport_piece)) == piece) return false;
    return true;
}

// The pad's script names up to four landing pieces (QueryLandingPad); the first free one, or -1.
i32 free_pad_piece(Missions& m, Unit& pad) {
    i32 p[4] = {-1, -1, -1, -1};
    script_query(m, pad, "QueryLandingPad", &p[0], &p[1], &p[2], &p[3]);
    for (i32 v : p)
        if (v != -1 && pad_free(pad, v)) return v;
    return -1;
}

// --- VTOL_Landing (53) ----------------------------------------------------
// Land on an air base (the order's target): circle it until its script offers a free pad
// piece, approach, settle on the piece and become its cargo; a damaged aircraft on a
// finished air base builder is then repaired (SelfRepair).
std::int32_t air_landing(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    Unit* pad = target_of(o);
    const i32 range = slot_range(m, u, 0);  // read up front, whatever the step
    if (!pad) {
        speech(m, u, 7, SpeechText::LandingAborted);
        return 8;
    }
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        announce(m, o, SpeechText::Landing);
        take_off(m, u, o);
        o.param0 = static_cast<i32>(m.orders.rng.next_mod(0x10000));
        return mission_result::kAdvance;
    case 1: {
        i32 piece = free_pad_piece(m, *pad);
        pad = target_of(o);
        if (piece == -1 || !pad_free(*pad, piece)) piece = free_pad_piece(m, *pad);
        if (piece != -1) {
            o.step = 2;
            return mission_result::kContinue;
        }
        const Vec p = ahead(pad->pos, static_cast<u16>(o.param0), wshl(range, 16));
        o.param0 = wadd(o.param0, 0x4000);
        attach(m, o, point_goal(p).set(Set::Radius, 0x80));
        o.wait = 0xE8;
        o.step = 1;
        return mission_result::kContinue;
    }
    case 2:
    case 3: {
        GoalRequest g;
        g.kind = GoalRequest::Kind::Dock;
        g.target = pad;
        g.piece = -1;
        if (o.step == 3) {
            o.param0 = free_pad_piece(m, *pad);
            if (o.param0 == -1) {
                speech(m, u, 7, SpeechText::LandingFailed);
                return mission_result::kRestart;
            }
            g.target = target_of(o);
            g.piece = static_cast<i16>(o.param0);
        }
        g.set(Set::Radius, o.step == 3 ? 0x30 : 0xA0);
        attach(m, o, g);
        o.wait = 0xE8;
        return mission_result::kAdvance;
    }
    case 4:
        return mission_result::kAdvance;
    case 5: {
        if (event & 0x20) return mission_result::kAdvance;
        i32 piece = o.param0;
        if (piece == -1 || !pad_free(*pad, piece)) piece = free_pad_piece(m, *pad);
        o.param0 = piece;
        if (piece == -1) {
            speech(m, u, 7, SpeechText::PadsOccupied);
            return mission_result::kRestart;
        }
        GoalRequest g;
        g.kind = GoalRequest::Kind::Dock;
        g.target = target_of(o);
        g.piece = static_cast<i16>(o.param0);
        g.set(Set::Altitude, u.cargo_head && u.cargo_head->type ? hi16(u.cargo_head->type->model_max_y) : 0);
        auto goal = make_goal(m, o, g);
        script_call(m, u, "EndTransport", true);
        attach_made(m, o, std::move(goal));
        set_deadline(m, o, 15);
        o.step = 5;
        o.wait |= 0xE8;
        return mission_result::kContinue;
    }
    case 6: {
        if (event & 0x40) return 8;
        if (!pad_free(*pad, o.param0)) {
            speech(m, u, 7, SpeechText::NoPads);
            return mission_result::kRestart;
        }
        if (!m.construction) {
            m.fault = "transport packet without the construction systems";
            return mission_result::kClearAll;
        }
        if (u.cargo_head) {
            script_call(m, u, "EndTransport", false);
            m.construction->systems.transport_attach(*u.cargo_head, target_of(o), static_cast<u8>(o.param0), 0);
            return mission_result::kFinished;
        }
        m.construction->systems.transport_attach(u, target_of(o), static_cast<u8>(o.param0), 0);
        Unit* base = target_of(o);
        if (static_cast<u32>(static_cast<i32>(u.hit_points)) < max_damage(u) && (type_flags(m, *base) & kTypeAirBase) &&
            (type_flags(m, *base) & kTypeBuilder) && is_zero(base->build_fraction_left)) {
            drop_goal(m, o);
            prepend_new(m, u, mission::kSelfRepair, target_of(o), nullptr);
        }
        return mission_result::kFinished;
    }
    default:
        return mission_result::kClearAll;
    }
}

// Attack orders end into the air search: a VTOL_SeekAttack at the end of the list when
// nothing follows and the unit may fire (event bits 0x1000A: cancelled / target lost).
// True when the order is over.
bool attack_order_over(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (event & 0x1000A) {
        if (!o.next && (u.flags & kFireOrders)) append_new(m, u, mission::kAirSeekAttack, target_of(o), o.pos.data());
        return true;
    }
    if (!target_of(o)) {
        if (o.flags & 0x200) {
            // The target is gone.
            if (!o.next) append_new(m, u, mission::kAirSeekAttack, nullptr, u.pos.data());
            return true;
        }
    } else {
        o.pos = target_of(o)->pos;
    }
    return false;
}

// An attack with a leash (param2, from the maneuver standing order) ends once the unit is that
// far (whole world units) from where the chase started (scratch0: whole x, z).
bool beyond_leash(const Unit& u, const MissionOrder& o) {
    if (o.param2 == 0) return false;
    const i32 dz = hi16(u.pos[2]) - static_cast<i16>(o.scratch0 >> 16);
    const i32 dx = hi16(u.pos[0]) - static_cast<i16>(o.scratch0 & 0xFFFF);
    return o.param2 <= x87::ftol32(x87::crt_hypot(static_cast<double>(dx), static_cast<double>(dz)));
}

// --- AirStrike (2) --------------------------------------------------------
// Bombing run: take off; if close to the target fly 2240 units out to get a run-up, swing
// in from a random side, run at the target releasing the bombs (slot 0 aims at the ground
// point) when the drop lead distance is reached, fly on 1440 units, and repeat until
// badly damaged (then land at an air base).
std::int32_t air_strike(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (attack_order_over(m, u, o, event)) return mission_result::kFinished;
    if (beyond_leash(u, o)) return mission_result::kFinished;
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        announce(m, o, SpeechText::Attacking);
        take_off(m, u, o);
        return mission_result::kAdvance;
    case 1:
        release_weapon_targets(u, 3);
        unrelease_weapon_slot(u, 0);
        if (flat_distance(o.pos, u.pos) < 0x1E00000) {
            attach(m, o, point_goal(ahead(u.pos, heading_between(u.pos, o.pos), 0x8C00000)).set(Set::Radius, 0x3C0));
            o.wait |= 0xE2;
        }
        return mission_result::kAdvance;
    case 2: {
        const i32 d = flat_distance(o.pos, u.pos);
        const u16 a = heading_between(u.pos, o.pos);
        const i32 half = d / 2;
        const u16 side = static_cast<u16>(m.orders.rng.next_mod(0x4000) + a - 0x2000);
        attach(m, o, point_goal(ahead(u.pos, side, half)).set(Set::Radius, 0x1E0));
        o.wait = 0x100E8;
        return mission_result::kAdvance;
    }
    case 3:
        return mission_result::kAdvance;
    case 4: {
        if (event & 0xE0) return mission_result::kAdvance;
        const i32 g = ctx(m).env.gravity;
        if (g == 0) return mission_result::kClearAll;
        // Lead distance: fall time from cruise altitude times the speed (whole units per step).
        const double fall = std::sqrt((2.0 * static_cast<double>(cruise_altitude(m, u))) / static_cast<double>(g));
        const i32 speed = hi16(tick_of(m, u).motion.speed);
        const i32 lead = x87::ftol32(fall * 30.0 * static_cast<double>(speed));
        const i32 radius = wadd(wadd(lead, 1), static_cast<i32>(extra_type(m, u).attack_run_length));
        GoalRequest goal;
        if (target_of(o)) {
            goal.kind = GoalRequest::Kind::Follow;
            goal.target = target_of(o);
        } else {
            goal = point_goal(o.pos);
        }
        attach(m, o, goal.set(Set::Radius, radius));
        set_deadline(m, o, 1);
        o.wait |= 0x100E8;
        return mission_result::kContinue;
    }
    case 5: {
        unrelease_weapon_slot(u, 0);
        aim_slot_at_point(u, o.pos, 0);
        const u16 a = heading_between(u.pos, o.pos);
        const i32 r = wshl(static_cast<i32>(extra_type(m, u).attack_run_length) + 0x3C0, 16);
        attach(m, o, point_goal(ahead(u.pos, a, r)).set(Set::Radius, 0x3C0));
        o.wait = 0xE2;
        return mission_result::kAdvance;
    }
    case 6:
        clear_weapon_target(u, 0);
        attach(m, o, point_goal(ahead(u.pos, u.heading, 0x5A00000)).set(Set::Radius, 0x80));
        o.wait = 0xE2;
        if (!badly_damaged(u)) {
            o.step = 3;
            return mission_result::kContinue;
        }
        seek_air_base(m, u, o);
        return mission_result::kRestart;
    default:
        return mission_result::kClearAll;
    }
}

// --- AirToGround (4) ------------------------------------------------------
// Gunship / fighter attack on a ground target: take off, swing in from a random side, fly at
// the target with weapon 0 aimed at it, overshoot to three times the weapon range, then
// break left or right and come round again; badly damaged, land at an air base.
std::int32_t air_to_ground(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    const i32 range = slot_range(m, u, 0);
    if (attack_order_over(m, u, o, event)) return mission_result::kFinished;
    if (u.bucket == kOffMapBucket) {
        set_deadline(m, o, 30);
        o.step = 2;
    }
    if (beyond_leash(u, o)) return mission_result::kFinished;
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        announce(m, o, SpeechText::Attacking);
        take_off(m, u, o);
        return mission_result::kAdvance;
    case 1: {
        release_weapon_targets(u, 3);
        const i32 d = flat_distance(o.pos, u.pos);
        const u16 a = heading_between(u.pos, o.pos);
        const i32 half = d / 2;
        const u16 side = static_cast<u16>(m.orders.rng.next_mod(0x4000) + a - 0x2000);
        attach(m, o, point_goal(ahead(u.pos, side, half)).set(Set::Radius, 0x80));
        o.wait = 0x100E8;
        return mission_result::kAdvance;
    }
    case 2:
        unrelease_weapon_slot(u, 0);
        if (Unit* t = target_of(o)) aim_weapon_at_unit(u, *t, 0);
        else aim_slot_at_point(u, o.pos, 0);
        attach(m, o, point_goal(o.pos).set(Set::Radius, range));
        o.wait = 0x100E8;
        return mission_result::kAdvance;
    case 3: {
        const u16 a = heading_between(u.pos, o.pos);
        auto goal = make_goal(m, o, point_goal(ahead(o.pos, a, wshl(static_cast<i32>(static_cast<u32>(range) * 3u), 16))));
        // (the radius is drawn after the goal is built)
        const i32 r = static_cast<i32>(m.orders.rng.next_mod(0x80)) + 0x80;
        set_goal_radius(m, o, goal, r);
        attach_made(m, o, std::move(goal));
        o.wait = 0x100EA;
        return mission_result::kAdvance;
    }
    case 4: {
        if (badly_damaged(u) && seek_air_base(m, u, o)) return mission_result::kRestart;
        const u16 turn = m.orders.rng.next_mod(2) ? static_cast<u16>(u.heading + 0x4000) : static_cast<u16>(u.heading - 0x4000);
        attach(m, o, point_goal(ahead(u.pos, turn, wshl(range, 16))).set(Set::Radius, 0x80));
        o.wait = 0x100EA;
        return mission_result::kAdvance;
    }
    case 5:
        o.step = 2;
        return mission_result::kContinue;
    default:
        return mission_result::kClearAll;
    }
}

// The low 16 bits of the original's "dot product" of two direction vectors, of which only
// the whole parts (high words) count: positive when they point the same way.
bool facing_same_way(const Vec& a, const Vec& b) {
    const u32 dot = static_cast<u32>(hi16(a[0]) * hi16(b[0])) + static_cast<u32>(hi16(a[2]) * hi16(b[2]));
    return static_cast<i16>(dot & 0xFFFF) > 0;
}

// --- AirToAir (3) ---------------------------------------------------------
// Dogfight: weapon 0 aims at the target; the fighter chases a point 45 steps ahead of the
// target (param0 counts how long the target is not in front); after a goal event while
// facing the target it extends straight on, otherwise (or after too long) it evades.
std::int32_t air_to_air(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (event & 0x10008) {
        if (!o.next && (u.flags & kFireOrders)) append_new(m, u, mission::kAirSeekAttack, target_of(o), o.pos.data());
        return mission_result::kFinished;
    }
    if (!target_of(o) && (o.flags & 0x200)) {
        if (!o.next) append_new(m, u, mission::kAirSeekAttack, nullptr, u.pos.data());
        return mission_result::kFinished;
    }
    if (u.bucket == kOffMapBucket) return head_back_onto_map(m, u, o);
    if (beyond_leash(u, o)) return mission_result::kFinished;
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        announce(m, o, SpeechText::Attacking);
        take_off(m, u, o);
        set_deadline(m, o, 1);
        o.param0 = 0;
        return mission_result::kAdvance;
    case 1:
        break;
    default:
        return mission_result::kClearAll;
    }
    release_weapon_targets(u, 3);
    unrelease_weapon_slot(u, 0);
    Unit* t = target_of(o);
    if (!t) {
        m.fault = "air-to-air attack without a target (the original reads through a null pointer)";
        return mission_result::kClearAll;
    }
    aim_weapon_at_unit(u, *t, 0);
    const u32 goal_event = event & 0xE0;
    auto toward_target = [&]() { return ahead(Vec{0, 0, 0}, heading_between(u.pos, t->pos), 0x140000); };
    auto own_heading = [&]() { return ahead(Vec{0, 0, 0}, u.heading, 0x140000); };
    if (goal_event) {
        const Vec to = toward_target();
        const Vec dir = own_heading();
        if (facing_same_way(to, dir)) {
            // Extend: a manoeuvre point 30 steps of full speed ahead, moving with full speed.
            const i32 vmax = motion_type(m, u).max_velocity;
            const Vec p = ahead(u.pos, u.heading, static_cast<i32>(static_cast<u32>(vmax) * 30u));
            const Vec v = ahead(Vec{0, 0, 0}, u.heading, vmax);
            GoalRequest g;
            g.kind = GoalRequest::Kind::Manoeuvre;
            g.pos = p;
            g.vel = v;
            g.set(Set::ManoeuvreAltitude, cruise_altitude(m, u));
            attach(m, o, g);
            set_deadline(m, o, m.orders.rng.next_mod(30) + 60);
            o.param0 = 0;
            return mission_result::kContinue;
        }
    } else if (o.param0 < 0x5A) {
        const Vec to = toward_target();
        const Vec dir = own_heading();
        if (facing_same_way(to, dir)) o.param0 = 0;
        else o.param0 = wadd(o.param0, 0x2D);
        const i32 d = flat_distance(u.pos, t->pos);
        if (static_cast<i16>(static_cast<u32>(d) >> 16) > 0xA0) {
            if (!tick_of(m, *t).has_motion) {
                m.fault = "air-to-air lead on a target without motion (the original reads through a null pointer)";
                return mission_result::kClearAll;
            }
            const Vec& tv = tick_of(m, *t).velocity;
            Vec p = t->pos;
            p[0] = wadd(p[0], static_cast<i32>(static_cast<u32>(tv[0]) * 45u));
            p[2] = wadd(p[2], static_cast<i32>(static_cast<u32>(tv[2]) * 45u));
            const i32 half = motion_type(m, *t).max_velocity / 2;
            const Vec side = ahead(Vec{0, 0, 0}, t->heading, half);
            GoalRequest g;
            g.kind = GoalRequest::Kind::Manoeuvre;
            g.pos = p;
            g.vel = {wadd(tv[0], side[0]), tv[1], wadd(tv[2], side[2])};
            attach(m, o, g);
        }
        set_deadline(m, o, 45);
        o.wait |= 0x100E8;
        return mission_result::kContinue;
    }
    drop_goal(m, o);
    prepend_new(m, u, mission::kAirEvade, target_of(o), nullptr);
    o.param0 = 0;
    o.wait = 0;
    return mission_result::kRestart;
}

// --- AirToGroundHover (5) -------------------------------------------------
// Gunship attack that hovers: swing in, close to weapon range, then hold a point two thirds
// of the range from the target, alternating 45 degrees left and right of the line to it,
// facing it; out of range twice running, a random point at range is set up (but never
// attached, as in the original). Badly damaged: land at an air base.
std::int32_t air_to_ground_hover(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (event & 0x10008) {
        if (!o.next && (u.flags & kFireOrders)) append_new(m, u, mission::kAirSeekAttack, target_of(o), o.pos.data());
        return mission_result::kFinished;
    }
    if (!target_of(o) && (o.flags & 0x200)) {
        if (!o.next) append_new(m, u, mission::kAirSeekAttack, nullptr, u.pos.data());
        return mission_result::kFinished;
    }
    if (u.bucket == kOffMapBucket) return head_back_onto_map(m, u, o);
    if (beyond_leash(u, o)) return mission_result::kFinished;
    const i32 range = slot_range(m, u, 0);
    if (o.step == 0) {
        if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
        announce(m, o, SpeechText::Attacking);
        take_off(m, u, o);
        return mission_result::kAdvance;
    }
    if (o.step > 3) return mission_result::kClearAll;
    Unit* t = target_of(o);
    if (!t) {
        m.fault = "hover attack without a target (the original reads through a null pointer)";
        return mission_result::kClearAll;
    }
    switch (o.step) {
    case 1: {
        release_weapon_targets(u, 3);
        const i32 d = flat_distance(t->pos, u.pos);
        const u16 a = heading_between(u.pos, t->pos);
        const i32 half = d / 2;
        const u16 side = static_cast<u16>(m.orders.rng.next_mod(0x4000) + a - 0x2000);
        attach(m, o, point_goal(ahead(u.pos, side, half)).set(Set::Radius, 0x80));
        o.wait = 0x100E8;
        return mission_result::kAdvance;
    }
    case 2:
        unrelease_weapon_slot(u, 0);
        aim_weapon_at_unit(u, *t, 0);
        attach(m, o, point_goal(t->pos).set(Set::Radius, range));
        o.wait = 0x100E8;
        o.param0 = 0;
        o.param1 = 0;
        return mission_result::kAdvance;
    default:
        break;
    }
    if (!weapon_reaches_unit(m, u, *t, 0)) o.param1 = wadd(o.param1, 1);
    if (o.param1 >= 2) {
        o.param1 = 0;
        const u16 a = static_cast<u16>(m.orders.rng.next_mod(0x10000));
        make_goal(m, o, point_goal(ahead(t->pos, a, wshl(range, 16))).set(Set::Radius, 0x80));
        o.wait |= 0x110E8;
        return mission_result::kContinue;
    }
    u16 a = heading_between(u.pos, t->pos);
    if (o.param0 != 0) {
        a = static_cast<u16>(a + 0x2000);
        o.param0 = 0;
    } else {
        a = static_cast<u16>(a - 0x2000);
        o.param0 = 1;
    }
    const i32 r = wshl(static_cast<i32>(static_cast<u32>(range) * 2u) / 3, 16);
    GoalRequest g;
    g.kind = GoalRequest::Kind::Face;
    g.target = t;
    g.pos = {wadd(t->pos[0], trig::mul_sin(a, r)), t->pos[1], wadd(t->pos[2], trig::mul_cos(a, r))};
    g.set(Set::Radius, 0x10).set(Set::Altitude, cruise_altitude(m, u));
    attach(m, o, g);
    o.wait = 0x100E8;
    if (badly_damaged(u) && seek_air_base(m, u, o)) return mission_result::kRestart;
    return mission_result::kContinue;
}

// --- VTOL_Evade (48) ------------------------------------------------------
// Break off: turn 90 degrees to a random side for the weapon range, then twice the range
// the same way, then finish (the attack that prepended it resumes).
std::int32_t air_evade(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    const i32 range = slot_range(m, u, 0);
    if (!target_of(o) || (event & 0x10008)) return mission_result::kFinished;
    switch (o.step) {
    case 0:
    case 1: {
        if (o.step == 0) {
            if (!has_motion(m, u) || !flies(m, u)) return mission_result::kClearAll;
            o.param0 = static_cast<i32>(m.orders.rng.next_mod(2));
        }
        const i32 dist = wshl(range, o.step == 0 ? 16 : 17);
        const u16 h = o.param0 ? static_cast<u16>(u.heading - 0x4000) : static_cast<u16>(u.heading + 0x4000);
        attach(m, o, point_goal(ahead(u.pos, h, dist)).set(Set::Radius, 0x80));
        o.wait = 0x100E8;
        return mission_result::kAdvance;
    }
    case 2:
        return mission_result::kFinished;
    default:
        return mission_result::kClearAll;
    }
}

// --- VTOL_GetRepaired (50) ------------------------------------------------
// Sitting on a repair pad (the order's target): wait until whole again.
std::int32_t air_get_repaired(Missions& m, Unit& u, MissionOrder& o) {
    if (!target_of(o)) {
        speech(m, u, 7, SpeechText::RepairAborted);
        return 8;
    }
    switch (o.step) {
    case 0:
        if (static_cast<u32>(static_cast<i32>(u.hit_points)) < max_damage(u)) {
            set_deadline(m, o, 30);
            o.wait |= 8;
            return mission_result::kContinue;
        }
        return mission_result::kAdvance;
    case 1:
        speech(m, u, 10, SpeechText::UnitRepaired);
        return mission_result::kFinished;
    default:
        return mission_result::kClearAll;
    }
}

// Player resources the repair patrols weigh (stored amount and storage capacity).
struct Stock {
    float energy = 0.0f, energy_cap = 0.0f, metal = 0.0f, metal_cap = 0.0f;
};
Stock stock_of(Missions& m, const Unit& u) {
    Stock s;
    const int p = tick_of(m, u).owner;
    const PlayerSlot& slot = m.orders.players[static_cast<std::size_t>(p)];
    s.energy = slot.energy_stored;
    s.metal = slot.metal_stored;
    const std::vector<PlayerEconomy>* e = ctx(m).economy;
    if (e && static_cast<std::size_t>(p) < e->size()) {
        s.energy_cap = (*e)[static_cast<std::size_t>(p)].energy_storage;
        s.metal_cap = (*e)[static_cast<std::size_t>(p)].metal_storage;
    }
    return s;
}
// "At least a fifth full" as the original's FCOMPP tests it (unordered counts as true).
bool fifth_full(float amount, float cap) { return !(static_cast<double>(cap) * 0.2 > static_cast<double>(amount)); }
// "Room for `more`" (amount + more <= capacity, the sum kept at double precision; unordered
// counts as true).
bool room_for(float amount, float more, float cap) {
    return !(static_cast<double>(amount) + static_cast<double>(more) > static_cast<double>(cap));
}

// --- VTOL_RepairPatrol (60) -----------------------------------------------
// An air builder's patrol leg: repair or help finish damaged allies in sight, reclaim wrecks
// when energy or metal runs low; badly damaged, land at an air base. When it looks for wrecks
// the event argument ends up pointing at its metal site record (a stack address, an input).
std::int32_t air_repair_patrol(Missions& m, Unit& u, MissionOrder& o, u32& event) {
    if (event & 0x48) {
        set_deadline(m, o, 30);
        return mission_result::kRestart;
    }
    if (o.step == 0) {
        if (!has_motion(m, u) || !flies(m, u) || !(mission_type(m, u).abilities & kAbilityRepair)) return mission_result::kClearAll;
        if (Unit* t = target_of(o)) o.pos = t->pos;
        ensure_patrol_return(m, u, o);
        announce(m, o, SpeechText::Patrolling);
        take_off(m, u, o);
        return mission_result::kAdvance;
    }
    if (o.step != 1) return mission_result::kClearAll;
    if (event & 0xE0) return mission_result::kRotate;
    attach(m, o, point_goal(o.pos).set(Set::Altitude, cruise_altitude(m, u)));
    set_deadline(m, o, 45);
    o.wait |= 0xE0;
    if (badly_damaged(u) && seek_air_base(m, u, o)) return mission_result::kRestart;
    Stock s = stock_of(m, u);
    if (fifth_full(s.energy, s.energy_cap)) {
        std::vector<Unit*> near;
        ctx(m).systems.units_in_radius(u.pos, wshl(targeting_type(m, u).sight_distance, 16), near);
        UnitList found;
        for (Unit* c : near)
            if (repair_patrol_candidate(m, u, *c)) found.push(m, c);
        if (!found.v.empty()) {
            Unit* c = found.v[m.orders.rng.next_mod(static_cast<u32>(found.v.size()))];
            if (can_repair(m, u, c) && is_zero(c->build_fraction_left))
                return enqueue_repair(m, u, *c, false) ? mission_result::kRotate : mission_result::kSnooze;
            if (can_repair(m, u, c) && !is_zero(c->build_fraction_left)) {
                drop_goal(m, o);
                prepend_new(m, u, mission::kAirHelpBuild, c, nullptr);
                o.wait = 0;
                return mission_result::kSnooze;
            }
        }
    }
    ReclaimSites sites;
    const bool any = pick_reclaim_sites(m, u.pos, 0xF00000, sites);
    event = sites.metal_found ? ctx(m).env.air_repair_patrol_site_address : 0u;
    if (!any) return mission_result::kContinue;
    s = stock_of(m, u);
    const Vec* at = nullptr;
    if (sites.metal_found && !fifth_full(s.metal, s.metal_cap)) at = &sites.metal_pos;
    else if (sites.energy_found && !fifth_full(s.energy, s.energy_cap)) at = &sites.energy_pos;
    else if (sites.metal_found && room_for(s.metal, sites.metal_amount, s.metal_cap)) at = &sites.metal_pos;
    else if (sites.energy_found && room_for(s.energy, sites.energy_amount, s.energy_cap)) at = &sites.energy_pos;
    if (!at) return mission_result::kContinue;
    drop_goal(m, o);
    prepend_new(m, u, mission::kAirReclaim, nullptr, at->data());
    o.wait = 0;
    return mission_result::kSnooze;
}
}  // namespace sim::um

namespace sim {

bool can_land_at(Missions& m, Unit& u, const std::array<std::int32_t, 3>& pos) {
    using namespace um;
    UnitMissions& c = ctx(m);
    const i32 fx = u.footprint_x, fz = u.footprint_z;
    const i16 cx = static_cast<i16>(wadd(wsub(pos[0], wshl(fx, 19)), 0x80000) >> 20);
    const i16 cz = static_cast<i16>(wadd(wsub(pos[2], wshl(fz, 19)), 0x80000) >> 20);
    if (cx < 0 || cz < 0) return false;
    const i32 W = c.map.width;
    if (cx + fx >= W || cz + fz >= c.map.height) return false;
    // Cells the owner has not explored count as free.
    const i32 at = (cx >> 1) + (fx >> 2) + ((cz >> 1) + (fx >> 2)) * (W >> 1);
    if (at < 0 || static_cast<std::size_t>(at) >= c.explored.size()) {
        m.fault = "landing check outside the explored map";
        return false;
    }
    const u8 slot = static_cast<u8>(tick_of(m, u).owner);
    if (!((1u << (slot & 31)) & c.explored[static_cast<std::size_t>(at)])) return true;
    const TargetingTypeInfo& tt = targeting_type(m, u);
    const i32 sea = c.env.sea_level;
    const u32 tf = type_flags(m, u);
    i32 low = sea - tt.max_water_depth;  // the lowest ground height allowed
    if (low < sea && (tf & kTypeCanFly) && !(tf & kTypeAmphibious)) low = sea;
    const i32 high = sea - tt.min_water_depth;
    const i32 slope = construction_type(m, u).max_slope;
    for (i32 z = 0; z < fz; ++z)
        for (i32 x = 0; x < fx; ++x) {
            const MapCell& cell = c.map.cells[static_cast<std::size_t>((cz + z) * W + cx + x)];
            bool blocked;
            const u16 f = cell.feature;
            if (f == 0xFFFF) {
                blocked = false;
            } else if (f < 0xFFFB) {
                blocked = static_cast<i32>(f) >= static_cast<i32>(c.features.size()) || (c.features[f].flags & 0x40);
            } else if (f == 0xFFFE) {
                // A part of a bigger feature: its own cell is back by the row / column offsets.
                const i32 back = static_cast<i32>(cell.feature_aux & 0xFF) * W + static_cast<i32>(cell.feature_aux >> 8);
                const i32 own = (cz + z) * W + cx + x - back;
                if (own < 0 || static_cast<std::size_t>(own) >= c.map.cells.size()) {
                    m.fault = "feature part pointing outside the map";
                    return false;
                }
                const u16 g = c.map.cells[static_cast<std::size_t>(own)].feature;
                if (g >= 0xFFFB) {
                    blocked = false;
                } else if (g >= c.features.size()) {
                    m.fault = "feature type past the table";
                    return false;
                } else {
                    blocked = (c.features[g].flags & 0x40) != 0;
                }
            } else {
                blocked = true;
            }
            if (blocked || (cell.flags & 2)) return false;
            if (cell.unit != 0 && cell.unit != u.id) return false;
            if (cell.unit2 != 0 && cell.unit2 != u.id) return false;
            if (static_cast<i32>(cell.height_min) < low) return false;
            if (high < static_cast<i32>(cell.height_max)) return false;
            if (slope < static_cast<i32>(cell.height_max) - static_cast<i32>(cell.height_min)) return false;
        }
    return true;
}

}  // namespace sim
