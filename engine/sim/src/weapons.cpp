#include "sim/weapons.h"

#include <cmath>
#include <cstring>

#include "sim/x87.h"
#include "sim/x87_math.h"

namespace sim {
namespace {

double from_bits(std::uint64_t b) {
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}

// The original's constants (as stored, slightly off pi/2, pi/4 and 1/pi).
const double kHalfPi = from_bits(0x3FF921FB54442D11ull);
const double kQuarterPi = from_bits(0x3FE921FB54442D11ull);
const double kInvPi = from_bits(0x3FD45F306DC9C889ull);

} // namespace

std::int16_t ballistic_pitch(std::int32_t dx, std::int32_t dy, std::int32_t dz, std::int32_t speed, float min_angle,
                             std::int32_t gravity) {
    // Every operation below rounds to a double, as the x87 does at 53-bit precision.
    const double g = gravity;
    const double gg = static_cast<std::int32_t>(static_cast<std::uint32_t>(gravity) * static_cast<std::uint32_t>(gravity));
    const double y = dy, v = speed;
    const double h = x87::crt_hypot(dx, dz);
    const double gdy = g * y;
    const double h2 = h * h, v2 = v * v, d2 = y * y;
    const double h4 = h2 * h2;
    const double t = v2 - gdy * -2.0;
    const double s = d2 + h2;
    const double x = t * v2 + d2 * gg;
    const double disc = x * h4 - h4 * gg * s;
    if (!(disc >= 0.0)) return -32768;
    const double r = std::sqrt(disc);
    const double b = (v2 + gdy) * h2;
    const double two_s = s + s;
    const double q_flat = (r + b) / two_s, q_steep = (b - r) / two_s;

    // Launch angle = acos(sqrt(q) / v); a non-positive q gives (about) 90 degrees.
    // The flat solution is stored as a double, the steep one stays extended.
    double flat = kHalfPi;
    bool flat_nan = false;
    if (q_flat > 0.0) {
        const auto a = x87::crt_acos(std::sqrt(q_flat) / v);
        if (a) flat = x87::to_double(*a);
        else flat_nan = true;
    }
    std::optional<x87::Ext80> steep;  // nullopt here = NaN
    bool steep_is_half_pi = true;
    if (q_steep > 0.0) {
        steep = x87::crt_acos(std::sqrt(q_steep) / v);
        steep_is_half_pi = false;
    }

    double chosen;
    // Unordered comparisons (NaN) count as "less" and "equal" on the x87.
    const bool min_nan = std::isnan(min_angle);
    const bool flat_above_min = flat_nan || min_nan || static_cast<double>(min_angle) < flat;
    const bool flat_within = flat_nan || !(flat > kQuarterPi);
    if (flat_above_min && flat_within) {
        chosen = flat_nan ? std::nan("") : flat;
    } else {
        if (steep_is_half_pi) {
            if (!(kHalfPi > static_cast<double>(min_angle)) || kHalfPi > kQuarterPi) return -32768;
            chosen = kHalfPi;
        } else {
            if (!steep || min_nan) return -32768;  // unordered against the minimum
            if (x87::compare(*steep, static_cast<double>(min_angle)) <= 0) return -32768;
            if (x87::compare(*steep, kQuarterPi) > 0) return -32768;
            chosen = x87::to_double(*steep);  // the multiply rounds the extended angle
        }
    }
    return static_cast<std::int16_t>(x87::ftol32(chosen * 32768.0 * kInvPi));
}

// ---------------------------------------------------------------------------
// Weapon slot pass and aiming (docs/RE_MEMORY.md, "Weapons: slot pass and aiming").

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;

i32 wadd(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
i32 wsub(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }

const char* const kAimNames[3] = {"AimPrimary", "AimSecondary", "AimTertiary"};
const char* const kAimFromNames[3] = {"AimFromPrimary", "AimFromSecondary", "AimFromTertiary"};
const char* const kQueryNames[3] = {"QueryPrimary", "QuerySecondary", "QueryTertiary"};

constexpr u16 kNetOutOfRange = 0x1000, kNetFired = 0x400, kNetCommandFired = 0x800;

// Runs a script function now with one in/out argument starting at `value`; a missing
// function leaves the value alone.
i32 ask_script(Unit& unit, const char* name, i32 value) {
    if (unit.script) unit.script->call_in_out(unit.script->program().find_function(name), &value, nullptr, nullptr, nullptr);
    return value;
}

int script_index(const WeaponSlot& s) { return (s.flags & slot_flags::kScriptIndexMask) >> 2; }

WorldPos offset_from_unit(const Unit& unit, i32 piece) {
    const auto o = piece_local_offset(unit, piece);
    return {wadd(unit.pos[0], o[0]), wadd(unit.pos[1], o[1]), wadd(unit.pos[2], o[2])};
}

// The slot's target unit by id (any table entry, dead or alive); nullptr when the id is
// outside the table (the original reads past its table then).
Unit* unit_by_target_id(Weapons& w, std::int16_t id) {
    if (id <= 0 || static_cast<std::size_t>(id) >= w.units.size()) return nullptr;
    return &w.units[static_cast<std::size_t>(id)];
}

// Starts the slot's aim script (not run now) with its result sink cleared, broadcasts the
// start in a network game, and marks the slot as aiming.
void start_aim(Weapons& w, Unit& unit, int slot, u16 heading, u16 pitch) {
    WeaponSlot& s = unit.weapons[static_cast<std::size_t>(slot)];
    s.aim.result = 0;
    s.aim.handle_word = w.env.addresses.unit(unit.id) + 8u + static_cast<u32>(slot) * 0x1Cu;
    const char* name = aim_script_name(script_index(s));
    const int fn = unit.script && name ? unit.script->program().find_function(name) : -1;
    if (unit.script) unit.script->call_with_args(fn, &s.aim, false, 2, heading, pitch, 0, 0);
    if (w.env.networked) {
        // 0x10, unit id, function index, argument count, four arguments.
        std::array<u8, 22> p{};
        p[0] = 0x10;
        p[1] = static_cast<u8>(unit.id);
        p[2] = static_cast<u8>(unit.id >> 8);
        p[3] = static_cast<u8>(fn);
        p[4] = static_cast<u8>(static_cast<u32>(fn) >> 8);
        p[5] = 2;
        const u32 args[4] = {heading, pitch, 0, 0};
        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b) p[static_cast<std::size_t>(6 + a * 4 + b)] = static_cast<u8>(args[a] >> (8 * b));
        const PlayerSlot& owner = w.players[static_cast<std::size_t>(w.ticks[unit.id].owner)];
        w.systems.network_send(owner.net_id, p.data(), static_cast<i32>(p.size()));
    }
    s.flags |= slot_flags::kAiming;
}

} // namespace

