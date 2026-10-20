#pragma once
// Multiplayer for the front end (client/front_end.h): the connection screen (host or join by
// address), the battle room (players, side, colour, ready, map, options, chat, start) and the
// battle session of a network game (network clock and pause, in-game chat, the room's game
// description handed to the importer's loader). See docs/RE_MEMORY.md, "Network and sync".
//
// Plugging in: call install_multiplayer(hooks, config) with a NetMenuConfig whose
// load_network_game builds the battle (the importer side: game data, map, sim::start_battle
// with request.start) and gives the battle `request.wrap_outside(client_outside)` as its
// outside, so the network side sees every packet the battle sends.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "client/front_end.h"
#include "net/lobby.h"
#include "net/net_battle.h"
#include "net/transport.h"

namespace client {

// What the importer needs to load a network game.
struct NetGameRequest {
    std::string map;
    std::array<std::string, 10> sides{};  // "ARM" / "CORE" per slot
    sim::BattleStart start;               // provider 3; this machine's players local, the others remote
    // Returns the outside to build the battle with (the network side over the client's own),
    // and after the battle exists and is started: attach(battle).
    std::function<sim::BattleOutside&(sim::BattleOutside& client_outside)> wrap_outside;
    std::function<void(sim::Battle&)> attach;
    // The user's settings (client options of the battle: screen, speeds, interface type).
    const Settings* settings = nullptr;
    // The host's "cheat codes" option (side record option word bit 13): cheat commands allowed.
    bool cheats = false;
    // The unit definitions' rules of the game (0x46E160), by definition key (the FBI file's
    // simple checksum): whether this machine may build it, and the most one player may own (-1
    // no limit). A definition without a rule cannot be built. Empty: no rules (tests).
    std::map<std::uint32_t, std::pair<bool, std::int32_t>> unit_rules;
    // Called by the loader once the game data is loaded, right before the battle starts: runs
    // the loading handshake (start positions, "loaded") and completes the start's players.
    // Empty: the handshake already ran.
    std::function<void(sim::BattleStart& start)> before_start;
};

// The original's chat line as its sender makes it (0x463E50, "<%s%s%s> %s"): "<name> text", or
// "<name->to> text" for a line to some players only (`to`: "Allies", "Enemies" or a player's
// name). The receivers show it as it arrives.
std::string chat_line(const std::string& name, const std::string& to, const std::string& text);

struct NetMenuConfig {
    std::uint16_t port = 47624;  // the original's session port
    std::string player_name = "Player";
    // Builds the session of a network game (importer side); nullptr with `error` on failure.
    std::function<std::unique_ptr<BattleSession>(const NetGameRequest&, const LoadProgress&, std::string& error)> load_network_game;
    // The unit definition checksums of this machine (key, checksum), for the room's comparison.
    std::function<net::UnitChecks()> unit_checks;
};

// The connection and battle room screens (the main menu's multiplayer button).
std::unique_ptr<Screen> make_multiplayer_menu(FrontEnd& fe, const NetMenuConfig& config);
// The battle room over a connected transport: `net_id` is this machine's player in it (from the
// transport's own session for the original's session protocol; 0 picks one), `game_name` the
// host's game name.
std::unique_ptr<Screen> make_room_screen(const NetMenuConfig& config, std::unique_ptr<net::Transport> transport, bool host, std::int32_t net_id,
                                         const std::string& game_name = {});
void install_multiplayer(FrontEndHooks& hooks, const NetMenuConfig& config);

// A battle room handed to the game when the start comes (the host's start, packet 0x08 on the
// others), in the original's order: the loading screen comes at once; while the data loads every
// machine sends its progress (0x2A) and shows the others' bars from theirs; once its data is
// loaded it runs the loading handshake (start positions, "loaded") and the battle starts.
struct NetLaunch {
    std::unique_ptr<net::Transport> transport;
    std::shared_ptr<net::NetSession> session;
    std::shared_ptr<net::BattleRoom> room;      // created with defer_loading_handshake(true)
    std::shared_ptr<sim::Lcg15> crt_rand;       // this machine's C runtime rand() (the host's shuffle)
};
void launch_network_battle(FrontEnd& fe, const NetMenuConfig& config, NetLaunch launch);

// The session of a network game: the inner session runs the battle; this one keeps the
// network clock, polls while the game is paused, sends chat and shows received lines.
class NetBattleSession final : public BattleSession {
public:
    NetBattleSession(std::unique_ptr<net::Transport> transport, std::shared_ptr<net::NetSession> session, std::unique_ptr<net::NetBattle> net,
                     std::unique_ptr<BattleSession> inner);
    sim::Battle& battle() override { return inner_->battle(); }
    Content& content() override { return inner_->content(); }
    GameClient& client() override { return inner_->client(); }
    int frame(std::uint32_t now_ms) override;
    BattleOutcome outcome() override { return inner_->outcome(); }
    void send_chat(const std::string& text) override;
    void surrender() override;
    bool network() const override { return true; }
    bool end_exchange_done(std::uint32_t now_ms) override;
    std::vector<PlayerStats> stats() override { return inner_->stats(); }
    bool end_table(EndTable& t) override { return inner_->end_table(t); }
    std::string battle_name() const override { return inner_->battle_name(); }
    int difficulty() const override { return inner_->difficulty(); }
    bool after_battle(FrontEnd& fe, BattleOutcome o) override { return inner_->after_battle(fe, o); }
    net::NetBattle& net() { return *net_; }
    // The players' names (the session's) and this machine's player: chat lines carry the
    // sender's name, lines to one player that player's name.
    void set_names(const std::array<std::string, 10>& names, int local) {
        names_ = names;
        local_ = local;
    }

private:
    std::array<std::string, 10> names_{};
    int local_ = 0;
    bool exchanged_ = false, exchange_started_ = false;
    std::uint32_t exchange_ms_ = 0;
    std::unique_ptr<net::Transport> transport_;
    std::shared_ptr<net::NetSession> session_;
    std::unique_ptr<net::NetBattle> net_;
    std::unique_ptr<BattleSession> inner_;
};

}  // namespace client
