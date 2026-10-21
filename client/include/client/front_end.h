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

#include <array>
#include <cstdint>
#include <cstdio>
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
    // Open alone over a battle, it leaves the keys to the battle's own key handling (a bar that
    // takes the pointer only); Tab still reaches it.
    virtual bool battle_keys() const { return false; }
    // The network games' tab bar (Tab closes it).
    virtual bool tab_bar() const { return false; }
    // As the top screen it shows no pointer (movies).
    virtual bool hides_pointer() const { return false; }
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

// The end-of-battle statistics as the statistics screen shows them (spec: docs/RE_MEMORY.md,
// "Client", "End of a battle"). Inputs: each player slot's record at the end.
struct EndPlayer {
    bool active = false;          // the slot is in use
    int controller = 0;           // 1 local, 2 computer / simulated here, 3 remote
    int state = 0;                // 10: out of the game
    std::uint32_t units_created = 0;
    bool present = true;          // still in the (network) game
    bool watching = false;        // a watcher (side record option bit 6)
    int colour = 0;               // its logo colour
    std::int16_t kills = 0, losses = 0;
    double energy_produced = 0, metal_produced = 0, energy_wasted = 0, metal_wasted = 0;
    std::string name;
};
struct EndRow {
    int slot = 0;
    std::string name;  // (at most 29 characters)
    int colour = 0;
    // Kills, losses, energy and metal produced, excess energy and metal, score.
    std::array<std::int32_t, 7> values{};
};
struct EndTable {
    std::vector<EndRow> rows;              // in slot order
    std::array<std::int32_t, 7> maxima{};  // each column's largest value (at least 10, 10, 100, ...)
    bool won = false, watching = false;    // the local player's outcome (the title)
};
// The table from the players' records, the step the battle ended at and the map's score
// factors (kill and time multipliers): a slot shows when it is present and either playing
// (in use, controller 1-3, not out, not watching) or it ever made a unit; amounts are
// truncated; score = trunc((step / 60) x time factor) + trunc(kills x kill factor), at least 0.
EndTable end_table(const std::array<EndPlayer, 10>& players, std::uint32_t end_step, float kill_mul, float time_mul);

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
    // Network games' chat recipients (GS+0x2BF0: 0 everybody, 1 allies, 2 enemies, 3 the chosen
    // players of GS+0x2BF1) and TALK2's switch (GS+0x2BEE bit 0x100: the recipient panel shown
    // with the chat line; cleared when a line is sent).
    int chat_mode = 0;
    std::array<bool, 10> chat_chosen{};
    bool chat_panel = false;
    // The local player gives up (in-game menu).
    virtual void surrender() {}
    // The program is about to end with this battle running (exit to Windows): network games
    // take this machine's players out of the game (0x451B60).
    virtual void leave_game() {}
    // A network game (Tab opens the tab bar instead of the game menu).
    virtual bool network() const { return false; }
    // Once the battle has ended: true when the end screens may show (network games first
    // exchange every player's final totals, 0x4572A0).
    virtual bool end_exchange_done(std::uint32_t /*now_ms*/) { return true; }
    // Totals for the statistics screen.
    virtual std::vector<PlayerStats> stats() { return {}; }
    // The statistics screen's table (false: the session has none).
    virtual bool end_table(EndTable&) { return false; }
    // The same battle from its start again (the exit menu's Restart) with the computer
    // players' difficulty given; an empty launcher when the session cannot restart.
    virtual std::function<std::unique_ptr<BattleSession>(const std::function<void(float, const std::string&)>&, std::string&)> restart_launcher(int /*difficulty*/) {
        return {};
    }
    // The computer players' difficulty the battle was started with (0 easy .. 2 hard).
    virtual int difficulty() const { return 1; }
    // The battle's name as its map describes it (the restart dialog shows it).
    virtual std::string battle_name() const { return {}; }
    // Called once the battle and its end screens are over; may push the next
    // screen (campaign debriefing / next mission). Return false to go back to
    // the screen the battle was started from (the default).
    virtual bool after_battle(FrontEnd&, BattleOutcome) { return false; }
    // Network games' in-battle screens (allies, share): the local player's alliance with another
    // player (packet 0x23), the other's answer (the local record's "allied by" byte), the explored
    // map sent to a player (0x16 kind 3) and the players' names.
    virtual void set_alliance(int /*from*/, int /*to*/, bool /*allied*/) {}
    virtual std::uint8_t allied_by(int /*local*/, int /*other*/) const { return 0; }
    virtual void send_explored_map(int /*from*/, int /*to*/) {}
    virtual std::string player_name(int /*slot*/) const { return {}; }
    // A player's ally team from the battle room (0..4, 5 none) and whether it is a computer player
    // (its side record's kind 2).
    virtual int ally_team(int /*slot*/) const { return 5; }
    virtual bool computer(int /*slot*/) const { return false; }
    // The local player's acceptance of an allied victory (side record flags bit 1).
    virtual void set_allied_victory(int slot, bool on);
    // Network games' time-out (0x453C20): the remote player nobody has heard from for the time-out
    // (-1: none), the seconds left before it is sent away (0x453640: time-out + 120 less the seconds
    // since it was last heard) and sending it away (status 6 "lost").
    virtual int timed_out_player() const { return -1; }
    virtual int timeout_seconds_left(int /*slot*/) const { return 0; }
    virtual void reject_player(int /*slot*/, int /*reason*/) {}
    // The local player's leaving byte (record +0x22: why it was sent away, 0 none; network games).
    virtual int local_leaving_reason() const { return 0; }
    // The host's control screen: whether this machine created the game (0x457A50: the first
    // record with the creator flag is one of this machine's players) and the local side record's
    // option word (+0x9B; bit 0x80 the watching switch), changed and announced.
    virtual bool host() const { return false; }
    virtual std::uint16_t local_options() const { return 0; }
    // The host's side record option word (the first record with the host bit, 0x456850).
    virtual std::uint16_t host_options() const { return 0; }
    virtual void set_local_options(std::uint16_t) {}
    // Set by the front end: the session reports combat involving the local player here (+1 a
    // hit, +5 a kill it scores; the music's battle/peace switch).
    std::function<void(int)> combat_activity;
};