const char* aim_script_name(int script_index) {
    return script_index >= 0 && script_index < 3 ? kAimNames[script_index] : nullptr;
}

WorldPos sweet_spot_position(Unit& target) {
    const i32 piece = ask_script(target, "SweetSpot", 0);
    // The box always contains the unit origin: every bound starts at 0.
    i32 lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    const auto& pieces = target.pose.pieces;
    if (piece >= 0 && static_cast<std::size_t>(piece) < pieces.size()) {  // the original does not check
        const auto& pts = pieces[static_cast<std::size_t>(piece)].points;
        for (std::size_t v = 0; v + 2 < pts.size(); v += 3)
            for (int k = 0; k < 3; ++k) {
                const i32 c = pts[v + static_cast<std::size_t>(k)];
                if (c < lo[k]) lo[k] = c;
                if (c > hi[k]) hi[k] = c;
            }
    }
    WorldPos p;
    for (int k = 0; k < 3; ++k) p[static_cast<std::size_t>(k)] = wadd(target.pos[static_cast<std::size_t>(k)], wadd(lo[k], hi[k]) / 2);
    return p;
}

WorldPos aim_from_position(Unit& unit, int script_index) {
    const bool known = script_index >= 0 && script_index < 3;
    i32 piece = known ? ask_script(unit, kAimFromNames[script_index], -1) : -1;
    if (piece == -1) piece = known ? ask_script(unit, kQueryNames[script_index], 0) : 0;
    return offset_from_unit(unit, piece);
}

