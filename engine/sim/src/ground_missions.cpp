// Ground mission behaviours that move the unit: chasing attacks, kamikaze runs, fire at a
// position, following (guarding), ground transports, repair patrols and teleporting
// (see docs/RE_MEMORY.md, "Mission behaviours").
#include "unit_missions_common.h"

#include <cstring>

#include "sim/trig.h"
#include "sim/weapons.h"
#include "sim/x87.h"
#include "sim/x87_math.h"

namespace sim::um {

// --- Attack_Chase (6) -----------------------------------------------------
// A mobile ground unit attacks: slot param0 (the first enabled slot unless given) fires when
// in range; out of range it closes in, trying in turn: full range, random flanking points
// at range (only while level with the target), half range, point blank, then rings around it.
std::int32_t attack_chase(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    const i32 slot = o.param0;
    if ((event & 0x800) || !target_of(o) || (event & 0x10008)) return mission_result::kFinished;
    if (beyond_leash(u, o)) return mission_result::kFinished;
    Unit* t = target_of(o);
    auto check_slot = [&]() {
        if (static_cast<u32>(slot) > 2) {
            m.fault = "attack order on a weapon slot outside 0..2";
            return false;
        }
        return true;
    };
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || flies(m, u) || !(u.flags & kHasWeapons)) return mission_result::kClearAll;
        announce(m, o);
        o.pos = u.pos;
        o.param1 = 0;
        if (slot == 0) o.param0 = first_weapon_slot(u);
        return mission_result::kAdvance;
    case 1:
        drop_goal(m, o);
        if (event & 0x3000) return mission_result::kAdvance;
        if (!check_slot() || !weapon_reaches_unit(m, u, *t, slot)) return mission_result::kAdvance;
        unrelease_weapon_slot(u, 0);
        unrelease_weapon_slot(u, 2);
        aim_weapon_at_unit(u, *t, slot);
        o.wait = 0x13808;
        return mission_result::kContinue;
    case 2: {
        const i32 range = slot_range(m, u, static_cast<u32>(slot));
        switch (static_cast<u32>(o.param1)) {
        case 0:
            circle_goal(m, o, t->pos, range);
            o.param1 = wadd(o.param1, 1);
            return mission_result::kAdvance;
        case 1: case 2: case 3: case 4: {
            const i32 dy = wsub(u.pos[1], t->pos[1]);
            if ((dy < 0 ? wneg(dy) : dy) > 0x80000) {
                circle_goal(m, o, t->pos, range / 2);
                o.param1 = 6;
                return mission_result::kAdvance;
            }
            const u16 a = heading_between(t->pos, u.pos);
            const u16 h = static_cast<u16>(a + m.orders.rng.next_mod(0x8000) - 0x4000);
            const Vec p = ahead(t->pos, h, wshl(range, 16));
            circle_goal(m, o, p, range / 4);
            return mission_result::kAdvance;
        }
        case 5:
            circle_goal(m, o, t->pos, range / 2);
            o.param1 = wadd(o.param1, 1);
            return mission_result::kAdvance;
        case 6:
            circle_goal(m, o, t->pos, 0);
            o.param1 = wadd(o.param1, 1);
            return mission_result::kAdvance;
        case 7:
            ring_goal(m, o, t->pos, range, range / 2);
            o.param1 = wadd(o.param1, 1);
            return mission_result::kAdvance;
        case 8:
            ring_goal(m, o, t->pos, static_cast<i32>(static_cast<u32>(range) * 2u), range);
            o.param1 = 0;
            return mission_result::kAdvance;
        default:
            return mission_result::kClearAll;
        }
    }
    case 3:
        if (event & 0x40E0) {
            o.step = 1;
            return 4;
        }
        if (!check_slot()) return mission_result::kClearAll;
        if (weapon_reaches_unit(m, u, *t, slot)) {
            unrelease_weapon_slot(u, 0);
            unrelease_weapon_slot(u, 2);
            aim_weapon_at_unit(u, *t, slot);
            o.wait = 0x148E8;
        } else {
            release_weapon_targets(u, 3);
            o.wait = 0x100E8;
        }
        set_deadline(m, o, 30);
        return mission_result::kContinue;
    default:
        return mission_result::kClearAll;
    }
}

