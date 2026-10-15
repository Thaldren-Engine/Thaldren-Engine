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

#include <cstdint>
#include <functional>
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
};

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
void install_multiplayer(FrontEndHooks& hooks, const NetMenuConfig& config);

// The session of a network game: the inner session runs the battle; this one keeps the
// network clock, polls while the game is paused, sends chat and shows received lines.
class NetBattleSession final : public BattleSession {
public:
    NetBattleSession(std::unique_ptr<net::Transport> transport, std::unique_ptr<net::NetBattle> net, std::unique_ptr<BattleSession> inner);
    sim::Battle& battle() override { return inner_->battle(); }
    Content& content() override { return inner_->content(); }
    GameClient& client() override { return inner_->client(); }
    int frame(std::uint32_t now_ms) override;
    BattleOutcome outcome() override { return inner_->outcome(); }
    void send_chat(const std::string& text) override;
    void surrender() override;
    bool network() const override { return true; }
    std::vector<PlayerStats> stats() override { return inner_->stats(); }
    bool after_battle(FrontEnd& fe, BattleOutcome o) override { return inner_->after_battle(fe, o); }
    net::NetBattle& net() { return *net_; }

private:
    std::unique_ptr<net::Transport> transport_;
    std::unique_ptr<net::NetBattle> net_;
    std::unique_ptr<BattleSession> inner_;
};

}  // namespace client
