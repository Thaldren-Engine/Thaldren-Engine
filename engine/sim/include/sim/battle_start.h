#pragma once
// Battle start: from a chosen map, players and options to the first step, in the original's
// order (docs/RE_MEMORY.md, "Battle start").
//
// The importer (or the game's own loader) fills the battle's static data first: unit types,
// weapon types, feature types and animations, the map grid and its features, the movement
// classes, the sight tables, the map's economy (metal, wind, tide). start_battle() then does
// what the original's battle initialisation does with players and options: player records,
// alliances, unit table ranges, resources and the storage bonus, the random generators'
// seeds, line of sight, routes, the commanders at their start positions, the players' first
// stage pass and the rest of the set-up.

#include <array>
#include <cstdint>
#include <string>

namespace sim {

class Battle;

// How a player record is driven.
enum class PlayerKind : std::uint8_t {
    Empty = 0,     // slot unused
    Local = 1,     // this machine's human player
    Computer = 2,  // a computer player (simulated on the hosting machine)
    Remote = 3,    // another machine's player (its units arrive as states)
};

struct StartPlayer {
    PlayerKind kind = PlayerKind::Empty;
    std::int32_t net_id = 0;              // network identity (any distinct value offline)
    std::uint16_t commander_type = 0;     // battle unit type of the side's commander (0: none)
    std::int32_t start_position = -1;     // index into BattleStart::start_positions (-1: random pick)
    std::uint8_t colour = 0;              // team colour index
    std::uint8_t ally_group = 0xFF;       // players with the same group (not 0xFF) are allied
    std::string name;
};

struct BattleStart {
    std::array<StartPlayer, 10> players{};
    // The map's start positions (world cells x16, from the map info's schema), up to 10.
    std::array<std::array<std::int32_t, 2>, 10> start_positions{};
    std::int32_t start_position_count = 0;
    // Options.
    float start_energy = 1000.0f, start_metal = 1000.0f;
    std::int32_t commander_mode = 0;  // 0: game continues after the commander dies, 1: the player is out
    std::uint16_t unit_limit = 250;   // units per player
    bool fog_of_war = false, explore = false, true_sight = false;
    bool target_everything = false;
    std::int32_t command_mode = 0;
    std::int32_t ai_difficulty = 2;
    std::uint8_t local_player = 0;    // this machine's player slot (and the viewed player)
    bool net_game = false;
    // Random generators. The original seeds the battle generator from the high-resolution
    // clock and the C runtime rand() of the simulation thread is whatever the menus left;
    // both are inputs here (a capture of the original records them).
    std::uint32_t battle_rng_seed = 0;  // the value passed to the seeding routine (before its mix)
    std::uint32_t crt_rand_state = 0;   // the simulation thread's rand() state at the first step
};

// Runs the original's battle set-up on a battle whose static data is loaded. Returns false
// (with a reason) when the description is unusable.
bool start_battle(Battle& battle, const BattleStart& start, std::string& error);

}  // namespace sim
