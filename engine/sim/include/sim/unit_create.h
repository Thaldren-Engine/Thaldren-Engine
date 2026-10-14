#pragma once
// Unit creation: a free entry in the player's part of the unit table becomes a
// new unit of a type (see docs/RE_MEMORY.md, "Unit creation"). The battle's
// entry point is sim::Battle::create_unit (sim/battle.h); the pieces here are
// the steps that do not need the whole battle.

#include <array>
#include <cstdint>

#include "sim/pose.h"
#include "sim/projectile.h"
#include "sim/script_vm.h"
#include "sim/unit.h"
#include "sim/weapon_slot.h"

namespace sim {

// Unit type data creation needs (besides UnitTypeInfo, UnitTickTypeInfo and GroundSettleType).
struct UnitCreateType {
    std::uint32_t type_flags = 0;      // the type's flag word (create_flags)
    std::int32_t max_per_player = -1;  // most units of this type one player may own, -1 = no limit
    std::uint8_t movement = 0;         // 1: mobile (motion object); 0: structure (yard map)
    std::uint8_t build_menu_pages = 0;
    std::int16_t footprint_x = 0, footprint_z = 0;  // cells
    std::uint16_t build_angle = 0;     // spread of a structure's random facing; a mobile unit's facing
    float extracts_metal = 0.0f;       // metal extraction per cell of metal under the footprint
    std::array<const WeaponType*, 3> weapons{};
    const PoseModel* model = nullptr;
    const ScriptProgram* script = nullptr;  // nullptr: the type has no unit script
};

namespace create_flags {
constexpr std::uint32_t kStandingMoveMask = 0x3, kStandingFireMask = 0xC, kInitCloaked = 0x10, kZBuffer = 0x80,
                        kAirBase = 0x200, kHasWeapons = 0x10000, kActivateWhenBuilt = 0x40000,
                        kLoadable = 0x800000, kIsFeature = 0x1000000;
}

// Packets announcing a new unit (sent to the owner's network id).
constexpr std::int32_t kUnitCreatedPacketSize = 23;  // 0x09, type, id, x, y, z, pitch, heading, bank
constexpr std::int32_t kUnitFinishedPacketSize = 5;  // 0x12, id, id (only for finished structures)

// Weapon slot set-up (0x49E070): each slot takes the type's weapon, is enabled when
// the weapon's index is nonzero, and keeps the height between its Query and AimFrom
// pieces (times 1.25) for ballistic shots; the script then hears SetMaxReloadTime with
// the longest reload in milliseconds.
void setup_weapon_slots(Unit& unit, const std::array<const WeaponType*, 3>& weapons);

// Metal extraction (0x437840): for a type that extracts metal, the metal of the cells under
// the footprint (each cell counts its metal + 1) sets the unit's extraction rate and the
// script hears SetSpeed with the sum.
void update_metal_extraction(Unit& unit, const MapGrid& map, float extracts_metal);

} // namespace sim
