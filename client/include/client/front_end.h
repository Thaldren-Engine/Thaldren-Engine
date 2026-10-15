#pragma once
// The front end: the menu screens before and after a battle, the loading screen,
// the battle itself and the end-of-game screens, as one stack of screens.
//
// This header is the ENTRY-POINT INTERFACE other parts plug into (campaign,
// multiplayer, save/load, chat). Its signatures are kept stable; implementations
// live in client/src/front_end.cpp (engine side, no game data names) and the
// importer side (ta_compat) that builds screens from the game's GUI files.
//
// How a part plugs in:
//  * Campaign: set FrontEndHooks::campaign_menu. The main/single-player menu's
//    campaign button calls it and pushes the returned screen. To start a
//    mission, the campaign screen calls FrontEnd::start_battle with a launcher
//    that builds the BattleSession (map, mission script, players). When the
//    battle ends, the session's after_battle() picks what comes next (debriefing,
//    the next mission's briefing, back to the campaign menu).
//  * Multiplayer: set FrontEndHooks::multiplayer_menu (the main menu's
//    multiplayer button). The lobby calls FrontEnd::start_battle with a launcher
//    returning a session whose frame() runs the lockstep (BattleSession::frame is
//    virtual for that) and whose send_chat() sends chat to the other players.
//  * Save / load: set FrontEndHooks::load_game_menu (single-player menu "Load
//    game" and the in-game menu's load button) and save_game_menu (in-game menu's
//    save button; gets the running session).
//  * Chat: the in-game chat line (Enter in battle) hands the typed line to the
//    running session's send_chat(); incoming lines are shown with
//    GameClient::message().

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "client/content.h"
#include "client/game_client.h"
#include "render/image.h"

namespace sim {
class Battle;
}

namespace client {

class FrontEnd;
class MusicPlayer;   // client/music.h
struct MenuContent;  // client/menu.h

// ---------------------------------------------------------------- settings
// The persistent settings (the original keeps them in the registry; here a
// key = value text file). Keys are plain names; values are integers or text.
class Settings {
public:
    int get_int(const std::string& key, int fallback) const;
    std::string get_text(const std::string& key, const std::string& fallback) const;
    void set_int(const std::string& key, int value);
    void set_text(const std::string& key, const std::string& value);
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    const std::map<std::string, std::string>& values() const { return values_; }

private:
    std::map<std::string, std::string> values_;
};

// ---------------------------------------------------------------- screens
// One screen of the front end. It draws onto the 8-bit frame (the front end's
// palette, see FrontEnd::palette) and takes input in frame coordinates.
class Screen {
public:
    virtual ~Screen() = default;
    // Becomes the top screen (also when a screen above it is popped).
    virtual void enter(FrontEnd&) {}
    virtual void handle(FrontEnd& fe, const InputEvent& e) = 0;
    // Once per pass of the main loop.
    virtual void frame(FrontEnd&, std::uint32_t /*now_ms*/) {}
    virtual void draw(FrontEnd& fe, render::Image8& out) = 0;
    // Screens under a see-through one are drawn first (dialogs).
    virtual bool see_through() const { return false; }
    // The palette the screen is shown with (null: the one below, else the game palette).
    virtual const render::Palette* palette() const { return nullptr; }
};

// ---------------------------------------------------------------- battles
// How a battle ended for the local player.
enum class BattleOutcome : std::uint8_t { None, Victory, Defeat, Draw, Quit };

// Per-player totals for the end-of-game statistics screen.
struct PlayerStats {
    std::string name;
    int colour = 0, side = 0;
    bool won = false, lost = false;
    int units_built = 0, units_killed = 0, units_lost = 0;
    int buildings_built = 0, buildings_killed = 0, buildings_lost = 0;
    float energy_produced = 0, energy_used = 0, energy_wasted = 0;
    float metal_produced = 0, metal_used = 0, metal_wasted = 0;
    float energy_excess = 0, metal_excess = 0, energy_received = 0, metal_received = 0, energy_sent = 0, metal_sent = 0;
};

// A running battle with its client: owns the battle, its content and the
// GameClient over it. Skirmish, campaign missions, multiplayer games and loaded
// games are all BattleSessions.
class BattleSession {
public:
    virtual ~BattleSession() = default;
    virtual sim::Battle& battle() = 0;
    virtual Content& content() = 0;
    virtual GameClient& client() = 0;
    // One pass of the main loop (default: the client's own step clock).
    // Multiplayer overrides it to run the lockstep. Returns steps run.
    virtual int frame(std::uint32_t now_ms) { return client().frame(now_ms); }
    // The local player's outcome so far (None while the battle goes on).
    virtual BattleOutcome outcome() = 0;
    // A line typed into the in-game chat line (default: shown locally only).
    virtual void send_chat(const std::string& text) { client().message(text); }
    // The local player gives up (in-game menu).
    virtual void surrender() {}
    // Totals for the statistics screen.
    virtual std::vector<PlayerStats> stats() { return {}; }
    // Called once the battle and its end screens are over; may push the next
    // screen (campaign debriefing / next mission). Return false to go back to
    // the screen the battle was started from (the default).
    virtual bool after_battle(FrontEnd&, BattleOutcome) { return false; }
    // Set by the front end: the session reports combat involving the local player here (+1 a
    // hit, +5 a kill it scores; the music's battle/peace switch).
    std::function<void(int)> combat_activity;
};

// Progress of a battle being loaded: fraction 0..1 and the stage's name. The
// front end redraws the loading screen on every call.
using LoadProgress = std::function<void(float fraction, const std::string& stage)>;
// Builds a session (game data, map, battle start); nullptr with `error` on failure.
using BattleLauncher = std::function<std::unique_ptr<BattleSession>(const LoadProgress& progress, std::string& error)>;

// ---------------------------------------------------------------- hooks
// Entry points other parts fill in. An empty hook leaves its button disabled.
struct FrontEndHooks {
    // Single-player menu > campaign: the campaign's first screen.
    std::function<std::unique_ptr<Screen>(FrontEnd&)> campaign_menu;
    // Main menu > multiplayer: the connection / lobby screens.
    std::function<std::unique_ptr<Screen>(FrontEnd&)> multiplayer_menu;
    // Single-player menu > load game, and the in-game menu's load button
    // (session = the running battle, or nullptr from the menus).
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession* session)> load_game_menu;
    // In-game menu > save game.
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession& session)> save_game_menu;
};

