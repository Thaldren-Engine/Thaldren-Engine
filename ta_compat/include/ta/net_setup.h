#pragma once
// Game-data side of network games (docs/RE_MEMORY.md, "Network and sync"): the unit
// definition checksums the battle room compares, and the battle's network tables.

#include <cstdint>
#include <string>
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

// The map checksum the battle room compares (side record +0xA9, 0x4373A0): the simple
// checksums of the TNT's 64-byte header, of its attribute block (width x height x 4 bytes at the
// header's attribute offset) and of its feature name block (count x 0x84 bytes), XORed with the
// checksum of the OTA's [GlobalHeader] section. 0 when the TNT is missing or not a TNT.
std::uint32_t map_sync_checksum(Vfs& vfs, const std::string& map);

// The weapon table by weapon index (fire packets name weapons by index).
void set_network_weapons(sim::Battle& battle, const std::vector<sim::WeaponType>& weapons);

}  // namespace ta
