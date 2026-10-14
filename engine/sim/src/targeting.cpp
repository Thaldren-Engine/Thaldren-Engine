#include "sim/targeting.h"

#include "sim/weapons.h"

namespace sim {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

i32 wadd(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
i32 wsub(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
i32 hi16(i32 v) { return static_cast<std::int16_t>(static_cast<u32>(v) >> 16); }
// High dword of the 64-bit square (whole world units squared, for 16.16 offsets).
i32 square_hi(i32 v) { return static_cast<i32>(static_cast<u32>(static_cast<std::uint64_t>(static_cast<i64>(v) * v) >> 32)); }
i32 flat_distance_sq(const std::array<i32, 3>& a, const std::array<i32, 3>& b) {
    return wadd(square_hi(wsub(a[0], b[0])), square_hi(wsub(a[2], b[2])));
}
// FCOMP against 0.0 and a test of "equal": NaN counts as equal.
bool is_zero(float f) { return !(f < 0.0f || f > 0.0f); }

constexpr u32 kDying = 0x4000u, kHasWeaponsFlag = 0x80000000u, kStatic = 0x20000000u;
constexpr u32 kFireOrderMask = 0x300000u, kFireAtWill = 0x200000u, kMoveOrderMask = 0xC0000u, kManeuver = 0x40000u;
constexpr u32 kToAir = 1u << 17;

bool alive(const Unit& u) { return (u.flags & unit_flags::kAlive) && !(u.flags & kDying); }

void set_fault(Targeting& t, const char* what) {
    if (t.fault.empty()) t.fault = what;
}

UnitTickState& tick_of(Targeting& t, const Unit& u) { return t.ticks[u.id]; }
u32 type_flags(Targeting& t, const Unit& u) {
    const UnitTickTypeInfo* tt = tick_of(t, u).tick_type;
    return tt ? tt->type_flags : 0;
}
u32 abilities(Targeting& t, const Unit& u) {
    const std::size_t k = tick_of(t, u).type_index;
    return k < t.mission_types.size() ? t.mission_types[k].abilities : 0;
}
const TargetingTypeInfo& ttype(Targeting& t, const Unit& u) {
    static const TargetingTypeInfo none;
    const std::size_t k = tick_of(t, u).type_index;
    return k < t.types.size() ? t.types[k] : none;
}
i32 model_height(const Unit& u) { return u.type ? hi16(u.type->model_max_y) : 0; }
bool has_motion(Targeting& t, const Unit& u) { return tick_of(t, u).has_motion; }
PlayerSlot& owner_of(Targeting& t, const Unit& u) { return t.players[static_cast<std::size_t>(tick_of(t, u).owner)]; }

// Whether `player`'s alliance byte for the owner of `other` is set.
bool allied_with(Targeting& t, int player, const Unit& other) {
    const u8 state = owner_of(t, other).state;
    if (player < 0 || static_cast<std::size_t>(player) >= t.allied.size() || state >= 16) {
        set_fault(t, "alliance byte outside the modelled range");
        return false;
    }
    return t.allied[static_cast<std::size_t>(player)][state] != 0;
}

// Whether `target`'s type is in the category mask.
bool in_category(Targeting& t, const CategoryMask& mask, const Unit& target) {
    const u16 bit = ttype(t, target).category_bit;
    if (static_cast<std::size_t>(bit >> 5) >= mask.size()) {
        set_fault(t, "category bit past the mask");
        return false;
    }
    return (mask[bit >> 5] >> (bit & 31)) & 1u;
}

bool reaches(Targeting& t, Unit& unit, Unit& target, int slot) {
    const bool r = weapon_reaches_unit(t.missions, unit, target, slot);
    if (!t.missions.fault.empty()) set_fault(t, "weapon range check failed (see missions fault)");
    return r;
}

const WeaponType* slot_weapon(Targeting& t, const Unit& u, int slot) {
    const WeaponType* w = u.weapons[static_cast<std::size_t>(slot)].type;
    if (!w) set_fault(t, "weapon slot without a weapon type");
    return w;
}

// The unit a weapon slot aims at (by id), or none for a ground point / no target.
Unit* slot_target_unit(Targeting& t, Unit& u, int slot) {
    const WeaponSlot& s = u.weapons[static_cast<std::size_t>(slot)];
    if (s.target_z != kUnitTarget || s.target_x == 0) return nullptr;
    if (s.target_x < 0 || static_cast<std::size_t>(s.target_x) >= t.units.size()) {
        set_fault(t, "weapon target id outside the unit table");
        return nullptr;
    }
    return &t.units[static_cast<std::size_t>(s.target_x)];
}

std::uint8_t by_layer(Targeting& t, const Unit& u, std::uint8_t air, std::uint8_t ground) {
    return (type_flags(t, u) & targeting_type_flags::kCanFly) ? air : ground;
}

// The unit can repair or help build `target` (0x4899B0).
bool can_help(Targeting& t, Unit& unit, Unit* target) {
    if (!target || !(abilities(t, unit) & targeting_abilities::kRepair)) return false;
    const i32 max = target->type ? static_cast<i32>(target->type->max_damage) : 0;
    if (static_cast<i32>(target->hit_points) == max || (target->flags & 3u) == 2) return false;
    const u32 tf = type_flags(t, unit);
    const bool flies = tf & targeting_type_flags::kCanFly;
    const i32 sea = t.env.sea_level;
    const i32 top = hi16(target->pos[1]) + model_height(*target);
    const bool air_ok = !flies || (tf & targeting_type_flags::kAmphibious) || sea <= top;
    const bool ground_ok = flies || sea - ttype(t, unit).max_water_depth <= top;
    return air_ok && ground_ok;
}

// The unit (a transport) can pick `target` up now (0x489A90).
bool can_pick_up(Targeting& t, Unit& unit, Unit& target) {
    if (abilities(t, target) & targeting_abilities::kNotTransportable) return false;
    if (!(abilities(t, unit) & targeting_abilities::kLoad)) return false;
    int carried = 0;
    for (const Unit* c = unit.cargo_head; c; c = c->cargo_next)
        if (c->carrier == &unit) ++carried;
    const TargetingTypeInfo& ut = ttype(t, unit);
    if (static_cast<int>(ut.transport_capacity) <= carried) return false;
    if (!has_motion(t, target)) return false;
    if (static_cast<std::int16_t>(ut.transport_size) < ttype(t, target).footprint_x) return false;
    if ((target.flags & 3u) == 2) return false;
    if (!(type_flags(t, unit) & targeting_type_flags::kCanFly) && ttype(t, target).min_water_depth >= 0) return false;
    const i32 top = wadd(target.pos[1], target.type ? target.type->model_max_y : 0);
    if (top <= static_cast<i32>(static_cast<u32>(t.env.sea_level) << 16)) return false;
    return is_zero(target.build_fraction_left);
}

// Flag byte of the feature on the map cell under `pos`, if a wreck-like feature is there
// (0x4815A0 and the feature lookup the resolution inlines); 0 for none.
std::uint8_t feature_flags_at(Targeting& t, const std::array<i32, 3>& pos) {
    const i32 cx = pos[0] >> 20, cz = pos[2] >> 20;
    if (cx < 0 || cx >= t.map.width || cz < 0 || cz >= t.map.height) return 0;
    const std::size_t at = static_cast<std::size_t>(cz) * static_cast<std::size_t>(t.map.width) + static_cast<std::size_t>(cx);
    if (at >= t.map.cells.size()) {
        set_fault(t, "map cell outside the grid");
        return 0;
    }
    const MapCell& cell = t.map.cells[at];
    if (cell.feature < 0xFFFB) {
        if (cell.feature >= t.feature_count) return 0;
        return t.systems.feature_flags(cell.feature);
    }
    if (cell.feature != 0xFFFE) return 0;
    // Part of a bigger feature: its own cell lies (low byte) rows and (high byte) columns back.
    const i64 back = static_cast<i64>(cell.feature_aux & 0xFF) * t.map.width + (cell.feature_aux >> 8);
    const i64 own = static_cast<i64>(at) - back;
    if (own < 0) {
        set_fault(t, "feature cell reference before the grid");
        return 0;
    }
    const u16 f = t.map.cells[static_cast<std::size_t>(own)].feature;
    if (f >= 0xFFFB) return 0;
    if (f >= t.feature_count) {  // the original does not check the count here
        set_fault(t, "feature index past the feature table");
        return 0;
    }
    return t.systems.feature_flags(f);
}

bool wreck_at(Targeting& t, Unit& unit, const std::array<i32, 3>& pos) {
    if (!t.systems.position_explored(tick_of(t, unit).owner, pos)) return false;
    return feature_flags_at(t, pos) & 0x80u;
}

// The attack kind (intent 3).
std::uint8_t resolve_attack(Targeting& t, Unit& unit, Unit* target, bool enemy) {
    const u32 tf = type_flags(t, unit);
    if (!(abilities(t, unit) & targeting_abilities::kAttack)) return 0;
    if (unit.flags & kHasWeaponsFlag) {
        const WeaponType* w0 = slot_weapon(t, unit, 0);
        if (!w0) return 0;
        if (!enemy) {
            // Ground or friendly targets.
            if (w0->flags & kToAir) return 0;
            if (!(tf & targeting_type_flags::kCanFly)) return mission::kSuppress;
            const WeaponType* first = ttype(t, unit).first_weapon;
            if (!first) {
                set_fault(t, "type without a first weapon");
                return 0;
            }
            return (first->flags & weapon_flags::kDropped) ? mission::kAirStrike : mission::kAirToGround;
        }
        if ((target->flags & 3u) != 2 && (w0->flags & kToAir)) return 0;
        const WeaponSlot& s1 = unit.weapons[1];
        auto slot1_water = [&]() -> bool {
            if (!(s1.flags & slot_flags::kEnabled)) return false;
            const WeaponType* w1 = slot_weapon(t, unit, 1);
            return w1 && (w1->flags & weapon_flags::kWaterWeapon);
        };
        const i32 top = model_height(*target) + hi16(target->pos[1]);
        if (top < static_cast<i32>(t.env.sea_level)) {
            if (!(w0->flags & weapon_flags::kWaterWeapon) && !slot1_water()) return 0;
        } else if (tf & targeting_type_flags::kHover) {
            if (w0->flags & weapon_flags::kWaterWeapon) return 0;
            if (slot1_water()) return 0;
        }
        if (tf & targeting_type_flags::kCanFly) {
            const WeaponType* first = ttype(t, unit).first_weapon;
            if (!first) {
                set_fault(t, "type without a first weapon");
                return 0;
            }
            const bool dropped = first->flags & weapon_flags::kDropped;
            const bool target_flies = type_flags(t, *target) & targeting_type_flags::kCanFly;
            if (dropped && !target_flies) return mission::kAirStrike;
            if (!dropped && target_flies) return mission::kAirToAir;
            if (!target_flies) return (tf & targeting_type_flags::kHoverAttack) ? mission::kAirToGroundHover : mission::kAirToGround;
            return 0;
        }
        if (has_motion(t, unit)) return mission::kAttackChase;
        if (unit.flags & kStatic) return mission::kAttackNoMove;
    }
    return (tf & targeting_type_flags::kKamikaze) ? mission::kAttackKamikaze : 0;
}

std::uint8_t move_kind(Targeting& t, const Unit& u) { return by_layer(t, u, mission::kAirMove, mission::kMoveGround); }

// The default command (intent 1).
std::uint8_t resolve_default(Targeting& t, Unit& unit, Unit* target, const std::array<i32, 3>* pos, bool enemy, bool ally) {
    const u32 ab = abilities(t, unit);
    const u32 tf = type_flags(t, unit);
    const bool flies = tf & targeting_type_flags::kCanFly;
    if ((ab & targeting_abilities::kAttack) && enemy) return resolve_mission(t, intent::kAttack, unit, target, pos);
    if (t.env.command_mode == 1) {
        if ((ab & targeting_abilities::kReclaim) && enemy) return by_layer(t, unit, mission::kAirReclaimUnit, mission::kReclaimUnit);
        if (ally) {
            if (can_help(t, unit, target) && !is_zero(target->build_fraction_left))
                return by_layer(t, unit, mission::kAirHelpBuild, mission::kHelpBuild);
            if (can_help(t, unit, target)) return by_layer(t, unit, mission::kAirRepairUnit, mission::kRepairUnit);
        }
        if (flies && ally && (type_flags(t, *target) & targeting_type_flags::kAirBase)) return mission::kAirLanding;
        if (target && can_pick_up(t, unit, *target)) return by_layer(t, unit, mission::kAirPickup, mission::kGroundPickup);
        if ((ab & targeting_abilities::kGuard) && ally) return by_layer(t, unit, mission::kAirFollow, mission::kFollowGround);
    } else {
        if ((ab & targeting_abilities::kReclaim) && enemy) return resolve_mission(t, intent::kReclaim, unit, target, pos);
        if (target) {
            if (can_help(t, unit, target) && !is_zero(target->build_fraction_left))
                return resolve_mission(t, intent::kRepair, unit, target, pos);
            // Clicking one of the local player's own selectable, finished units means nothing.
            const UnitTickState& tt = tick_of(t, *target);
            if (static_cast<u8>(tt.owner) == t.env.local_player && (target->flags & 0x20u) && is_zero(target->build_fraction_left) &&
                tt.transfer_holdoff == 0 && (!target->carrier || (target->carrier->flags & 0x40000000u)))
                return 0;
        }
    }
    if ((ab & targeting_abilities::kResurrect) && pos && wreck_at(t, unit, *pos)) return mission::kResurrect;
    if ((ab & targeting_abilities::kReclaim) && pos && wreck_at(t, unit, *pos))
        return by_layer(t, unit, mission::kAirReclaim, mission::kReclaim);
    if (!(ab & targeting_abilities::kMove) || !has_motion(t, unit)) return 0;
    return move_kind(t, unit);
}

}  // namespace

void create_player_targeting(Targeting& t) {
    for (int slot = 0; slot < 10; ++slot) {
        const PlayerSlot& p = t.players[static_cast<std::size_t>(slot)];
        if (p.active && p.controller == 3) continue;
        PlayerController& c = t.state.controllers[static_cast<std::size_t>(slot)];
        c = PlayerController{};
        c.exists = true;
        // The unit table goes under the player's state index (which names its player record).
        if (p.state >= t.state.tables.size()) {
            set_fault(t, "unit table index past the table array");
            continue;
        }
        UnitTable& tb = t.state.tables[p.state];
        tb = UnitTable{};
        tb.exists = true;
        tb.player = p.state;
    }
}

void enemies_in_radius(Targeting& t, int table, const std::array<i32, 3>& pos, i32 radius, std::vector<Unit*>& out) {
    if (table < 0 || static_cast<std::size_t>(table) >= t.state.tables.size() || !t.state.tables[static_cast<std::size_t>(table)].exists) {
        set_fault(t, "enemy list of a player without a unit table");
        return;
    }
    const UnitTable& tb = t.state.tables[static_cast<std::size_t>(table)];
    const i32 r2 = static_cast<i32>(static_cast<u32>(radius) * static_cast<u32>(radius));
    for (Unit* u : tb.seen)
        if (flat_distance_sq(pos, u->pos) <= r2 && alive(*u)) out.push_back(u);
    // Radar dots count only with a targeting facility, and only when nothing is in sight.
    if (!tb.targeting_upgrade || !out.empty()) return;
    for (Unit* u : tb.radar)
        if (flat_distance_sq(pos, u->pos) <= r2 && alive(*u)) out.push_back(u);
}

Unit* find_best_target(Targeting& t, Unit& unit, int slot, bool in_weapon_range) {
    const PlayerSlot& owner = owner_of(t, unit);
    const bool ai = owner.active && owner.controller == 2;
    std::vector<Unit*> list;
    i32 radius = ttype(t, unit).sight_distance;
    if (in_weapon_range) {
        const WeaponType* w = slot_weapon(t, unit, slot);
        if (!w) return nullptr;
        radius = w->range;
    }
    enemies_in_radius(t, tick_of(t, unit).owner, unit.pos, radius, list);
    BattleRng& rng = t.missions.orders.rng;
    Unit* good = nullptr;
    Unit* bad = nullptr;
    i32 good_score = 0x7FFFFFFF, bad_score = 0x7FFFFFFF;
    const TargetingTypeInfo& ut = ttype(t, unit);
    const u32 uf = type_flags(t, unit);
    for (int picks = 0; picks < 50 && !list.empty(); ++picks) {
        // A random candidate, taken out of the list (the last one fills its place).
        const u32 k = rng.next_mod(static_cast<u32>(list.size()));
        Unit* target = list[k];
        list[k] = list.back();
        list.pop_back();
        if (!alive(*target)) continue;
        if (!(type_flags(t, *target) & targeting_type_flags::kShootMe) && !ai && !t.env.target_everything) continue;
        if (!(uf & targeting_type_flags::kKamikaze) && !reaches(t, unit, *target, slot)) continue;
        if (!in_weapon_range && in_category(t, ut.no_chase, *target)) continue;
        const WeaponType* w = slot_weapon(t, unit, slot);
        if (!w) return nullptr;
        if ((w->flags & weapon_flags::kParalyzer) && (target->activate_flags & unit_flags::kParalyzed)) continue;
        // Distance squared, less a random discount of up to all of it.
        const i32 score = static_cast<i32>(rng.next_mod(static_cast<u32>(flat_distance_sq(unit.pos, target->pos))));
        if (in_category(t, ut.bad_target[static_cast<std::size_t>(slot)], *target)) {
            if (score < bad_score) {
                bad_score = score;
                bad = target;
            }
        } else if (score < good_score) {
            good_score = score;
            good = target;
        }
    }
    return good ? good : bad;
}

Unit* fire_at_will_target(Targeting& t, Unit& unit) {
    if ((unit.flags & kFireOrderMask) != kFireAtWill) return nullptr;
    return find_best_target(t, unit, 0, false);
}

bool issue_attack_order(Targeting& t, Unit& unit, Unit& target, bool forced) {
    if (&unit == &target) return false;
    if (!(unit.flags & kMoveOrderMask) && !forced) return false;   // hold position
    if (!(unit.flags & kFireOrderMask) && !forced) return false;   // hold fire
    const std::uint8_t kind = resolve_mission(t, intent::kAttack, unit, &target, nullptr);
    if (!kind) return false;
    OrderContext& c = t.missions.orders;
    if ((unit.flags & kMoveOrderMask) == kManeuver && !forced) {
        // On "maneuver" the unit comes back afterwards: a move to where it stands goes behind the attack.
        const std::uint8_t back = resolve_mission(t, intent::kMove, unit, nullptr, &unit.pos);
        MissionOrder* move = c.systems.allocate_order();
        construct_order(c, *move, back, nullptr, unit.pos.data(), 0, 0, 0);
        prepend_order(unit, *move);
        MissionOrder* attack = c.systems.allocate_order();
        construct_order(c, *attack, kind, &target, nullptr, 0, 0, static_cast<u16>(ttype(t, unit).maneuver_leash));
        // Where the chase started (whole world units of x and z).
        attack->scratch0 = (static_cast<u32>(unit.pos[0]) >> 16) | (static_cast<u32>(unit.pos[2]) & 0xFFFF0000u);
        prepend_order(unit, *attack);
        return true;
    }
    MissionOrder* attack = c.systems.allocate_order();
    construct_order(c, *attack, kind, &target, nullptr, 0, 0, 0);
    prepend_order(unit, *attack);
    return true;
}

std::uint8_t resolve_mission(Targeting& t, std::uint8_t what, Unit& unit, Unit* target, const std::array<i32, 3>* pos) {
    bool enemy = false, ally = false;
    if (target) {
        if (!(target->flags & unit_flags::kAlive)) return 0;
        if (allied_with(t, tick_of(t, unit).owner, *target)) ally = true;
        else enemy = true;
    }
    const u32 ab = abilities(t, unit);
    switch (what) {
    case intent::kDefault: return resolve_default(t, unit, target, pos, enemy, ally);
    case intent::kMove:
        if (!(ab & targeting_abilities::kMove)) return 0;
        if (!has_motion(t, unit)) return mission::kQueuedMove;
        if (target) {
            if ((ab & targeting_abilities::kCapture) && enemy) return mission::kCapture;
            if ((ab & targeting_abilities::kReclaim) && enemy) return by_layer(t, unit, mission::kAirReclaimUnit, mission::kReclaimUnit);
            if (ally) {
                if (can_help(t, unit, target) && !is_zero(target->build_fraction_left))
                    return by_layer(t, unit, mission::kAirHelpBuild, mission::kHelpBuild);
                const u32 max = target->type ? target->type->max_damage : 0;
                if (can_help(t, unit, target) && static_cast<u32>(static_cast<i32>(target->hit_points)) < max)
                    return by_layer(t, unit, mission::kAirRepairUnit, mission::kRepairUnit);
            }
            if ((type_flags(t, unit) & targeting_type_flags::kCanFly) && ally &&
                (type_flags(t, *target) & targeting_type_flags::kAirBase))
                return mission::kAirLanding;
            if (can_pick_up(t, unit, *target)) return by_layer(t, unit, mission::kAirPickup, mission::kGroundPickup);
            if ((ab & targeting_abilities::kGuard) && ally) return by_layer(t, unit, mission::kAirFollow, mission::kFollowGround);
        }
        return move_kind(t, unit);
    case intent::kAttack: return resolve_attack(t, unit, target, enemy);
    case intent::kSpecialAttack: return (ab & targeting_abilities::kSpecialAttack) ? mission::kAttackSpecial : 0;
    case intent::kUnload:
        if ((ab & targeting_abilities::kLoad) && (type_flags(t, unit) & targeting_type_flags::kCanFly) && target &&
            (type_flags(t, *target) & targeting_type_flags::kAirBase))
            return mission::kAirLanding;
        if (ab & targeting_abilities::kLoad) return by_layer(t, unit, mission::kAirUnload, mission::kGroundUnload);
        return 0;
    case intent::kPickup:
        if (target && can_pick_up(t, unit, *target)) return by_layer(t, unit, mission::kAirPickup, mission::kGroundPickup);
        return 0;
    case intent::kGuard:
        if ((ab & targeting_abilities::kGuard) && ally) return by_layer(t, unit, mission::kAirFollow, mission::kFollowGround);
        return 0;
    case intent::kRepair:
        if (!can_help(t, unit, target)) return 0;
        if (is_zero(target->build_fraction_left)) return by_layer(t, unit, mission::kAirRepairUnit, mission::kRepairUnit);
        return by_layer(t, unit, mission::kAirHelpBuild, mission::kHelpBuild);
    case intent::kPatrol:
        if (!(ab & targeting_abilities::kPatrol)) return 0;
        if (!has_motion(t, unit)) return mission::kQueuedPatrol;
        if (!(ab & targeting_abilities::kRepair)) return by_layer(t, unit, mission::kAirPatrol, mission::kPatrol);
        return by_layer(t, unit, mission::kAirRepairPatrol, mission::kRepairPatrol);
    case intent::kStop: return mission::kStop;
    case intent::kTeleport: return mission::kTeleport;
    case intent::kReclaim: {
        if (!(ab & targeting_abilities::kReclaim)) return 0;
        if (!pos) {  // the original reads the position before checking it
            set_fault(t, "reclaim resolution without a position");
            return 0;
        }
        if ((ab & targeting_abilities::kResurrect) && wreck_at(t, unit, *pos)) return mission::kResurrect;
        if (wreck_at(t, unit, *pos)) return by_layer(t, unit, mission::kAirReclaim, mission::kReclaim);
        if (!target) return 0;
        return by_layer(t, unit, mission::kAirReclaimUnit, mission::kReclaimUnit);
    }
    case intent::kCapture:
        if ((ab & targeting_abilities::kCapture) && target && tick_of(t, *target).owner != tick_of(t, unit).owner)
            return mission::kCapture;
        return 0;
    case intent::kBuild:
        if (ttype(t, unit).has_build_options && has_motion(t, unit)) return by_layer(t, unit, mission::kAirMobileBuild, mission::kMobileBuild);
        return 0;
    default: return 0;
    }
}

void retarget_attack(Targeting& t, Unit* attacker, Unit& victim) {
    // Every order targeting the victim hears event 0x10 (orders of other units waiting on it wake up).
    for (UnitLink* l = victim.referrers; l; l = l->next)
        if (l->owner) l->owner->events |= 0x10u;
    if (attacker && tick_of(t, *attacker).type_index == 0) attacker = nullptr;
    UnitTickState& vt = tick_of(t, victim);
    const PlayerSlot& owner = owner_of(t, victim);
    // An AI unit that can capture stops what it does (the AI holds it back a while).
    if ((abilities(t, victim) & targeting_abilities::kCapture) && owner.active && owner.controller == 2) {
        const u32 r = t.missions.orders.rng.next_mod(300);
        PlayerController& c = t.state.controllers[static_cast<std::size_t>(vt.owner)];
        if (!c.exists) set_fault(t, "AI player without a controller");
        c.ai_hold_tick = r + t.env.game_tick + 30;
        clear_orders(t.missions.orders, victim, false);
    }
    if (attacker && owner.active && (owner.controller == 1 || owner.controller == 2) &&
        (type_flags(t, victim) & (targeting_type_flags::kHasWeapons | targeting_type_flags::kKamikaze)) &&
        is_zero(victim.build_fraction_left) && !allied_with(t, vt.owner, *attacker)) {
        bool answered = false;
        const TargetingTypeInfo& vtype = ttype(t, victim);
        const MissionOrder* head = victim.orders;
        if ((!head || (head->flags & 0x20000u)) && !in_category(t, vtype.no_chase, *attacker) &&
            !in_category(t, vtype.bad_target[0], *attacker) && reaches(t, victim, *attacker, 0))
            answered = issue_attack_order(t, victim, *attacker, false);
        if (!answered && (victim.flags & kFireOrderMask)) {
            // Released weapons turn on the attacker unless they have a good target in reach.
            for (int slot = 0; slot < 3; ++slot) {
                const WeaponSlot& s = victim.weapons[static_cast<std::size_t>(slot)];
                if (!(s.flags & slot_flags::kEnabled) || !(s.flags & slot_flags::kReleased)) continue;
                if (!reaches(t, victim, *attacker, slot)) continue;
                const WeaponType* w = slot_weapon(t, victim, slot);
                if (!w || (w->flags & weapon_flags::kCommandFire)) continue;
                Unit* current = slot_target_unit(t, victim, slot);
                if (current && reaches(t, victim, *current, slot) &&
                    !in_category(t, vtype.bad_target[static_cast<std::size_t>(slot)], *current))
                    continue;
                aim_weapon_at_unit(victim, *attacker, slot);
            }
        }
    }
    const u32 head_flags = victim.orders ? victim.orders->flags : 0;
    if (!(head_flags & 0x80u) && (vt.last_attacker_slot != static_cast<u8>(vt.owner) || vt.last_damage_kind == 1))
        t.systems.under_attack(victim);
}

void weapon_target_pass(Targeting& t, int player, bool ai) {
    PlayerController& c = t.state.controllers[static_cast<std::size_t>(player)];
    const PlayerSlot& p = t.players[static_cast<std::size_t>(player)];
    const int visits = t.env.unit_limit / 30;
    for (int n = 0; n <= visits; ++n) {
        // Round robin over the player's range of the unit table.
        if (c.cursor == 0 || c.cursor == p.last_unit) c.cursor = static_cast<u16>(p.first_unit);
        else ++c.cursor;
        if (c.cursor >= t.units.size()) {
            set_fault(t, "weapon-target cursor outside the unit table");
            return;
        }
        Unit& u = t.units[c.cursor];
        if (tick_of(t, u).type_index == 0 || !is_zero(u.build_fraction_left) || !(u.flags & kHasWeaponsFlag) ||
            (u.flags & kFireOrderMask) != kFireAtWill)
            continue;
        const TargetingTypeInfo& ut = ttype(t, u);
        for (int slot = 0; slot < 3; ++slot) {
            const WeaponSlot& s = u.weapons[static_cast<std::size_t>(slot)];
            if (!(s.flags & slot_flags::kEnabled) || !(s.flags & slot_flags::kReleased)) continue;
            const WeaponType* w = slot_weapon(t, u, slot);
            if (!w) return;
            if (w->flags & weapon_flags::kDropped) continue;
            if (!ai && (w->flags & weapon_flags::kCommandFire)) continue;
            // Keep a current target that is still an enemy, a wanted type and not already paralysed by this weapon.
            if (Unit* cur = slot_target_unit(t, u, slot)) {
                const bool keep = !allied_with(t, player, *cur) &&
                                  !in_category(t, ut.bad_target[static_cast<std::size_t>(slot)], *cur) &&
                                  !((w->flags & weapon_flags::kParalyzer) && (cur->activate_flags & unit_flags::kParalyzed));
                if (keep) continue;
            }
            if (!(w->flags & weapon_flags::kInterceptor)) {
                if ((u.flags & kFireOrderMask) != kFireAtWill) continue;
                Unit* target = find_best_target(t, u, slot, true);
                if (target) aim_weapon_at_unit(u, *target, slot);
                else clear_weapon_target(u, slot);
            } else {
                std::array<i32, 3> at{};
                if (t.systems.intercept_target(u, slot, at) < 0) {
                    clear_weapon_target(u, slot);
                } else {
                    // Aim at the shot's current spot as a ground point.
                    WeaponSlot& ws = u.weapons[static_cast<std::size_t>(slot)];
                    ws.target_x = static_cast<std::int16_t>(static_cast<u32>(at[0]) >> 16);
                    ws.target_z = static_cast<std::int16_t>(static_cast<u32>(at[2]) >> 16);
                    if (ws.target_z == kUnitTarget) ws.target_z = static_cast<std::int16_t>(-0x7FFF);
                    u.net_dirty = static_cast<u16>(u.net_dirty & 0x83FFu);
                }
            }
        }
    }
}

void refresh_unit_table(Targeting& t, int table) {
    if (table < 0 || static_cast<std::size_t>(table) >= t.state.tables.size()) return;
    UnitTable& tb = t.state.tables[static_cast<std::size_t>(table)];
    if (!tb.exists || tb.refresh_tick + 30u > t.env.game_tick) return;
    tb.seen.clear();
    tb.radar.clear();
    tb.targeting_upgrade = false;
    t.systems.ai_table_begin(table);
    const PlayerSlot& me = t.players[static_cast<std::size_t>(tb.player)];
    for (std::size_t id = 1; id < t.units.size(); ++id) {
        Unit& u = t.units[id];
        if (!alive(u)) continue;
        if (!allied_with(t, tb.player, u)) {
            if (t.systems.unit_visible(tb.player, u) && !(u.flags & 0x8000u)) tb.seen.push_back(&u);
            if (u.flags & 0x100u) tb.radar.push_back(&u);
        } else if (owner_of(t, u).state == me.state && is_zero(u.build_fraction_left)) {
            t.systems.ai_own_unit(table, u);
            if ((type_flags(t, u) & targeting_type_flags::kTargetingUpgrade) && (u.activate_flags & unit_flags::kActivated))
                tb.targeting_upgrade = true;
        }
    }
    t.systems.ai_table_end(table);
    tb.refresh_tick = t.env.game_tick;
    if (t.missions.orders.rng.next_mod(30) == 0) t.systems.ai_rescore(table);
}

} // namespace sim
