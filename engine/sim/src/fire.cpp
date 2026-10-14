#include "sim/fire.h"

#include "sim/x87.h"
#include "sim/x87_math.h"

namespace sim {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i16 = std::int16_t;
using i32 = std::int32_t;

namespace wf = weapon_flags;

i32 sub(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
i16 hi16(i32 v) { return static_cast<i16>(static_cast<u32>(v) >> 16); }

int script_index(const WeaponSlot& s) { return (s.flags >> 2) & 3; }

FireSlot fire_slot(const WeaponSlot& s, int index) {
    return FireSlot{s.type, index, s.aim_heading, s.aim_pitch, static_cast<u32>(s.aux)};
}

// Heading (absolute) and pitch from the muzzle straight at the target, as the
// line-of-sight and vertical-launch routines compute them.
void aim_from_muzzle(const WorldPos& muzzle, const WorldPos& target, u16& heading, u16& pitch) {
    const i32 dx = sub(muzzle[0], target[0]), dz = sub(muzzle[2], target[2]);
    heading = static_cast<u16>(x87::atan2_scaled(dx, dz));
    const i32 len = x87::ftol32(x87::crt_hypot(dx, dz));
    pitch = static_cast<u16>(x87::atan2_scaled(-static_cast<i32>(hi16(sub(muzzle[1], target[1]))), hi16(len)));
}

// Network packet 0x0D announcing a shot (36 bytes): start, aim point, weapon,
// interceptor bit, slot aim, target and shooter ids, slot. The original leaves
// bits 1-7 of byte 26 uninitialised; they are sent as 0 here.
void announce_shot(ProjectileWorld& w, const Unit& unit, const WeaponSlot& s, const WorldPos& start, const WorldPos& aim,
                   const Unit* target) {
    if (!w.env.net_game) return;
    u8 pkt[36] = {};
    auto put32 = [&](int at, i32 v) {
        for (int b = 0; b < 4; ++b) pkt[at + b] = static_cast<u8>(static_cast<u32>(v) >> (8 * b));
    };
    auto put16 = [&](int at, u16 v) {
        pkt[at] = static_cast<u8>(v);
        pkt[at + 1] = static_cast<u8>(v >> 8);
    };
    pkt[0] = 0x0D;
    for (int k = 0; k < 3; ++k) {
        put32(1 + 4 * k, start[static_cast<std::size_t>(k)]);
        put32(13 + 4 * k, aim[static_cast<std::size_t>(k)]);
    }
    pkt[25] = s.type->index;
    pkt[26] = static_cast<u8>((s.type->flags >> 30) & 1);
    put16(27, s.aim_heading);
    put16(29, s.aim_pitch);
    put16(31, target ? target->id : 0);
    put16(33, unit.id);
    pkt[35] = static_cast<u8>(script_index(s));
    const PlayerSlot& owner = w.combat.players[static_cast<std::size_t>(w.combat.ticks[unit.id].owner)];
    w.systems.network_send(owner.net_id, pkt, 36);
}

i32 fire_turret(ProjectileWorld& w, Unit& unit, int index, Unit* target, const WorldPos& target_pos) {
    WeaponSlot& s = unit.weapons[static_cast<std::size_t>(index)];
    if (!(s.flags & slot_flags::kAiming) || s.aim.result == 0) return 0;
    const WeaponType& wt = *s.type;
    const int si = script_index(s);
    // Re-solve the aim from the aim-from piece.
    u16 heading = 0, pitch = 0;
    bool ok = false;
    if (wt.flags & wf::kBallistic) {
        const WorldPos from = aim_from_position(unit, si);
        heading = static_cast<u16>(x87::atan2_scaled(sub(from[0], target_pos[0]), sub(from[2], target_pos[2])) - unit.heading);
        const i16 p = ballistic_pitch(sub(from[0], target_pos[0]), sub(from[1], target_pos[1]), sub(from[2], target_pos[2]),
                                      wt.velocity, wt.min_barrel_angle, w.env.gravity);
        pitch = static_cast<u16>(p);
        ok = p != -0x8000;
    } else if (wt.flags & wf::kLineOfSight) {
        direct_aim(unit, si, target_pos, heading, pitch);
        ok = true;
    }
    if (!ok) {
        s.flags = static_cast<u8>(s.flags & ~slot_flags::kAiming);
        unit.net_dirty = static_cast<u16>(unit.net_dirty | 0x1000);
        return 0;
    }
    // The script's aim must still be within the tolerances of the fresh solution.
    if (!aim_within_tolerance(unit, s, heading, pitch)) {
        s.flags = static_cast<u8>(s.flags & ~slot_flags::kAiming);
        return 0;
    }
    const WorldPos muzzle = weapon_piece_position(unit, si, -1);
    // Fire along the script's aim (made absolute) plus the accuracy spread: worse
    // when damaged, divided by kill tiers of 12 for veterans.
    s.aim_heading = static_cast<u16>(s.aim_heading + unit.heading);
    const u32 max = unit.type ? unit.type->max_damage : 0;
    const u16 damaged = static_cast<u16>(max ? static_cast<u32>(static_cast<i32>(unit.hit_points) << 11) / max : 0);  // the original faults on 0
    u16 spread = static_cast<u16>(wt.accuracy - damaged + 0x800);
    const i32 tiers = w.combat.ticks[unit.id].kill_count / 12;
    if (tiers > 1) spread = static_cast<u16>(spread / tiers);
    if (spread != 0) {
        const u16 half = static_cast<u16>(spread >> 1);
        s.aim_heading = static_cast<u16>(s.aim_heading + static_cast<u16>(w.rng.next_mod(spread)) - half);
        s.aim_pitch = static_cast<u16>(s.aim_pitch + static_cast<u16>(w.rng.next_mod(spread)) - half);
    }
    bool fired = false;
    if ((wt.flags & wf::kLineOfSight) || (wt.flags & wf::kSelfPropelled))
        fired = fire_directed(w, fire_slot(s, index), unit, muzzle, target_pos, target);
    else if (wt.flags & wf::kBallistic)
        fired = fire_ballistic(w, fire_slot(s, index), unit, muzzle, target_pos, target);
    if (!fired) return 0;
    s.aim.result = 0;
    s.flags = static_cast<u8>(s.flags & ~slot_flags::kAiming);
    announce_shot(w, unit, s, muzzle, target_pos, target);
    return 1;
}

i32 fire_vertical_launch(ProjectileWorld& w, Unit& unit, int index, Unit* target, const WorldPos& target_pos) {
    WeaponSlot& s = unit.weapons[static_cast<std::size_t>(index)];
    if (s.aim.result == 0) return 0;
    const int si = script_index(s);
    const WorldPos muzzle = weapon_piece_position(unit, si, -1);
    aim_from_muzzle(muzzle, target_pos, s.aim_heading, s.aim_pitch);
    i32 intercept = -1;
    if (s.type->flags & wf::kInterceptor) {
        intercept = find_intercept_target(w, unit, index);
        if (intercept < 0) return 0;
    }
    // The launch clears the aim result as soon as it has a table slot.
    if (!fire_vertical(w, fire_slot(s, index), unit, muzzle, target_pos, target, intercept)) return 0;
    s.aim.result = 0;
    announce_shot(w, unit, s, muzzle, target_pos, target);
    s.aim.result = 0;
    s.flags = static_cast<u8>(s.flags & ~slot_flags::kAiming);
    return 1;
}

i32 fire_line_of_sight(ProjectileWorld& w, Unit& unit, int index, Unit* target, const WorldPos& target_pos) {
    WeaponSlot& s = unit.weapons[static_cast<std::size_t>(index)];
    const WorldPos muzzle = weapon_piece_position(unit, script_index(s), -1);
    aim_from_muzzle(muzzle, target_pos, s.aim_heading, s.aim_pitch);
    // The unit itself must face the target: heading against the unit's heading,
    // pitch against the unit's bank word (as the original reads it).
    if (!aim_within_tolerance(unit, s, unit.heading, static_cast<u16>(unit.bank))) return 0;
    if (!fire_directed(w, fire_slot(s, index), unit, muzzle, target_pos, target)) return 0;
    announce_shot(w, unit, s, unit.pos, target_pos, target);
    return 1;
}

i32 fire_dropped(ProjectileWorld& w, Unit& unit, int index, Unit* target, const WorldPos& target_pos) {
    WeaponSlot& s = unit.weapons[static_cast<std::size_t>(index)];
    const WorldPos muzzle = weapon_piece_position(unit, script_index(s), -1);
    if (!spawn_from_motion(w, fire_slot(s, index), unit, muzzle)) return 0;
    announce_shot(w, unit, s, muzzle, target_pos, target);
    return 1;
}

} // namespace

i32 fire_weapon(ProjectileWorld& w, Unit& unit, int slot, Unit* target, const WorldPos& target_pos) {
    const WeaponSlot& s = unit.weapons[static_cast<std::size_t>(slot)];
    if (!s.type) return 0;
    switch (s.type->fire) {
    case WeaponFireRoutine::Turret: return fire_turret(w, unit, slot, target, target_pos);
    case WeaponFireRoutine::VerticalLaunch: return fire_vertical_launch(w, unit, slot, target, target_pos);
    case WeaponFireRoutine::LineOfSight: return fire_line_of_sight(w, unit, slot, target, target_pos);
    case WeaponFireRoutine::Dropped: return fire_dropped(w, unit, slot, target, target_pos);
    case WeaponFireRoutine::None: break;
    }
    return 0;
}

} // namespace sim