// --- Attack_Kamikaze (7) --------------------------------------------------
// Run at the target (within its kamikaze distance, at least 16) and blow up there.
std::int32_t attack_kamikaze(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (event & 0x10008) return mission_result::kFinished;
    if (Unit* t = target_of(o)) o.pos = t->pos;
    switch (o.step) {
    case 0: {
        if (u.carrier) return mission_result::kClearAll;
        announce(m, o);
        const u16 d = extra_type(m, u).kamikaze_distance;
        circle_goal(m, o, o.pos, d < 0x10 ? 0x10 : d);
        set_deadline(m, o, 60);
        o.wait |= 0xE0;
        return mission_result::kAdvance;
    }
    case 1:
        if (event & 0x20) {
            speech(m, u, 6);
            prepend_new(m, u, mission::kSelfDestruct, nullptr, nullptr, 1);
            return mission_result::kFinished;
        }
        if (event & 0x40) return 8;
        o.step = 0;
        return mission_result::kContinue;
    default:
        return mission_result::kClearAll;
    }
}

// --- Suppress (46) --------------------------------------------------------
// Fire at a ground position (slot param0; 2 = the special weapon, else slots 0 and 1), moving
// in (param1 = the approach radius, shrinking by a random third of the range each time).
std::int32_t suppress(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (event & 0x800) return mission_result::kFinished;
    switch (o.step) {
    case 0:
        if (flies(m, u)) return 8;
        announce(m, o);
        o.param1 = slot_range(m, u, static_cast<u32>(o.param0));
        return mission_result::kAdvance;
    case 1:
        if (o.param0 == 2) {
            unrelease_weapon_slot(u, 3);
            aim_slot_at_point(u, o.pos, 2);
        } else {
            unrelease_weapon_slot(u, 0);
            unrelease_weapon_slot(u, 1);
            aim_slot_at_point(u, o.pos, 0);
            aim_slot_at_point(u, o.pos, 1);
        }
        o.wait = 0x1C00;
        return mission_result::kAdvance;
    case 2: {
        release_weapon_targets(u, 3);
        if (event & 0x400) {
            o.step = 1;
            return mission_result::kRotate;
        }
        if (!has_motion(m, u) || o.param1 < 1) return mission_result::kFinishOrIdle;
        circle_goal(m, o, o.pos, o.param1);
        o.wait = 0xE0;
        const i32 range = slot_range(m, u, static_cast<u32>(o.param0));
        const u32 r = m.orders.rng.next_mod(static_cast<u32>(range / 3));
        o.step = 1;
        o.param1 = wsub(o.param1, static_cast<i32>(r));
        return 4;
    }
    default:
        return mission_result::kClearAll;
    }
}

// --- Follow_Ground (18) ---------------------------------------------------
// Guard a ground unit: answer its attackers, repair it or join what it builds, else stay
// near it at a random spot around it (the order position holds that offset). The HelpBuild
// kind it may look up lands in the low byte of the event argument.
std::int32_t follow_ground(Missions& m, Unit& u, MissionOrder& o, u32& event) {
    Unit* followed = target_of(o);
    if (!followed) return mission_result::kFinished;
    if (u.carrier) return mission_result::kClearAll;
    if (flies(m, *followed)) return 8;
    if (o.step == 0) {
        announce(m, o, SpeechText::Guarding);
        release_weapon_targets(u, 3);
        const i32 d = (static_cast<i32>(followed->footprint_x) + 2 + static_cast<i32>(u.footprint_x)) * 16;
        o.param0 = d;
        const i32 r = wshl(d, 16);
        const u16 a = static_cast<u16>(m.orders.rng.next_mod(0x10000));
        o.pos = ahead(Vec{0, 0, 0}, a, r);
        return mission_result::kAdvance;
    }
    if (o.step != 1) return mission_result::kClearAll;
    if (answer_attacker(m, u, o, *followed, event)) return mission_result::kSnooze;
    followed = target_of(o);
    if (static_cast<u32>(static_cast<i32>(followed->hit_points)) < max_damage(*followed) && (type_flags(m, u) & kTypeBuilder)) {
        const u8 kind = m.systems.resolve_mission(OrderIntent::Repair, u, followed, nullptr);
        if (kind) {
            drop_goal(m, o);
            prepend_new(m, u, kind, target_of(o), nullptr);
            o.wait = 0;
            return mission_result::kSnooze;
        }
    }
    MissionOrder* head = followed->orders;
    if (head && head->kind != mission::kReady && (type_flags(m, u) & kTypeBuilder) && (type_flags(m, *followed) & kTypeBuilder) &&
        (head->flags & 0x100000) && &u != head->target.unit) {
        const bool building = head->kind == mission::kMobileBuild || head->kind == mission::kBuildingBuild;
        const bool has_object = ((head->flags & 0x200) && head->target.unit) || (head->flags & 0x400);
        u8 kind = 0;
        if (building) {
            if (head->target.unit) {
                drop_goal(m, o);
                event = (event & ~0xFFu) | mission::kHelpBuild;
                kind = mission::kHelpBuild;
            }
        } else if (has_object) {
            drop_goal(m, o);
            kind = target_of(o)->orders->kind;
        }
        if (kind) {
            MissionOrder* h = target_of(o)->orders;
            prepend_new(m, u, kind, h->target.unit, h->pos.data());
            o.wait = 0;
            return mission_result::kSnooze;
        }
    }
    const Vec p = {wadd(followed->pos[0], o.pos[0]), wadd(o.pos[1], followed->pos[1]), wadd(o.pos[2], followed->pos[2])};
    circle_goal(m, o, p, o.param0 / 2);
    set_deadline(m, o, 30);
    o.wait |= 0x18;
    return mission_result::kContinue;
}