// Progress of a battle being loaded: fraction 0..1 and the stage's name. The loading screen
// has six rows filled one after the other (kLoadRowNames, in their order); a stage named
// "row:K" sets row K to the fraction (whole percent, rounded down). "map:NAME" gives the map
// line its name, "map:" alone leaves the screen without a map line (missions). Other stage
// names only redraw. The front end redraws at most every 200 ms (kLoadRedrawMs), as often as the
// original's loading screen does.
using LoadProgress = std::function<void(float fraction, const std::string& stage)>;
constexpr int kLoadRows = 6;
constexpr std::uint32_t kLoadRedrawMs = 200;
inline std::string load_row_stage(int row) { return "row:" + std::to_string(row); }

// A player of a network game on the loading screen (the bars along its bottom).
struct LoadingPlayer {
    std::string name;
    int percent = 0;      // its loading as its packets report it (local players: this machine's)
    bool local = false;   // played on this machine (its bar shows this machine's average)
    bool loaded = false;  // said it has loaded (counts as ready once at 100)
};

// What the loading screen shows. The front end owns it while a battle loads; the drawing
// keeps its own per-row state in `flash` / `seen` / `flash_tick` (reset with each load).
struct LoadingState {
    std::array<int, kLoadRows> rows{};  // percent per row
    bool map_line = true;               // the map line is shown (not for missions)
    std::string map;                    // the map's name on it
    bool network = false;               // a network game: the players' bars and the status line
    bool synchronised = false;          // every machine has loaded (the status line says so)
    std::vector<LoadingPlayer> players;  // the counted players in slot order
    // The drawing's state: a row that reaches 100 % lights its label up (flash 30, two less at
    // each redraw on a later clock tick); `seen` is each row's value at the last redraw.
    std::array<int, kLoadRows> flash{}, seen{};
    std::uint32_t flash_tick = 0;
    std::uint32_t now_ms = 0;  // the clock at this redraw
};
// Builds a session (game data, map, battle start); nullptr with `error` on failure.
using BattleLauncher = std::function<std::unique_ptr<BattleSession>(const LoadProgress& progress, std::string& error)>;

