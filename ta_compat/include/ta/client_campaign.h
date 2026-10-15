#pragma once
// The campaign and saved games in the front end (client/front_end.h hooks), as the original
// runs them (docs/RE_MEMORY.md, "Campaign", "Saved games"):
//   - a new campaign starts at its first mission (any mission with the "AllMissions" setting,
//     the original's DRDEATH cheat) and resets the mission results;
//   - the briefing, then the battle; at its end the mission's result goes into the results
//     string ('W' won, 'L' lost, 'U' not played; one character per mission) and the
//     end-of-mission screen lists the missions with their results: start any of them (the one
//     after a won mission is selected), save between missions, change the difficulty;
//   - saved games: SAVEGAME\<name>.SAV in the original's format (ta/saved_game.h), written
//     from the in-game menu (the running battle) or between missions (only the summary, with
//     the next mission), loaded from the load menu.
// The results string is the campaign's progress: the original keeps it only in its session
// and in saved games (its registry functions for it are never called in 3.1).

#include <string>

#include "ta/saved_game.h"

namespace client {
struct FrontEndHooks;
class BattleSession;
}  // namespace client

namespace ta {

class MenuArt;
class SkirmishSession;

// A campaign being played: its name, side, the current mission, difficulty and results.
struct CampaignProgress {
    std::string campaign;
    std::string side = "ARM";
    int mission = 0;
    int difficulty = 1;
    std::string results = std::string(25, 'U');  // GS+0x391CF: 'U' / 'W' / 'L' per mission
    bool won = false;                            // the last mission's outcome (GS+0x391AF)
};

// The results string after loading a save's "Thumbs": kept when it has 25 characters, else
// all 'U' again (the load menu 0x492360).
std::string progress_from_thumbs(const std::string& thumbs);

// The save context of a running battle (the in-game menu's save, 0x492DF0 -> 0x4326B0).
SaveContext battle_save_context(SkirmishSession& session, const CampaignProgress* campaign, const std::string& description);

// Fills the hooks (`art` and the directory outlive the front end; saves are <dir>\*.SAV).
void install_campaign_hooks(client::FrontEndHooks& hooks, MenuArt& art, const std::string& save_dir);

}  // namespace ta
