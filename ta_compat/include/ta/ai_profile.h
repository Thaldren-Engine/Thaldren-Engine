#pragma once
// The computer player's profile (ai\*.txt) and the per-type data it reads, imported for
// sim/ai.h (see docs/RE_MEMORY.md, "Computer player").
//
// A profile is a text of command lines run through the game's console command
// dispatcher: "plan <difficulty words>" gates the lines after it, "weight <unit or
// category> <factor>" scales the build weight of the named types, "limit <unit or
// category> <count>" caps how many of them are built. Units whose definitions carry
// their own "ai_weight" text (downloadable units) run that text too.

#include <cstdint>
#include <string>
#include <vector>

#include "sim/ai.h"
#include "sim/weapon_slot.h"
#include "ta/hpi.h"
#include "ta/unitdefs.h"

namespace ta {

// A unit definition as the computer player sees it. `weapons` is indexed by weapon slot
// (to_weapon_types); build options keep the definition indices, so the battle's type
// indices must equal the definition indices for the computer player.
void to_ai_type(const UnitDefRecord& d, const std::vector<sim::WeaponType>& weapons, sim::AiTypeInfo& out);

// The profile path of a map: the OTA schema's "aiprofile" stem as ai\<stem>.txt, else
// ai\default.txt (`schema`: the [Schema N] section the game selects).
std::string ai_profile_path(const std::vector<std::uint8_t>& ota, int schema);

struct AiProfileReport {
    std::size_t lines = 0, plans = 0, weights = 0, limits = 0, other = 0, includes = 0;
    std::vector<std::string> notes;  // lines the model does not carry out (they would create units)
    // The original runs profile text through the whole command registry with every permission
    // bit: with `defer_other` the lines that are not plan / weight / limit are kept here, in
    // order, for the battle's command table (unit names create units at the pointer's map point,
    // console commands run, unknown words run debugdat\<word>.txt).
    bool defer_other = false;
    std::vector<std::string> deferred;
};

// Runs command text on the computer players (the original's multi-line dispatcher).
void run_ai_commands(const std::string& text, Vfs& vfs, const UnitDefTable& defs, sim::Ai& ai, AiProfileReport& report);
// The whole profile load of a battle (0x4648E0): the map's profile (or ai\default.txt), then
// for every active computer player the downloadable units' own lines. Call after
// Battle::start_players(). False when no profile text was found.
bool load_ai_profile(Vfs& vfs, const std::string& path, const UnitDefTable& defs, sim::Ai& ai, AiProfileReport& report);

}  // namespace ta
