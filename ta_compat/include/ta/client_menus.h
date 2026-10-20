#pragma once
// The original game's front end on the client's front end runner (client/front_end.h):
// its screens from guis\*.GUI with their backgrounds and art, its sounds and fonts, the
// skirmish set-up, options, loading, in-game menus, the end of a battle and the music
// tracks (spec: docs/RE_MEMORY.md, "Client", "Menus").
//
// Other parts (campaign, multiplayer, save/load) plug in through client::FrontEndHooks;
// they can build screens from the game's GUI files with MenuArt::layout() and
// client::LayoutScreen, and start battles with SkirmishSession-like sessions.

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "client/front_end.h"
#include "client/menu.h"
#include "client/music.h"
#if __has_include("client/net_menu.h")
#include "client/net_menu.h"
#define TA_CLIENT_HAVE_NET 1
#endif
#include "sim/rng.h"
#include "ta/battle_loader.h"
#include "ta/client_world.h"
#include "ta/hpi.h"

namespace ta {

// Everything the menus draw and play, loaded from the game files on demand.
class MenuArt {
public:
    bool load(const std::string& game_dir, std::string& error);

    client::MenuContent& content() { return content_; }
    const std::string& game_dir() const { return game_dir_; }
    Vfs& vfs() { return vfs_; }

    // A screen's layout by GUI file name ("SKIRMISH" or "SKIRMISH.GUI"), with the
    // background the original shows behind it.
    const client::ScreenLayout* layout(const std::string& gui);
    // A bitmaps\<name>.PCX picture with its own palette (null when missing).
    const render::Sprite8* picture(const std::string& name, const render::Palette** palette = nullptr);
    // Frames of a GAF sequence (null when missing).
    const std::vector<render::Sprite8>* sequence(const std::string& gaf_path, const std::string& name);
    // A sound of the sound bank by its allsound.tdf section name (-1: none).
    int sound(const std::string& name);
    // Interface colour k for a palette (GUIPAL entry k's nearest colour in it).
    std::uint8_t gui_colour(const render::Palette& palette, int k) const;
    // A map's preview picture (the TNT's minimap), scaled into w x h; null when missing.
    const render::Sprite8* map_preview(const std::string& map, int w, int h);

    // The game's music tracks: music\<n>.mp3 for n = 1.. (decoded on demand).
    int music_tracks() const { return music_tracks_; }
    std::shared_ptr<const client::Sample> music_track(int n);

    // Skirmish maps (maps\*.ota with a network schema), sorted by name.
    const std::vector<std::string>& skirmish_maps();
    // Side names from gamedata\sidedata.tdf (ARM, CORE, ...).
    const std::vector<std::string>& sides() const { return sides_; }

    // Movies: Data\<name> beside the game (the original's 1-5.zrb, loose files; empty when
    // missing). The original plays them only on a full screen; the window front end stands for
    // one, headless runs do not (unless asked to: tests).
    std::vector<std::uint8_t> movie(const std::string& name) const;
    bool full_screen = true;
    // A bit font fonts\<name>.FNT (null when missing).
    const client::Font* fnt_font(const std::string& name);
    // A GAF file's sequence by path and name (null when missing), its frames' offsets kept.
    const std::vector<render::Sprite8>* gaf_sequence(const std::string& gaf_path, const std::string& name) { return sequence(gaf_path, name); }
    // GAF frame durations of a sequence (ticks; empty when missing) and its loop byte.
    std::vector<int> sequence_times(const std::string& gaf_path, const std::string& name, bool* loop = nullptr);

private:
    const std::vector<std::uint8_t>* gaf(const std::string& path);
    client::Font gaf_font(const std::string& path);