WorldPos weapon_piece_position(Unit& unit, int script_index, i32 piece) {
    if (piece < 0) piece = script_index >= 0 && script_index < 3 ? ask_script(unit, kQueryNames[script_index], 0) : 0;
    return offset_from_unit(unit, piece);
}

bool weapon_target_position(Weapons& w, Unit& unit, int slot, WorldPos& pos) {
    WeaponSlot& s = unit.weapons[static_cast<std::size_t>(slot)];
    if (s.target_z != kUnitTarget) {
        // Ground point: terrain height, but never below the sea surface.
        pos[0] = static_cast<i32>(static_cast<u32>(static_cast<i32>(s.target_x)) << 16);
        pos[2] = static_cast<i32>(static_cast<u32>(static_cast<i32>(s.target_z)) << 16);
        const i32 sea = w.env.sea_level;
        const i32 h = w.env.heightmap ? w.env.heightmap->sample(s.target_x, s.target_z) : -1;
        pos[1] = static_cast<i32>(static_cast<u32>(h > sea ? h : sea) << 16);
        return true;
    }
    if (s.target_x == 0) return false;
    Unit* t = unit_by_target_id(w, s.target_x);
    if (!t || w.ticks[t->id].type_index == 0) {
        // The target died: forget it and tell the script which slot lost its target.
        s.target_x = 0;
        s.target_z = kUnitTarget;
        if (unit.script)
            unit.script->call_with_args(unit.script->program().find_function("TargetCleared"), nullptr, false, 1, slot, 0, 0, 0);
        return false;
    }
    pos = sweet_spot_position(*t);
    const UnitTickState& tt = w.ticks[t->id];
    const UnitTickState& mine = w.ticks[unit.id];
    // Veterans lead a moving target by its velocity times the shot's flight time (5/6
    // of the distance over the muzzle speed, in 16.16), except with cruise weapons.
    if ((s.flags & slot_flags::kEnabled) && s.type && !(s.type->flags & weapon_flags::kCruise) && tt.has_motion &&
        mine.kill_count > 5 && s.type->velocity != 0) {
        const double dx = wsub(unit.pos[0], pos[0]), dy = wsub(unit.pos[1], pos[1]), dz = wsub(unit.pos[2], pos[2]);
        const i32 dist = x87::ftol32(std::sqrt(dx * dx + dy * dy + dz * dz));
        const i64 scaled = static_cast<i64>(static_cast<u64>(static_cast<i64>(dist)) << 16);
        const i32 ticks = static_cast<i32>(scaled / static_cast<i64>(s.type->velocity));
        const i32 lead = static_cast<i32>((static_cast<i64>(ticks) * 0xCCCC) >> 16);
        for (std::size_t k = 0; k < 3; ++k)
            pos[k] = wadd(pos[k], static_cast<i32>((static_cast<i64>(tt.velocity[k]) * lead) >> 16));
    }
    return true;
}

Unit* weapon_target_unit(Weapons& w, Unit& unit, int slot) {
    const WeaponSlot& s = unit.weapons[static_cast<std::size_t>(slot)];
    if (s.target_z != kUnitTarget || s.target_x == 0) return nullptr;
    return unit_by_target_id(w, s.target_x);
}

