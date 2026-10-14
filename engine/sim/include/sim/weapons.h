#pragma once
// Weapons: aiming, ballistics, reload and firing (see docs/RE_MEMORY.md, "Weapons").

#include <array>
#include <cstdint>
#include <vector>

#include "sim/heightmap.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"
#include "sim/weapon_slot.h"

namespace sim {

// Pitch for a ballistic shot from the weapon to a target offset (dx, dy, dz)
// at muzzle speed `speed` under `gravity`: the higher of the two solutions that
// lies above `min_angle` (radians) and at most 45 degrees. Returns the pitch in
// half-turns of 32768, or -32768 when no solution qualifies. Bit-exact with the
// original's double/x87 evaluation, including its NaN cases (pitch 0).
std::int16_t ballistic_pitch(std::int32_t dx, std::int32_t dy, std::int32_t dz, std::int32_t speed, float min_angle,
                             std::int32_t gravity);

using WorldPos = std::array<std::int32_t, 3>;  // 16.16 world coordinates

struct WeaponEnvironment {
    bool networked = false;       // aim script starts are broadcast (the network game flag)
    std::uint8_t sea_level = 0;
    std::int32_t gravity = 0;
    const HeightMap* heightmap = nullptr;
    OriginalAddresses addresses;  // scripts see the aim result sink as an original address
};

// Systems the slot pass reaches outside this module.
class WeaponSystems {
public:
    virtual ~WeaponSystems() = default;
    // Runs the slot's fire routine (its type's `fire`) at the target unit (or none) and
    // position; nonzero when a shot went off. The routine may change `target_pos`.
    virtual std::int32_t fire(Unit& unit, int slot, Unit* target, WorldPos& target_pos) = 0;
    virtual void network_send(std::int32_t net_id, const std::uint8_t* packet, std::int32_t size) = 0;
    virtual void stockpile_used(Unit& unit) = 0;  // user interface: refresh a shown stockpile count
};

struct Weapons {
    std::vector<Unit>& units;  // index = unit id
    std::vector<UnitTickState>& ticks;
    std::vector<PlayerSlot>& players;
    WeaponEnvironment& env;
    WeaponSystems& systems;
};

// Script function names by script index (0..2).
const char* aim_script_name(int script_index);

// Centre of the box spanned by the origin and the transformed vertices of the
// piece the unit's SweetSpot script names (piece 0 without one), plus the unit position.
WorldPos sweet_spot_position(Unit& target);
// World position of the weapon's aim-from piece: AimFrom<n> (starting at -1); if it
// leaves -1, Query<n> (starting at 0). Script indexes other than 0..2 are undefined in the original.
WorldPos aim_from_position(Unit& unit, int script_index);
// World position of `piece`, or of the Query<n> piece when `piece` is negative.
WorldPos weapon_piece_position(Unit& unit, int script_index, std::int32_t piece);

// Where the slot aims: a ground point at terrain height (at least sea level), or the
// target unit's sweet spot, led by its motion for veteran (more than 5 kills) shooters.
// False when there is no target; a dead target is cleared (and the script told).
bool weapon_target_position(Weapons& w, Unit& unit, int slot, WorldPos& pos);
// The slot's target unit, or nullptr for a ground point / no target (dead or not).
Unit* weapon_target_unit(Weapons& w, Unit& unit, int slot);
// Whether the slot's weapon can reach `target` from `from` (range; above the water
// for a non-water weapon; a ballistic solution for a ballistic one).
bool weapon_in_range(Weapons& w, Unit& unit, int slot, const WorldPos& from, const WorldPos& target);
// Heading (relative to the unit) and pitch from the aim-from piece straight at the target.
void direct_aim(Unit& unit, int script_index, const WorldPos& target, std::uint16_t& heading, std::uint16_t& pitch);
// Whether the slot's current aim is within its type's tolerances of (heading, pitch).
bool aim_within_tolerance(const Unit& unit, const WeaponSlot& slot, std::uint16_t heading, std::uint16_t pitch);
// Pays a shot from the owner's stored resources into the unit's use counters: nothing
// (false) if either is short; metal only if still affordable after the energy.
bool use_resources(PlayerSlot& owner, UnitTickState& unit, float energy, float metal);

// One step of the unit's weapon slots: reload countdown, target, aim scripts, range
// check and firing. `stale_word` is the unit pass's stack word (see "Stale stack word"):
// the original leaves the height of the last resolved target position there.
void process_weapon_slots(Weapons& w, Unit& unit, std::uint32_t& stale_word);

} // namespace sim