    std::string game_dir_;
    Vfs vfs_;
    client::MenuContent content_;
    std::array<render::Rgb, 256> guipal_{};
    std::map<std::string, std::vector<std::uint8_t>> gafs_;
    std::map<std::string, const std::vector<render::Sprite8>*> seq_index_;
    std::deque<std::vector<render::Sprite8>> sequences_;
    std::deque<render::Sprite8> sprites_;
    std::deque<render::Palette> palettes_;
    std::map<std::string, std::pair<const render::Sprite8*, const render::Palette*>> pictures_;
    std::map<std::string, std::unique_ptr<client::ScreenLayout>> layouts_;
    std::deque<client::Font> fonts_;
    std::map<std::string, int> sound_index_;
    int music_tracks_ = 0;
    std::map<int, std::shared_ptr<const client::Sample>> music_cache_;
    std::vector<std::string> maps_;
    bool maps_done_ = false;
    std::vector<std::string> sides_;
    std::map<std::string, std::unique_ptr<render::Sprite8>> previews_;
};

// A skirmish (or any battle built from a BattleRequest) with the playable client.
class SkirmishSession : public client::BattleSession {
public:
    // Loads and starts the battle; null with `error` on failure.
    // wrap / ttach (network games): the network side's outside and its hook to the battle.
    static std::unique_ptr<SkirmishSession> start(const std::string& game_dir, const BattleRequest& request, int human,
                                                  const client::ClientOptions& options, const client::LoadProgress& progress,
                                                  std::string& error,
                                                  const std::function<sim::BattleOutside&(sim::BattleOutside&)>& wrap = {},
                                                  const std::function<void(sim::Battle&)>& attach = {});
    sim::Battle& battle() override { return world_->battle(); }
    client::Content& content() override { return world_->content(); }
    client::GameClient& client() override { return *client_; }
    client::BattleOutcome outcome() override;
    void surrender() override;
    std::vector<client::PlayerStats> stats() override;
    bool end_table(client::EndTable& t) override;
    client::BattleLauncher restart_launcher(int difficulty) override;
    int difficulty() const override;
    std::string battle_name() const override;
    ClientWorld& world() { return *world_; }

private:
    std::string dir_;
    int human_ = 0;
    client::ClientOptions options_;
    bool network_start_ = false;
    std::unique_ptr<ClientWorld> world_;
    std::unique_ptr<client::GameClient> client_;
    bool surrendered_ = false;
};

// Client options from the settings (interface type, speeds, text, unit chat, ...).
client::ClientOptions client_options(const client::Settings& settings, int width, int height);

#ifdef TA_CLIENT_HAVE_NET
// The multiplayer part's configuration: network games load through SkirmishSession with the
// network side wrapped around the battle's outside; the room compares unit checksums.
client::NetMenuConfig network_config(MenuArt& art, client::FrontEnd* settings_from);
#endif

// The original's message box (MSGBOX.GUI, 0x4ABD90) over the screen shown: the text word-wrapped
// to 320 pixels, one centred line per row, OK; the screen under it dimmed — `under` its box
// (null: the whole frame).
void show_message(client::FrontEnd& fe, MenuArt& art, const std::string& text, const render::ClipRect* under = nullptr);

// Text broken for a width as the original's 0x4AC4C0 does (at the end of a word, when the line so
// far is at least `width` wide, the last space or hyphen before becomes a line break).
std::string wrap_for_width(const client::Font& font, const std::string& text, int width);

// The campaign's part of the end of a battle (the end state machine 0x41F7F0, states 5-7, for a
// mission; docs/RE_MEMORY.md "Campaign"): the glamour picture faded in after a won mission, the
// ending movies after the last one, the statistics screen's campaign buttons and mission list.
struct CampaignEnd {
    bool buttons = false;    // Start, Load / Save Game, the mission list, Difficulty (not after the last mission won)
    bool final_win = false;  // the last mission won: the ending movies instead of the statistics
    std::vector<std::string> final_movies;
    std::vector<std::string> missions;  // the campaign's missions' names
    std::string results;                // 'U' / 'W' / 'L' per mission
    int mission = 0;                    // the mission just played
    bool won = false;
    int difficulty = 1;
    const render::Sprite8* glamour = nullptr;  // a won mission's picture (null: none)
    const render::Palette* glamour_palette = nullptr;
    std::shared_ptr<client::Sample> glamour_sound;
    const client::Font* click_font = nullptr;  // "Click to continue."
    std::function<void(client::FrontEnd&, int mission)> start;  // Start / a mission picked: its briefing
    std::function<void(client::FrontEnd&)> load, save;          // the load / save menus
    std::function<void(int difficulty)> set_difficulty;
};
// Fills `out` when the battle is a campaign mission (and records the mission's result).
bool campaign_end(MenuArt& art, client::BattleSession& session, client::BattleOutcome outcome, CampaignEnd& out);

// The C runtime's rand() of the program's main thread as the menus draw from it (the main
// menu's sparks, the briefing's wind); what is left of it seeds a battle's rand().
sim::Lcg15& menus_rand();

// Fills the front end set-up with the original's screens. `art` must outlive the run.
void make_front_end(MenuArt& art, client::MusicPlayer* music, client::FrontEndSetup& setup);

// The statistics screen on its own for a given table (tests: ta_play --end-screen), with the
// battle's interface colours made from the game palette.
std::unique_ptr<client::Screen> make_end_screen(MenuArt& art, const client::EndTable& table, const CampaignEnd* campaign = nullptr);

}  // namespace ta