// "Wait while the script is busy" (0x438730): busy → wait on `mask` and the script's state change.
std::int32_t wait_if_busy(Unit& u, MissionOrder& o, u32 mask) {
    if (u.script_flags & unit_flags::kBusy) {
        o.wait = mask | 4;
        return mission_result::kContinue;
    }
    return mission_result::kAdvance;
}

// --- Ground_Pickup (20) ---------------------------------------------------
// A ground transport loads the target: its script's TransportPickup does the work (up to
// three tries, moving onto the target in between).
std::int32_t ground_pickup(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    Unit* cargo = target_of(o);
    if (!cargo || (event & 8)) {
        speech(m, u, 7, SpeechText::TransportFailed);
        return 8;
    }
    switch (o.step) {
    case 0:
        if (!has_motion(m, u) || !(mission_type(m, u).abilities & kAbilityLoad)) return mission_result::kClearAll;
        if (cargo->footprint_x > static_cast<i16>(targeting_type(m, u).transport_size)) {
            speech(m, u, 7, SpeechText::TooLarge);
            return 8;
        }
        announce(m, o, SpeechText::LoadingUnit);
        return mission_result::kAdvance;
    case 1:
    case 3:
        return wait_if_busy(u, o, 8);
    case 2:
        script_call_args(m, u, "TransportPickup", true, 1, static_cast<i32>(cargo->id));
        speech(m, u, 0xC);
        o.param0 = wadd(o.param0, 1);
        set_deadline(m, o, 15);
        return mission_result::kAdvance;
    case 4:
        if (cargo->carrier) return mission_result::kFinished;
        if (o.param0 >= 3) return mission_result::kFinishOrIdle;
        circle_goal(m, o, cargo->pos, 0);
        o.wait = 0xE8;
        return mission_result::kAdvance;
    case 5:
        drop_goal(m, o);
        return mission_result::kRestart;
    default:
        return mission_result::kClearAll;
    }
}

// --- Ground_Unload (21) ---------------------------------------------------
// A ground transport unloads its first cargo at the order's position (TransportDrop with the
// cargo and the packed position; up to three tries).
std::int32_t ground_unload(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (event & 8) {
        speech(m, u, 7, SpeechText::UnloadingSlow);
        return 8;
    }
    switch (o.step) {
    case 0: {
        if (!has_motion(m, u) || !(mission_type(m, u).abilities & kAbilityLoad)) return mission_result::kClearAll;
        relink_unit(m.orders, o.target, u.cargo_head);
        if (!target_of(o)) return mission_result::kFinished;
        announce(m, o, SpeechText::Unloading);
        const i32 packed = wadd(static_cast<i32>(static_cast<u32>(o.pos[0]) & 0xFFFF0000u), o.pos[2] >> 16);
        // (one argument is declared although two are passed, as in the original)
        script_call_args(m, u, "TransportDrop", true, 1, static_cast<i32>(target_of(o)->id), packed);
        o.param0 = wadd(o.param0, 1);
        set_deadline(m, o, 15);
        return mission_result::kAdvance;
    }
    case 1:
        return wait_if_busy(u, o, 8);
    case 2: {
        Unit* cargo = target_of(o);
        if (!cargo) {
            m.fault = "unload without a cargo link (the original reads through a null pointer)";
            return mission_result::kClearAll;
        }
        if (cargo->carrier != &u) {
            speech(m, u, 0xD);
            return mission_result::kFinished;
        }
        if (o.param0 >= 3) return mission_result::kFinishOrIdle;
        i32 radius = 0;
        if (type_flags(m, u) & kTypeHover) {
            const i32 size_z = u.type ? hi16(wsub(u.type->model_max_z, u.type->model_min_z)) : 0;
            radius = x87::ftol32(static_cast<double>(size_z) * 1.5);
        }
        circle_goal(m, o, o.pos, radius);
        o.wait = 0xE8;
        return mission_result::kAdvance;
    }
    case 3:
        return mission_result::kRestart;
    default:
        return mission_result::kClearAll;
    }
}