bool weapon_in_range(Weapons& w, Unit& unit, int slot, const WorldPos& from, const WorldPos& target) {
    const WeaponType& type = *unit.weapons[static_cast<std::size_t>(slot)].type;
    // Squares of the 16.16 offsets, keeping their high words (whole units squared).
    auto square_hi = [](i32 v) { return static_cast<i32>(static_cast<u64>(static_cast<i64>(v) * v) >> 32); };
    const i32 dist2 = wadd(square_hi(wsub(target[0], from[0])), square_hi(wsub(target[2], from[2])));
    const i32 range2 = static_cast<i32>(static_cast<u32>(type.range) * static_cast<u32>(type.range));
    if (dist2 > range2) return false;
    if (type.flags & weapon_flags::kWaterWeapon) return true;
    // Other weapons need the unit's top above the water.
    const i32 top = static_cast<std::int16_t>(static_cast<u32>(from[1]) >> 16) +
                    static_cast<std::int16_t>(static_cast<u32>(unit.type ? unit.type->model_max_y : 0) >> 16);
    if (top <= static_cast<i32>(w.env.sea_level)) return false;
    if ((type.flags & weapon_flags::kBallistic) &&
        ballistic_pitch(wsub(from[0], target[0]), wsub(from[1], target[1]), wsub(from[2], target[2]), type.velocity,
                        type.min_barrel_angle, w.env.gravity) == -32768)
        return false;
    return true;
}

void direct_aim(Unit& unit, int script_index, const WorldPos& target, u16& heading, u16& pitch) {
    const WorldPos from = aim_from_position(unit, script_index);
    const i32 dx = wsub(from[0], target[0]), dy = wsub(from[1], target[1]), dz = wsub(from[2], target[2]);
    heading = static_cast<u16>(x87::atan2_scaled(dx, dz) - unit.heading);
    const i32 flat = x87::ftol32(x87::crt_hypot(dx, dz));
    // Pitch from the whole-unit parts of the height difference and the flat distance.
    const i32 rise = -static_cast<i32>(static_cast<std::int16_t>(static_cast<u32>(dy) >> 16));
    pitch = static_cast<u16>(x87::atan2_scaled(rise, static_cast<std::int16_t>(flat >> 16)));
}

bool aim_within_tolerance(const Unit& unit, const WeaponSlot& slot, u16 heading, u16 pitch) {
    const WeaponType& type = *slot.type;
    i32 tol_heading, tol_pitch;
    if (type.tolerance == 0) {
        tol_heading = tol_pitch = (unit.flags & 0xC) ? 2000 : 150;
    } else {
        tol_heading = type.tolerance;
        tol_pitch = type.pitch_tolerance != 0 ? type.pitch_tolerance : type.tolerance;
    }
    auto off = [](u16 a, u16 b) {
        const i32 d = static_cast<std::int16_t>(static_cast<u16>(a - b));
        return d < 0 ? -d : d;
    };
    return off(slot.aim_heading, heading) <= tol_heading && off(slot.aim_pitch, pitch) <= tol_pitch;
}

bool use_resources(PlayerSlot& owner, UnitTickState& unit, float energy, float metal) {
    // Unordered (NaN) comparisons count as "short".
    if (!(owner.energy_stored >= energy) || !(owner.metal_stored >= metal)) return false;
    owner.energy_stored = static_cast<float>(static_cast<double>(owner.energy_stored) - energy);
    unit.energy_use = static_cast<float>(static_cast<double>(energy) + unit.energy_use);
    if (owner.metal_stored >= metal) {
        owner.metal_stored = static_cast<float>(static_cast<double>(owner.metal_stored) - metal);
        unit.metal_use = static_cast<float>(static_cast<double>(metal) + unit.metal_use);
    }
    return true;
}