// ---------------------------------------------------------------- hooks
// Entry points other parts fill in. An empty hook leaves its button disabled.
struct FrontEndHooks {
    // Single-player menu > campaign: the campaign's first screen.
    std::function<std::unique_ptr<Screen>(FrontEnd&)> campaign_menu;
    // Single-player menu > any mission (shown with the all-missions setting): the same screen
    // with the mission list.
    std::function<std::unique_ptr<Screen>(FrontEnd&)> any_mission_menu;
    // Main menu > multiplayer: the connection / lobby screens.
    std::function<std::unique_ptr<Screen>(FrontEnd&)> multiplayer_menu;
    // A start-up switch asked for the multiplayer screens at once (-n; GS+0x39245): the main menu
    // opens them first (and the start's 1.zrb is not shown).
    std::function<bool()> network_start;
    // Single-player menu > load game, and the in-game menu's load button
    // (session = the running battle, or nullptr from the menus).
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession* session)> load_game_menu;
    // In-game menu > save game.
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession& session)> save_game_menu;
    // The game menu's Mission button in a campaign: the mission's briefing pages (BRIEFING.GUI).
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession& session)> mission_briefing;
    // Network games: the chat line's screen (TALK.GUI with its SENDTO switch, or TALK2.GUI with
    // the recipient panel), opened when the battle's chat line opens.
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession& session)> chat_panel;
    // "+save <name>" (0x417430): the battle written as savegame\<name>.sav in the game folder
    // (description "Generic Game Description", game id 666).
    std::function<void(BattleSession& session, const std::string& name)> save_game_command;
    // Network games: Tab opens the tab bar (options, allies, share, control); its buttons
    // other than the options use these.
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession& session)> allies_menu;
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession& session)> share_menu;
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession& session)> control_menu;
    // F1 in a battle: the unit information screen for a unit type (0x4942E0).
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession& session, std::uint16_t type)> unit_info;
    // A network battle's time-out dialog (TIMEOUT.GUI, 0x453A50): opened while a remote player is
    // silent past the time-out; it closes itself when nobody is.
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession& session)> timeout_dialog;
    // A network battle's local player put in watch mode (0x464F80): kind 1 the question "You're
    // out!  Continue Watching?" (YESORNO.GUI), 2 the box that its computer players keep it there.
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession& session, int kind)> watch_dialog;
    // The unit definitions enumerated (0x42BD10 at the top of each front-end pass, 0x496BB0: at
    // the first pass and at the first after a battle's data was loaded); the hook may show the
    // incompatible units message (system_message).
    std::function<void(FrontEnd&)> enumerate_units;
    // Opens a web address with the system's handler (program false) or starts a program, its
    // path as the command line (program true); returns whether it started. Unset: nothing is
    // opened (a failure). Tests capture it instead of launching anything.
    std::function<bool(const std::string& target, bool program)> open_external;
};

// ---------------------------------------------------------------- front end
class FrontEnd {
public:
    virtual ~FrontEnd() = default;
    // Screen stack.
    virtual void push(std::unique_ptr<Screen> screen) = 0;
    virtual void pop() = 0;
    // The number of screens on the stack (0: not known).
    virtual std::size_t depth() const { return 0; }
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
    // Leaves the running battle without its end screens and starts `launcher` in its place
    // (the exit menu's Restart); the screen the battle was started from stays below.
    virtual void restart_battle(BattleLauncher launcher) { start_battle(std::move(launcher)); }
    // Ends the program.
    virtual void quit() = 0;
    // Exit to Windows as the original does it (the main menu's EXIT, 0x425D80 state 8; the exit
    // menu's "Exit Game" answered Yes, 0x4605C0 -> 0x491C60): in a battle the music goes silent
    // and the session leaves the game (network games: the local players' units die and the
    // players leave the session, 0x451B60); then one black frame (colour 0, no pointer) is shown
    // and the program ends.
    virtual void exit_to_windows() { quit(); }