namespace {
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
bool fifth_full(float amount, float cap) { return !(static_cast<double>(cap) * 0.2 > static_cast<double>(amount)); }
bool room_for(float amount, float more, float cap) {
    return !(static_cast<double>(amount) + static_cast<double>(more) > static_cast<double>(cap));
}
}  // namespace

// --- RepairPatrol (34) ----------------------------------------------------
// A ground builder's patrol leg: repair damaged allies in sight while energy lasts, reclaim
// wrecks when energy or metal runs low. When it looks for wrecks the event argument ends up
// holding the metal amount found (float bits) or 0.
std::int32_t repair_patrol(Missions& m, Unit& u, MissionOrder& o, u32& event) {
    if (o.step == 0) {
        if (Unit* t = target_of(o)) o.pos = t->pos;
        ensure_patrol_return(m, u, o);
        return mission_result::kAdvance;
    }
    if (o.step != 1) return mission_result::kClearAll;
    if (event & 0xE0) return mission_result::kRotate;
    circle_goal(m, o, o.pos, 0x10);
    set_deadline(m, o, 60);
    o.wait |= 0xE0;
    Stock s = stock_of(m, u);
    if (fifth_full(s.energy, s.energy_cap)) {
        std::vector<Unit*> near;
        ctx(m).systems.units_in_radius(u.pos, wshl(targeting_type(m, u).sight_distance, 16), near);
        UnitList found;
        for (Unit* c : near)
            if (repair_patrol_candidate(m, u, *c)) found.push(m, c);
        if (!found.v.empty()) {
            Unit* c = found.v[m.orders.rng.next_mod(static_cast<u32>(found.v.size()))];
            if (allied(m, tick_of(m, u).owner, *c) && m.systems.resolve_mission(OrderIntent::Repair, u, c, nullptr))
                return enqueue_repair(m, u, *c, false) ? mission_result::kRotate : mission_result::kSnooze;
        }
    }
    s = stock_of(m, u);
    if (fifth_full(s.energy, s.energy_cap) && fifth_full(s.metal, s.metal_cap)) return mission_result::kContinue;
    ReclaimSites sites;
    const bool any = pick_reclaim_sites(m, u.pos, wshl(targeting_type(m, u).sight_distance, 16), sites);
    if (sites.metal_found) std::memcpy(&event, &sites.metal_amount, 4);
    else event = 0;
    if (!any) return mission_result::kContinue;
    s = stock_of(m, u);
    if (sites.metal_found && !fifth_full(s.metal, s.metal_cap)) {
        drop_goal(m, o);
        prepend_new(m, u, mission::kReclaim, nullptr, sites.metal_pos.data());
        drop_goal(m, o);
        o.wait = 0;
        return mission_result::kSnooze;
    }
    const Vec* at = nullptr;
    if (sites.energy_found && !fifth_full(s.energy, s.energy_cap)) at = &sites.energy_pos;
    else if (sites.metal_found && room_for(s.metal, sites.metal_amount, s.metal_cap)) at = &sites.metal_pos;
    else if (sites.energy_found && room_for(s.energy, sites.energy_amount, s.energy_cap)) at = &sites.energy_pos;
    if (!at) return mission_result::kContinue;
    drop_goal(m, o);
    prepend_new(m, u, mission::kReclaim, nullptr, at->data());
    o.wait = 0;
    return mission_result::kSnooze;
}

// --- Teleport (47) --------------------------------------------------------
// Every table entry (the empty ones too) standing inside the teleporter's model box moves by
// the offset between the order's position and the teleporter (a trail effect marks it).
std::int32_t teleport(Missions& m, Unit& u, MissionOrder& o) {
    const UnitTypeInfo none;
    const UnitTypeInfo& t = u.type ? *u.type : none;
    const i32 min_x = wadd(u.pos[0], t.model_min_x), max_x = wadd(t.model_max_x, u.pos[0]);
    const i32 min_y = wadd(t.model_min_y, u.pos[1]), max_y = wadd(u.pos[1], t.model_max_y);
    const i32 min_z = wadd(t.model_min_z, u.pos[2]), max_z = wadd(t.model_max_z, u.pos[2]);
    for (Unit& w : m.units) {
        const i32 x = w.pos[0], y = w.pos[1], z = w.pos[2];
        if (!(min_x <= x && x <= max_x && min_z <= z && z <= max_z && min_y <= y && y <= max_y && &w != &u)) continue;
        const Vec to = {wsub(wadd(o.pos[0], x), u.pos[0]), wsub(wadd(o.pos[1], y), u.pos[1]), wsub(wadd(o.pos[2], z), u.pos[2])};
        ctx(m).systems.teleport_trail(w.pos, to);
        ctx(m).systems.place_unit(w, to, 1);
    }
    return mission_result::kFinished;
}

}  // namespace sim::um