void process_weapon_slots(Weapons& w, Unit& unit, std::uint32_t& stale_word) {
    UnitTickState& t = w.ticks[unit.id];
    // The target position lives in the original's frame; its height word is the caller's
    // stale stack word, so it starts as that word and is left behind in it.
    WorldPos target{0, static_cast<i32>(stale_word), 0};
    for (int i = 0; i < 3; ++i) {
        WeaponSlot& s = unit.weapons[static_cast<std::size_t>(i)];
        if (!(s.flags & slot_flags::kEnabled)) continue;
        if (s.reload != 0) --s.reload;
        if (!weapon_target_position(w, unit, i, target)) {
            s.flags &= static_cast<u8>(~slot_flags::kAiming);
            continue;
        }
        const WeaponType& type = *s.type;
        if (type.fire == WeaponFireRoutine::None) continue;
        const bool stockpile = (type.flags & weapon_flags::kStockpile) != 0;
        const int si = script_index(s);
        if (type.flags & weapon_flags::kTurret) {
            if (!(s.flags & slot_flags::kAiming)) {
                bool aimed = false;
                u16 heading = 0, pitch = 0;
                if (type.flags & weapon_flags::kBallistic) {
                    const WorldPos from = aim_from_position(unit, si);
                    const i32 dx = wsub(from[0], target[0]), dy = wsub(from[1], target[1]), dz = wsub(from[2], target[2]);
                    heading = static_cast<u16>(x87::atan2_scaled(dx, dz) - unit.heading);
                    const std::int16_t p = ballistic_pitch(dx, dy, dz, type.velocity, type.min_barrel_angle, w.env.gravity);
                    pitch = static_cast<u16>(p);
                    aimed = p != -32768;
                } else if (type.flags & weapon_flags::kLineOfSight) {
                    direct_aim(unit, si, target, heading, pitch);
                    aimed = true;
                }
                if (aimed) {
                    s.aim_pitch = pitch;
                    s.aim_heading = heading;
                    start_aim(w, unit, i, heading, pitch);
                }
            }
        } else if ((type.flags & weapon_flags::kVerticalLaunch) && (!stockpile || s.stockpile != 0) &&
                   !(s.flags & slot_flags::kAiming)) {
            start_aim(w, unit, i, 0, 0);
        }
        if (s.reload != 0) continue;
        if (!weapon_in_range(w, unit, i, unit.pos, target)) {
            unit.net_dirty |= kNetOutOfRange;
            continue;
        }
        PlayerSlot& owner = w.players[static_cast<std::size_t>(t.owner)];
        const bool can_fire = stockpile ? s.stockpile != 0
                                        : owner.energy_stored >= type.energy_per_shot && owner.metal_stored >= type.metal_per_shot;
        if (!can_fire) continue;
        if (w.systems.fire(unit, i, weapon_target_unit(w, unit, i), target) == 0) continue;
        if (stockpile) {
            --s.stockpile;
            w.systems.stockpile_used(unit);
        } else {
            // Reload: the type's time, up to 30% shorter for veterans (6% per 5 kills), times
            // (120 - 20 x health fraction) / 100, so damaged units reload slower.
            u32 tier = t.kill_count / 5u;
            if (tier > 5) tier = 5;
            const u32 max = unit.type ? unit.type->max_damage : 0;
            const u32 hurt = max ? static_cast<u32>(static_cast<i32>(unit.hit_points) * 20) / max : 0;  // the original faults on 0
            const i32 health_factor = static_cast<i32>(120u - hurt);
            const i32 base = static_cast<i32>((100u - tier * 6u) * type.reload_time) / 100;
            s.reload = static_cast<u16>(static_cast<i32>(static_cast<u32>(health_factor) * static_cast<u32>(base)) / 100);
        }
        unit.net_dirty |= (type.flags & weapon_flags::kCommandFire) ? kNetCommandFired : kNetFired;
        if (!stockpile) use_resources(owner, t, type.energy_per_shot, type.metal_per_shot);
    }
    stale_word = static_cast<u32>(target[1]);
}

} // namespace sim
