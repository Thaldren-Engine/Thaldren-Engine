#pragma once
// The campaign and saved games in the front end (client/front_end.h hooks): the campaign
// menu (campaigns, missions reached, difficulty), the briefing (text pages and narration),
// mission battles with progression (a won mission opens the next one), and the load and save
// screens over the bank files of saved games (docs/RE_MEMORY.md, "Campaign", "Saved games").
//
// install_campaign_hooks() fills FrontEndHooks::campaign_menu, load_game_menu and
// save_game_menu. Exact saves need the running session to keep the battle's input journal:
// sessions that do implement JournaledSession; for others the save screen says so.

#include <string>

#include "ta/saved_game.h"

namespace client {
struct FrontEndHooks;
class BattleSession;
}  // namespace client

namespace ta {

class MenuArt;

// A session that can be saved exactly: its start description and its input journal so far.
class JournaledSession {
public:
    virtual ~JournaledSession() = default;
    // Fills everything of the save but the summary's description; false (with a reason) when
    // the session cannot be saved now.
    virtual bool make_save(SavedGame& out, std::string& why) = 0;
};

// The campaign progress kept in the settings: "Campaign <name>" = missions completed.
std::string campaign_progress_key(const std::string& campaign);

// Fills the hooks (`art` and the directory outlive the front end; saves are <dir>\*.sav).
void install_campaign_hooks(client::FrontEndHooks& hooks, MenuArt& art, const std::string& save_dir);

}  // namespace ta
