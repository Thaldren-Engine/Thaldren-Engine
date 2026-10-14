#include "sim/missions.h"

#include "sim/weapons.h"
#include "sim/x87.h"

namespace sim {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

i32 wadd(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
i32 wsub(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
// Whole world units of a 16.16 value (its high word, signed).
i32 hi16(i32 v) { return static_cast<std::int16_t>(static_cast<u32>(v) >> 16); }
// High dword of the 64-bit square of a signed value.
i32 square_hi(i32 v) { return static_cast<i32>(static_cast<u32>(static_cast<std::uint64_t>(static_cast<i64>(v) * v) >> 32)); }
// The horizontal distance measure the original compares with range squared.
i32 flat_distance_sq(const Unit& from, const Unit& to) {
    return wadd(square_hi(wsub(to.pos[0], from.pos[0])), square_hi(wsub(to.pos[2], from.pos[2])));
}

// Mission kinds looked up by name inside behaviours.
constexpr u8 kKindSelfDestruct = mission::kSelfDestruct, kKindLandIfCan = mission::kAirLandIfCan;
// Return addresses of the behaviour calls in the original's order routines.
constexpr u32 kPrimaryReturn = 0x0043B880, kSecondaryReturn = 0x0043BB25;
constexpr u32 kToAirWeapon = 1u << 17;

UnitTickState& tick_of(Missions& m, const Unit& u) { return m.orders.ticks[u.id]; }
u32 type_flags(Missions& m, const Unit& u) {
    const UnitTickTypeInfo* t = tick_of(m, u).tick_type;
    return t ? t->type_flags : 0;
}
const MissionTypeInfo& type_of(Missions& m, const Unit& u) {
    static const MissionTypeInfo none;
    const std::size_t i = tick_of(m, u).type_index;
    return i < m.types.size() ? m.types[i] : none;
}
u32 game_tick(Missions& m) { return m.orders.game_tick; }

// Sleep on the timer for `ticks` steps.
void set_deadline(Missions& m, MissionOrder& o, u32 ticks) {
    o.wait |= order_wait::kTimer;
    o.wake_tick = game_tick(m) + ticks;
}

// A newly issued order acknowledges itself once (order flag 0x2000).
void acknowledge(Missions& m, MissionOrder& o) {
    if (!(o.flags & 0x2000)) return;
    o.flags &= ~0x2000u;
    m.systems.speech(*o.unit, 5, SpeechText::None);
}

bool has_motion(Missions& m, const Unit& u) { return tick_of(m, u).has_motion; }

// The unit an order links (nullptr if none).
Unit* target_of(const MissionOrder& o) { return o.target.unit; }

// A float compared with 0.0f the way the original's FCOMP + "equal" test does: NaN counts as equal.
bool is_zero(float f) { return !(f < 0.0f || f > 0.0f); }

// A new order in front of the unit's list.
void push_order(Missions& m, Unit& unit, u8 kind, Unit* target, const std::int32_t* pos, i32 p0) {
    if (kind >= mission::kCount) {
        m.fault = "order kind outside the mission table";
        return;
    }
    MissionOrder* o = m.orders.systems.allocate_order();
    construct_order(m.orders, *o, kind, target, pos, p0, 0, 0);
    prepend_order(unit, *o);
}

// World position of the unit's nano piece (QueryNanoPiece, starting at 0).
std::array<i32, 3> nano_piece_position(Missions& m, Unit& u) {
    i32 piece = 0;
    if (!u.script) {
        m.fault = "nano piece of a unit without a script";
        return u.pos;
    }
    u.script->call_in_out(u.script->program().find_function("QueryNanoPiece"), &piece, nullptr, nullptr, nullptr);
    const auto off = piece_local_offset(u, piece);
    return {wadd(u.pos[0], off[0]), wadd(u.pos[1], off[1]), wadd(u.pos[2], off[2])};
}

// The beam drawn when a repair step went through: from the worker's nano piece to the target's box.
void repair_beam(Missions& m, Unit& worker, const Unit& target) {
    const auto from = nano_piece_position(m, worker);
    const UnitTypeInfo* t = target.type;
    const UnitTypeInfo none;
    const UnitTypeInfo& ti = t ? *t : none;
    const std::array<i32, 6> box = {wadd(target.pos[0], ti.model_min_x), target.pos[1], wadd(target.pos[2], ti.model_min_z),
                                    wadd(target.pos[0], ti.model_max_x), wadd(target.pos[1], ti.model_max_y),
                                    wadd(target.pos[2], ti.model_max_z)};
    m.systems.nano_beam(from, box);
}

// One repair step at the worker's rate (worker time / 30 per step).
bool repair_step(Missions& m, Unit& worker, Unit& target) {
    const float rate = static_cast<float>(static_cast<i32>(type_of(m, worker).worker_time) / 30);
    return repair_heal(m.combat, worker, target, rate);
}

// Hit points at or above the type's maximum (the original compares unsigned).
bool fully_healthy(const Unit& u) {
    const u32 max = u.type ? u.type->max_damage : 0;
    return static_cast<u32>(static_cast<i32>(u.hit_points)) >= max;
}

// --- The behaviours -------------------------------------------------------

std::int32_t activate(Missions& m, Unit& u, bool on) {
    if (type_of(m, u).abilities & mission_abilities::kOnOff) unit_set_activate(u, m.world, unit_flags::kActivated, on);
    return mission_result::kFinished;
}

std::int32_t cloak(Missions& m, Unit& u, bool on) {
    if (type_of(m, u).abilities & mission_abilities::kCloak) {
        if (on) u.flags |= 0x800u;
        else u.flags &= ~0x800u;
    }
    return mission_result::kFinished;
}

std::int32_t make_selectable(Unit& u) {
    u.flags = (u.flags & ~0x8000u) | 0x20u;
    return mission_result::kFinished;
}

std::int32_t standing_move_order(Unit& u, MissionOrder& o) {
    u.flags = (u.flags & 0xFFF3FFFFu) | ((static_cast<u32>(o.param0) & 3u) << 18);
    return mission_result::kFinished;
}

std::int32_t standing_fire_order(Unit& u, MissionOrder& o) {
    u.flags = (u.flags & 0xFFCFFFFFu) | ((static_cast<u32>(o.param0) & 3u) << 20);
    if (o.param0 == 0 || o.param0 == 1)
        for (int s = 0; s < 3; ++s)
            if (u.weapons[static_cast<std::size_t>(s)].flags & slot_flags::kReleased) clear_weapon_target(u, s);
    return mission_result::kFinished;
}

std::int32_t wait_for_attack(MissionOrder& o) {
    if (!target_of(o)) return mission_result::kFinished;
    if (o.step == 0) {
        o.wait = 0x18;
        return mission_result::kAdvance;
    }
    return o.step == 1 ? mission_result::kFinished : mission_result::kClearAll;
}

std::int32_t wait(Missions& m, Unit& u, MissionOrder& o) {
    if (o.param1 != 0) {
        // Wait until an enemy comes within param1 or the time (param0) runs out.
        std::vector<Unit*> seen;
        m.systems.enemies_in_radius(tick_of(m, u).owner, u.pos, o.param1, seen);
        if (!seen.empty() || o.param0 < 1) return mission_result::kFinished;
        const u32 n = m.orders.rng.next_mod(30) + 150;
        o.param0 = wsub(o.param0, static_cast<i32>(n));
        set_deadline(m, o, n);
        return mission_result::kContinue;
    }
    if (o.step == 0) {
        set_deadline(m, o, static_cast<u32>(o.param0));
        return mission_result::kAdvance;
    }
    return o.step == 1 ? mission_result::kFinished : mission_result::kClearAll;
}

std::int32_t stop(Missions& m, Unit& u, MissionOrder& o) {
    acknowledge(m, o);
    for (int s = 0; s < 3; ++s) clear_weapon_target(u, s);
    // A flying aircraft lands where it is.
    if ((u.flags & 3u) == 2 && (type_flags(m, u) & mission_type_flags::kCanFly))
        push_order(m, u, kKindLandIfCan, nullptr, u.pos.data(), 0);
    return mission_result::kFinished;
}

std::int32_t paralyze(Missions& m, Unit& u, MissionOrder& o) {
    if (o.param0 == 0) {
        unit_set_activate(u, m.world, unit_flags::kParalyzed, false);
        return mission_result::kFinished;
    }
    if (o.param0 > 0x708) o.param0 = 0x708;
    unrelease_weapon_slot(u, 3);
    for (int s = 0; s < 3; ++s) clear_weapon_target(u, s);
    if (has_motion(m, *o.unit) && o.attached) m.systems.drop_goal_object(*o.unit, o);
    set_deadline(m, o, static_cast<u32>(o.param0));
    o.param0 = 0;
    unit_set_activate(u, m.world, unit_flags::kParalyzed, true);
    return mission_result::kAdvance;
}

std::int32_t self_destruct(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    const u32 abilities = type_of(m, u).abilities;
    if ((static_cast<u32>(o.param1) & 0xF0000000u) == 0)
        o.param1 = static_cast<i32>(((abilities >> 20) & 7u) | 0xF0000000u);
    if (o.param0 == 0 && (abilities & mission_abilities::kCountdownMask)) {
        if (event & 2) {  // cancelled
            if (!(u.flags & unit_tick_flags::kDying)) m.systems.speech(u, 0x17, SpeechText::None);
            return mission_result::kFinished;
        }
        const u32 left = static_cast<u32>(o.param1) & 0x0FFFFFFFu;
        if (left == 0) o.param0 = 1;
        else o.param1 = static_cast<i32>((left - 1) | 0xF0000000u);
        // The count is spoken from a six-entry table on the stack; larger counts read
        // past it into the caller's return address and the behaviour's arguments.
        static constexpr u32 kCount[6] = {0x16, 0x15, 0x14, 0x13, 0x12, 0x11};
        u32 id = 0;
        if (left < 6) id = kCount[left];
        else if (left == 6) id = m.caller == MissionCaller::Primary ? kPrimaryReturn : kSecondaryReturn;
        else if (left == 7) id = m.env.addresses.unit(u.id);
        else if (left == 8) id = o.address;
        else if (left == 9) id = event;
        else m.fault = "self-destruct countdown beyond the stack words the model knows";
        m.systems.speech(u, id, SpeechText::None);
        set_deadline(m, o, left > 0 ? 30u : m.orders.rng.next_mod(15));
        o.wait |= order_wait::kNotifyDestroy;
        return mission_result::kAdvance;
    }
    apply_damage(m.combat, &u, u, 30000, damage_kind::kSelfDestruct, 0);
    return mission_result::kFinished;
}

// The unit is repaired by the order's target, a builder (self repair at a repair pad).
std::int32_t self_repair(Missions& m, Unit& u, MissionOrder& o) {
    Unit* worker = target_of(o);
    if (!worker) {
        m.systems.speech(u, 7, SpeechText::RepairAborted);
        return 8;
    }
    switch (o.step) {
    case 0:
        if (!(type_flags(m, *worker) & mission_type_flags::kBuilder)) return mission_result::kClearAll;
        if (is_zero(worker->build_fraction_left) && (u.activate_flags & unit_flags::kActivated)) {
            unrelease_weapon_slot(u, 3);
            return mission_result::kAdvance;
        }
        return 8;
    case 1:
        if (fully_healthy(u)) return mission_result::kAdvance;
        u.cloak_holdoff_tick = game_tick(m) + 150;
        if (repair_step(m, *worker, u)) repair_beam(m, *target_of(o), u);
        set_deadline(m, o, 1);
        o.wait |= 8;
        return mission_result::kContinue;
    case 2:
        m.systems.speech(u, 10, SpeechText::UnitRepaired);
        return mission_result::kFinished;
    default:
        return mission_result::kClearAll;
    }
}

// The unit (a builder) repairs the order's target without moving.
std::int32_t repair_no_move(Missions& m, Unit& u, MissionOrder& o) {
    Unit* target = target_of(o);
    if (!target) {
        m.systems.speech(u, 7, SpeechText::RepairsUnsuccessful);
        return mission_result::kFinished;
    }
    switch (o.step) {
    case 0:
        if (!(type_flags(m, u) & mission_type_flags::kBuilder)) return mission_result::kClearAll;
        if (is_zero(target->build_fraction_left) && (u.activate_flags & unit_flags::kActivated)) {
            unrelease_weapon_slot(u, 3);
            return mission_result::kAdvance;
        }
        return 8;
    case 1:
        if (fully_healthy(*target)) return mission_result::kAdvance;
        if (target->flags & 0xCu) return mission_result::kAdvance;
        u.cloak_holdoff_tick = game_tick(m) + 150;
        if (repair_step(m, u, *target)) repair_beam(m, *o.unit, *target_of(o));
        set_deadline(m, o, 1);
        o.wait |= 8;
        return mission_result::kContinue;
    case 2:
        m.systems.speech(u, 10, SpeechText::UnitRepaired);
        return mission_result::kFinished;
    default:
        return mission_result::kClearAll;
    }
}

// Builds stockpile rounds for weapon slot param0: param1 rounds to go, param2 the
// progress of the current round (in reload-time steps of 5).
std::int32_t build_weapon(Missions& m, Unit& u, MissionOrder& o) {
    if (o.param0 < 0 || o.param0 > 2) {
        m.fault = "stockpile order for a slot outside 0..2";
        return mission_result::kClearAll;
    }
    WeaponSlot& slot = u.weapons[static_cast<std::size_t>(o.param0)];
    if (o.step == 1 && !slot.type) {
        m.fault = "stockpile order for a slot without a weapon";
        return mission_result::kClearAll;
    }
    switch (o.step) {
    case 0:
        if (o.param1 < 1) return mission_result::kFinished;
        if (slot.stockpile >= 200) {
            set_deadline(m, o, 300);
            return mission_result::kContinue;
        }
        o.param2 = 0;
        return mission_result::kAdvance;
    case 1: {
        const WeaponType& w = *slot.type;
        const i32 reload = w.reload_time;
        const i32 was = o.param2;
        const i32 stepped = wadd(was, 5);
        const i32 now = stepped < reload ? stepped : reload;
        // Pay the part of one shot's cost between the old and the new progress.
        const double r = reload, p = was, n = now;
        const double metal = w.metal_per_shot, energy = w.energy_per_shot;
        const i32 m_cost = wsub(x87::ftol32(n * metal / r), x87::ftol32(p * metal / r));
        const i32 e_cost = wsub(x87::ftol32(n * energy / r), x87::ftol32(p * energy / r));
        if (!m.systems.use_resources_if_not_stalled(u, static_cast<float>(e_cost), static_cast<float>(m_cost))) {
            set_deadline(m, o, 10);
            return mission_result::kContinue;
        }
        o.param2 = now;
        if (now < reload) {
            set_deadline(m, o, 5);
            return mission_result::kContinue;
        }
        return mission_result::kAdvance;
    }
    case 2:
        ++slot.stockpile;
        o.param1 = wsub(o.param1, 1);
        m.systems.build_counts_changed(u);
        return mission_result::kRestart;
    default:
        return mission_result::kClearAll;
    }
}

// Idle: weapons free; every 30-59 steps look for a target to attack (fire at will).
std::int32_t standby(Missions& m, Unit& u, MissionOrder& o, bool mine) {
    if (o.step == 0) {
        if (mine ? !(u.flags & 0x20000000u) : !has_motion(m, u)) return mission_result::kClearAll;
        release_weapon_targets(u, 3);
        o.wait |= order_wait::kClearWeapons;
        set_deadline(m, o, 1);
        return mission_result::kAdvance;
    }
    if (o.step != 1) return mission_result::kClearAll;
    Unit* t = m.systems.fire_at_will_target(u);
    if (mine) {
        // A mine goes off when a ground unit is in reach and it may fire.
        if (t && (t->flags & 3u) == 1 && (u.flags & 0x300000u)) {
            push_order(m, u, kKindSelfDestruct, nullptr, nullptr, 1);
            return mission_result::kFinished;
        }
    } else if (t && m.systems.issue_attack_order(u, *t, false)) {
        return mission_result::kFinished;
    }
    o.wait |= order_wait::kClearWeapons;
    set_deadline(m, o, m.orders.rng.next_mod(30) + 30);
    return mission_result::kContinue;
}

std::int32_t guard_no_move(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (event & 0x10008u) {
        o.step = 3;
        return mission_result::kContinue;
    }
    switch (o.step) {
    case 0:
        release_weapon_targets(u, 3);
        set_deadline(m, o, 30);
        return mission_result::kAdvance;
    case 1: {
        // Keep guarding what weapon 0 already aims at, if it is alive.
        const WeaponSlot& s0 = u.weapons[0];
        Unit* aimed = nullptr;
        if (s0.target_z == kUnitTarget && s0.target_x != 0) {
            if (s0.target_x < 0 || static_cast<std::size_t>(s0.target_x) >= m.units.size()) {
                m.fault = "weapon target id outside the unit table";
                return mission_result::kContinue;
            }
            aimed = &m.units[static_cast<std::size_t>(s0.target_x)];
        }
        relink_unit(m.orders, o.target, aimed);
        Unit* t = target_of(o);
        if (t && (t->flags & unit_flags::kAlive)) {
            o.pos = t->pos;
            unrelease_weapon_slot(u, 0);
            aim_weapon_at_unit(u, *target_of(o), 0);
            o.param0 = 0;
            o.param1 = static_cast<i32>(m.orders.rng.next_mod(3) + 3);
            return mission_result::kAdvance;
        }
        set_deadline(m, o, 30);
        return mission_result::kContinue;
    }
    case 2: {
        if (event & 0x4000u) o.param0 = 0;
        else o.param0 = wadd(o.param0, 1);
        if (o.param0 <= o.param1) {
            Unit* t = target_of(o);
            if (!t) {
                m.fault = "guard range check without a target";
                return mission_result::kContinue;
            }
            if (weapon_reaches_unit(m, u, *t, 0)) {
                o.wait |= 0x7008u;
                return mission_result::kContinue;
            }
        }
        if (static_cast<i32>(m.orders.rng.next_mod(100)) < 80) {
            o.param0 = 0;
            return mission_result::kAdvance;
        }
        return mission_result::kRestart;
    }
    case 3: {
        // Pick a random visible enemy within 640 of the guarded spot.
        std::vector<Unit*> seen;
        m.systems.enemies_in_radius(tick_of(m, u).owner, o.pos, 0x280, seen);
        if (seen.empty()) return mission_result::kRestart;
        const u32 pick = m.orders.rng.next_mod(static_cast<u32>(seen.size()));
        relink_unit(m.orders, o.target, seen[pick]);
        if (!target_of(o)) {
            m.fault = "guard picked an empty table slot";
            return mission_result::kContinue;
        }
        aim_weapon_at_unit(u, *target_of(o), 0);
        o.step = 1;
        return mission_result::kContinue;
    }
    default:
        return mission_result::kClearAll;
    }
}

std::int32_t attack_no_move(Missions& m, Unit& u, MissionOrder& o, u32 event) {
    if (!target_of(o) || (event & 0x10808u)) return mission_result::kFinished;
    switch (o.step) {
    case 0:
        acknowledge(m, o);
        return mission_result::kAdvance;
    case 1:
        unrelease_weapon_slot(u, 0);
        aim_weapon_at_unit(u, *target_of(o), 0);
        o.wait = 0x11808;
        return mission_result::kAdvance;
    case 2:
        release_weapon_targets(u, 3);
        return mission_result::kFinishOrIdle;
    default:
        return mission_result::kClearAll;
    }
}

// Becomes the attack order the unit would use on the target, with weapon slot 2 (param0 = 2).
std::int32_t attack_special(Missions& m, Unit& u, MissionOrder& o) {
    const u8 kind = m.systems.resolve_attack_mission(u, target_of(o));
    if (kind >= mission::kCount) {
        m.fault = "order kind outside the mission table";
        return mission_result::kContinue;
    }
    o.kind = kind;
    const u32 d = mission::default_flags()[kind];
    o.flags = ((d ^ o.flags) & 0x600u) ^ d;
    o.param0 = 2;
    return mission_result::kContinue;
}

// Attack the nearest (with a random discount) non-allied unit of type param0.
std::int32_t attack_unit_type(Missions& m, Unit& u, MissionOrder& o) {
    if (o.step == 0) {
        if (!(type_of(m, u).abilities & mission_abilities::kAttack)) return mission_result::kClearAll;
        set_deadline(m, o, m.orders.rng.next_mod(90) + 1);
        return mission_result::kAdvance;
    }
    if (o.step != 1) return mission_result::kClearAll;
    Unit* best = nullptr;
    i32 best_score = 0x7FFFFFFF;
    const auto& allies = m.allied[static_cast<std::size_t>(tick_of(m, u).owner)];
    for (std::size_t i = 1; i < m.units.size(); ++i) {
        Unit& t = m.units[i];
        const UnitTickState& tt = m.orders.ticks[i];
        if (static_cast<u32>(tt.type_index) != static_cast<u32>(o.param0)) continue;
        const u8 state = m.orders.players[static_cast<std::size_t>(tt.owner)].state;
        if (state >= allies.size()) {
            m.fault = "alliance byte outside the modelled range";
            return mission_result::kContinue;
        }
        if (allies[state] != 0) continue;
        const i32 d = flat_distance_sq(u, t);
        const i32 score = wsub(d, static_cast<i32>(m.orders.rng.next_mod(static_cast<u32>(d / 2))));
        if (score <= best_score) {
            best_score = score;
            best = &t;
        }
    }
    if (!best) return mission_result::kFinished;
    push_order(m, u, m.systems.resolve_attack_mission(u, best), best, nullptr, 0);
    return mission_result::kRestart;
}

}  // namespace

bool mission_implemented(std::uint8_t kind) {
    switch (kind) {
    case mission::kReady: case mission::kActivate: case mission::kAttackNoMove: case mission::kAttackSpecial:
    case mission::kAttackUnitType: case mission::kBuildWeapon: case mission::kCloakOff: case mission::kCloakOn:
    case mission::kDeactivate: case mission::kGuardNoMove: case mission::kMakeSelectable: case mission::kParalyze:
    case mission::kRepairUnitNoMove: case mission::kSelfDestruct: case mission::kSelfDestructFg: case mission::kSelfRepair:
    case mission::kStandby: case mission::kStandbyMine: case mission::kStandingFireOrder: case mission::kStandingMoveOrder:
    case mission::kStop: case mission::kWait: case mission::kWaitForAttack:
        return true;
    default:
        return false;
    }
}

std::int32_t run_mission_behaviour(Missions& m, Unit& u, MissionOrder& o, std::uint32_t& event) {
    switch (o.kind) {
    case mission::kReady: return mission_result::kFinished;
    case mission::kActivate: return activate(m, u, true);
    case mission::kDeactivate: return activate(m, u, false);
    case mission::kCloakOn: return cloak(m, u, true);
    case mission::kCloakOff: return cloak(m, u, false);
    case mission::kMakeSelectable: return make_selectable(u);
    case mission::kStandingMoveOrder: return standing_move_order(u, o);
    case mission::kStandingFireOrder: return standing_fire_order(u, o);
    case mission::kWaitForAttack: return wait_for_attack(o);
    case mission::kWait: return wait(m, u, o);
    case mission::kStop: return stop(m, u, o);
    case mission::kParalyze: return paralyze(m, u, o);
    case mission::kSelfDestruct:
    case mission::kSelfDestructFg: return self_destruct(m, u, o, event);
    case mission::kSelfRepair: return self_repair(m, u, o);
    case mission::kRepairUnitNoMove: return repair_no_move(m, u, o);
    case mission::kBuildWeapon: return build_weapon(m, u, o);
    case mission::kStandby: return standby(m, u, o, false);
    case mission::kStandbyMine: return standby(m, u, o, true);
    case mission::kGuardNoMove: return guard_no_move(m, u, o, event);
    case mission::kAttackNoMove: return attack_no_move(m, u, o, event);
    case mission::kAttackSpecial: return attack_special(m, u, o);
    case mission::kAttackUnitType: return attack_unit_type(m, u, o);
    default: return m.systems.run_other(u, o, event);
    }
}

void unrelease_weapon_slot(Unit& unit, int slot) {
    if (slot == 3) {
        unrelease_weapon_slot(unit, 0);
        unrelease_weapon_slot(unit, 1);
        slot = 2;
    }
    WeaponSlot& s = unit.weapons[static_cast<std::size_t>(slot)];
    if (!(s.flags & slot_flags::kEnabled) || !(s.flags & slot_flags::kReleased)) return;
    s.flags = static_cast<u8>(s.flags & ~slot_flags::kReleased);
    clear_weapon_target(unit, slot);
}

void aim_weapon_at_unit(Unit& unit, const Unit& target, int slot) {
    WeaponSlot& s = unit.weapons[static_cast<std::size_t>(slot)];
    s.target_x = static_cast<std::int16_t>(target.id);
    s.target_z = kUnitTarget;
    unit.net_dirty = static_cast<u16>(unit.net_dirty & 0x83FFu);
}

bool weapon_reaches_unit(Missions& m, Unit& unit, Unit& target, int slot) {
    const WeaponType* w = unit.weapons[static_cast<std::size_t>(slot)].type;
    if (!w) {
        m.fault = "range check of a slot without a weapon";
        return false;
    }
    const i32 sea = m.env.sea_level;
    auto model_height = [](const Unit& x) { return x.type ? hi16(x.type->model_max_y) : 0; };
    if (w->flags & weapon_flags::kWaterWeapon) {
        // Underwater weapons: the target must be in the water (floaters may be on it);
        // a hovercraft only while its middle is below the surface.
        const u32 tf = type_flags(m, target);
        if (!(tf & mission_type_flags::kFloater) && hi16(target.pos[1]) > sea) return false;
        if ((tf & mission_type_flags::kHover) && hi16(target.pos[1]) + (model_height(target) >> 1) > sea) return false;
    } else {
        if (hi16(unit.pos[1]) + model_height(unit) <= sea) return false;
        if (hi16(target.pos[1]) + model_height(target) <= sea) return false;
        if ((w->flags & kToAirWeapon) && (target.flags & 3u) != 2) return false;
        if (w->flags & weapon_flags::kBallistic) {
            const std::int16_t pitch = ballistic_pitch(wsub(unit.pos[0], target.pos[0]), wsub(unit.pos[1], target.pos[1]),
                                                       wsub(unit.pos[2], target.pos[2]), w->velocity, w->min_barrel_angle,
                                                       m.env.gravity);
            if (pitch == -0x8000) return false;
        }
    }
    return flat_distance_sq(unit, target) <= static_cast<i32>(static_cast<u32>(w->range) * static_cast<u32>(w->range));
}

} // namespace sim