    virtual Settings& settings() = 0;
    // The session's unit limit (GS+0x37EEC): set at start-up from totala.ini (0x491200, the
    // setup's unit_limit), replaced by a loaded save's maxunits (0x432610); skirmish starts, save
    // loads and the network room's host slider read it.
    int& unit_limit() { return unit_limit_; }
    virtual FrontEndHooks& hooks() = 0;
    // Menu art and sounds (the importer's), the frame size, the menu palette.
    virtual const MenuContent& menu_content() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    // Plays an interface sound (index into menu_content().samples): centred, 5.85 dB down,
    // nothing while the effect volume or the sound mode is off.
    virtual void play_sound(int sample) = 0;
    // The same sound looping until every sound stops (only one looping sound at a time).
    virtual void play_sound_loop(int sample) { play_sound(sample); }
    // A sound file with a buffer of its own (the sound options' test).
    virtual void play_sound_file(int sample) { play_sound(sample); }
    // Narration: its own channel at full volume, whatever the sound settings; a new one
    // replaces the last; stop_narration() ends it.
    virtual void play_narration(int sample) { play_sound(sample); }
    // The same channel for a sound the caller keeps (it must live until stop_narration()).
    virtual void play_narration_sample(const Sample* /*sample*/) {}
    virtual void stop_narration() {}
    // Every playing sound stops (the sound mode switched off).
    virtual void stop_sounds() {}
    // The music player (null when there is no music).
    virtual MusicPlayer* music() = 0;
    // Applies the settings (volumes, speeds, interface options) after an options screen.
    virtual void apply_settings() = 0;
    // Milliseconds since the program started (the main loop's clock).
    virtual std::uint32_t now_ms() const = 0;
    // The desktop's size in pixels (the screen-size choices depend on it).
    virtual void desktop_size(int& w, int& h) const = 0;
    // Whether the program's window has the keyboard focus (GetFocus() == the window; headless:
    // always).
    virtual bool window_focused() const { return true; }
    // The display's modes (width, height), when the platform lists them (else none).
    virtual void display_modes(std::vector<std::array<int, 2>>& out) const { out.clear(); }
    // The loading screen's state while a battle loads (null otherwise): a network game's
    // launcher fills in its players and their progress.
    virtual LoadingState* loading() { return nullptr; }
    // The system's own message box (0x4B6B80: MessageBoxA with no owner, MB_OK | MB_TOPMOST):
    // shown at once and waited for (headless: printed).
    virtual void system_message(const std::string& title, const std::string& text) { std::printf("system message: %s: %s\n", title.c_str(), text.c_str()); }

protected:
    int unit_limit_ = 250;
};

// ---------------------------------------------------------------- running it
// What the importer gives the front end (the game's screens are built there).
struct FrontEndSetup {
    MenuContent* content = nullptr;
    int width = 640, height = 480;
    std::string settings_path;           // where the settings live (loaded at start, saved at exit)
    int unit_limit = 250;                // the session's unit limit at start-up (FrontEnd::unit_limit)
    FrontEndHooks hooks;
    std::function<std::unique_ptr<Screen>(FrontEnd&)> main_menu;
    // Draws the loading screen (on a frame of the menus' size).
    std::function<void(FrontEnd&, render::Image8&, LoadingState&)> draw_loading;
    // The screens after a battle (outcome and statistics); popping below them ends the battle.
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession&, BattleOutcome)> battle_over;
    // The in-game menu (F2, and Tab outside network games).
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession&)> game_menu;
    // The tab bar (Tab in network games).
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession&)> tab_menu;
    // Closing the window while a battle runs: the question shown instead of quitting (null:
    // quit at once).
    std::function<std::unique_ptr<Screen>(FrontEnd&, BattleSession&)> close_prompt;
    MusicPlayer* music = nullptr;
    // Applies the settings to a running battle's client (game speed, interface type, ...).
    std::function<void(FrontEnd&, BattleSession&)> apply_to_battle;
    // The screenshot key (Ctrl+F9, in menus and battles): the frame shown last goes to
    // <shot_dir>/<shot_prefix>NNNN.pcx (front.h, save_numbered_picture).
    std::string shot_dir, shot_prefix;
};

struct FrontOptions;  // client/front.h
// Runs menus and battles until the program quits. Returns a process exit code.
int run_front_end(FrontEndSetup& setup, const FrontOptions& options);
// Tests: for each key of `keys` (comma list, or "all": a..z, 0..9, enter, esc, tab, space, bksp
// and the arrows) and each modifier set of `mods` (comma list of none, shift, ctrl, alt joined
// by '+'), a fresh front end opens its first screens (two frames and a drawing), gets the key as
// the window front end delivers it (key down, the typed character's Text event, key up) and runs
// one frame; prints "OUTCOME <layout> <key> <mods> [<codes>] <what happened>" (the format of
// gui_harness's sweep: the gadgets pressed, a screen change, focus / field / text / stage changes).
// click_x >= 0: a left click there (and a frame) before each key; `typed`: characters typed
// (key down, Text, key up, a frame each) after it.
int run_key_sweep(FrontEndSetup& setup, const std::string& keys, const std::string& mods, int click_x = -1, int click_y = -1,
                  const std::string& typed = std::string());

}  // namespace client