// ---------------------------------------------------------------- front end
class FrontEnd {
public:
    virtual ~FrontEnd() = default;
    // Screen stack.
    virtual void push(std::unique_ptr<Screen> screen) = 0;
    virtual void pop() = 0;
    // Pops every screen above the main menu.
    virtual void to_main_menu() = 0;
    // Shows the loading screen, runs the launcher, then plays the battle; when
    // it ends: victory/defeat, statistics, then session->after_battle(), else
    // back to the screen on top now. On failure shows the error and stays.
    virtual void start_battle(BattleLauncher launcher) = 0;
    // The running battle (nullptr in the menus).
    virtual BattleSession* session() = 0;
    // Leaves the running battle (in-game menu quit / surrender, or a loaded game
    // replacing it).
    virtual void end_battle(BattleOutcome outcome) = 0;
    // Ends the program.
    virtual void quit() = 0;

    virtual Settings& settings() = 0;
    virtual FrontEndHooks& hooks() = 0;
    // Menu art and sounds (the importer's), the frame size, the menu palette.
    virtual const MenuContent& menu_content() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    // Plays an interface sound (index into menu_content().samples).
    virtual void play_sound(int sample) = 0;
    // The music player (null when there is no music).
    virtual MusicPlayer* music() = 0;
    // Applies the settings (volumes, speeds, interface options) after an options screen.
    virtual void apply_settings() = 0;
    // Milliseconds since the program started (the main loop's clock).
    virtual std::uint32_t now_ms() const = 0;
    // The desktop's size in pixels (the screen-size choices depend on it).
    virtual void desktop_size(int& w, int& h) const = 0;
};

// ---------------------------------------------------------------- running it
// What the importer gives the front end (the game's screens are built there).
struct FrontEndSetup {
    MenuContent* content = nullptr;
    int width = 640, height = 480;
    std::string settings_path;           // where the settings live (loaded at start, saved at exit)
    FrontEndHooks hooks;
    std::function<std::unique_ptr<Screen>(FrontEnd&)> main_menu;
    // Draws the loading screen (fraction 0..1, stage name).
    std::function<void(FrontEnd&, render::Image8&, float, const std::string&)> draw_loading;
    // The screens after a battle (outcome and statistics); popping below them ends the battle.
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession&, BattleOutcome)> battle_over;
    // The in-game menu (Tab or F2 in a battle).
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession&)> game_menu;
    MusicPlayer* music = nullptr;
    // Applies the settings to a running battle's client (game speed, interface type, ...).
    std::function<void(FrontEnd&, BattleSession&)> apply_to_battle;
};

struct FrontOptions;  // client/front.h
// Runs menus and battles until the program quits. Returns a process exit code.
int run_front_end(FrontEndSetup& setup, const FrontOptions& options);

}  // namespace client
