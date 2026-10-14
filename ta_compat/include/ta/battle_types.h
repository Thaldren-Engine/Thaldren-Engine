#pragma once
// Imported definitions -> the engine's battle type data (sim/battle.h). Shared
// by the tools that set up a battle from the original game's files.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/battle.h"
#include "ta/cob.h"
#include "ta/hpi.h"
#include "ta/unitdefs.h"
#include "ta/weapondefs.h"

namespace ta {

// One weapon type per definition slot (index = slot). Sounds are left to the
// presentation (none here).
sim::WeaponType to_weapon_type(const WeaponDefRecord& d);
std::vector<sim::WeaponType> to_weapon_types(const WeaponDefTable& defs);

// A unit type's model and script, loaded from objects3d\<objectname>.3DO and
// scripts\<unitname>.COB. Must outlive the battle types made from it.
struct UnitTypeAssets {
    std::vector<std::uint8_t> model_file, cob_file;
    sim::PoseModel pose_model;
    CobFile cob;
    bool has_model = false, has_script = false;
    bool has_footprint = false;  // the model's selection footprint (ground settle)
    std::array<std::array<std::int32_t, 2>, 4> footprint{};
};
// False (with a reason) when the model cannot be loaded; a missing script is allowed.
bool load_unit_assets(Vfs& vfs, const UnitDefRecord& d, UnitTypeAssets& out, std::string& error);

// Fills every part of a battle unit type from its definition. `weapons` is indexed by
// weapon definition slot (to_weapon_types); a definition's missing weapon (-1) is null.
// Values are taken as the original stores them (a max damage of 0 stays 0).
void to_battle_unit_type(const UnitDefRecord& d, const UnitTypeAssets& assets, const std::vector<sim::WeaponType>& weapons,
                         sim::BattleUnitType& out);
// The targeting part of a battle unit type (sim/targeting.h): category masks by name from the
// definition table's registry (a type's bit is its definition index), sight, leash, transport
// and water limits.
void to_targeting_type(const UnitDefRecord& d, const CategoryRegistry& categories, const std::vector<sim::WeaponType>& weapons,
                       sim::TargetingTypeInfo& out);

// The movement classes of gamedata\moveinfo.tdf (a class is in use when it has a name).
std::array<sim::MoveClass, sim::kMoveClasses> to_move_classes(const UnitDefContext& ctx);

} // namespace ta
