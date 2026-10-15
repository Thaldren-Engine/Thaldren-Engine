#pragma once
// Game-data side of network games (docs/RE_MEMORY.md, "Network and sync"): the unit
// definition checksums the battle room compares, and the battle's network tables.

#include <cstdint>
#include <utility>
#include <vector>

#include "sim/battle.h"
#include "ta/hpi.h"
#include "ta/unitdefs.h"

namespace ta {

// Per unit definition: (key, checksum) as the original exchanges them (0x42A610): the key is
// the FBI file's simple checksum; the checksum XORs the simple checksums of
// scripts\<unitname>.cob, of every guis\<unitname>*.gui, of download\<unitname>.tdf, and the
// definition's weapon section hash.
std::vector<std::pair<std::uint32_t, std::uint32_t>> unit_sync_checks(Vfs& vfs, const UnitDefTable& defs);

// The weapon table by weapon index (fire packets name weapons by index).
void set_network_weapons(sim::Battle& battle, const std::vector<sim::WeaponType>& weapons);

}  // namespace ta
