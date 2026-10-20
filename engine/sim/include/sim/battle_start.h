#pragma once
// Battle start: from a chosen map, players and options to the first step, in the original's
// order (docs/RE_MEMORY.md, "Battle start").
//
// The importer (or the game's own loader) fills the battle's static data first: unit types,
// weapon types, feature types and animations, the map grid and its features, the map's
// economy (metal, wind range, tide), the movement classes, the sight tables, the meteor
// configuration. start_battle() then does what the original's battle initialisation does:
// seeds, options, the load-time wind step, the meteor schedule, the player records (economy,
// sight counts, controllers and unit tables), routes, start positions and commanders,
// starting resources and storage bonuses, the line-of-sight rebuild, one players' stage pass
// at step 0 and the resource reset after it.

#include <array>
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/mission_goals.h"

namespace sim {

class Battle;

// How a player record is driven (the record's controller).
enum class PlayerKind : std::uint8_t {
    Empty = 0,     // slot unused
    Local = 1,     // a human player this simulation runs (controller 1)
    Computer = 2,  // a computer player (controller 2)
    Remote = 3,    // a player whose units another machine simulates (controller 3: scripts only)
};

struct StartPlayer {
    PlayerKind kind = PlayerKind::Empty;
    std::int32_t net_id = 0;              // network identity (any distinct nonzero value offline)
    std::uint16_t commander_type = 0;     // battle unit type of the side's commander (0: none)
    std::string commander_name;           // its unit name (the commander test and deathmatch)
    // Start position id (the OTA's StartPosN gives id N-1). -1: skirmish assignment (fixed =
    // the slot number, else the shuffle). Network games: the id the host assigned.
    std::int32_t start_position = -1;
    std::uint8_t colour = 0;              // team colour index
    std::uint8_t ally_group = 0xFF;       // players with the same group (not 0xFF) are allied
    std::int32_t energy = -1, metal = -1; // skirmish start resources (-1: BattleStart's defaults)
    bool watcher = false;                 // network: joined to watch (no commander)
    bool allied_victory = false;          // network: accepts an allied victory
    bool host = false;                    // network: the side record's host flag (the hosting machine's records)
    std::string name;
};

// One StartPos special of the map's schema, in file order.
struct StartRule {
    std::int32_t id = 0;
    std::int16_t x = 0, z = 0;  // world units
};

// A scripted mission's part of the start (provider 1, docs/RE_MEMORY.md "Campaign").
struct MissionStart {
    std::vector<MissionUnit> units;        // placed after the line-of-sight rebuild
    MissionOrderParser parse_orders;       // the units' order strings (the importer's)
    MissionGoals goals;                    // win and loss goals
    std::array<float, 10> metal{}, energy{};  // per player record: stored amounts and storage bonus
    bool restrict_types = false;           // only `allowed_types` can be made (the mission's unit list)
    std::vector<std::uint16_t> allowed_types;
};

struct BattleStart {
    std::array<StartPlayer, 10> players{};
    // Network games: the records' alliance bytes (+0x108) by slot as the room left them (the
    // team rule, alliances made in the room, forced ones heard). Else from the ally groups.
    std::array<std::array<std::uint8_t, 10>, 10> alliances{};
    bool alliances_given = false;
    std::vector<StartRule> start_rules;  // the selected schema's start positions
    // Legacy form of the start positions (used when start_rules is empty): position k has id k.
    std::array<std::array<std::int32_t, 2>, 10> start_positions{};
    std::int32_t start_position_count = 0;
    bool fixed_locations = false;      // skirmish: player n starts at position n (else shuffled)
    // Options.
    std::int32_t provider = 2;         // 1 campaign, 2 skirmish, 3 network game
    float start_energy = 1000.0f, start_metal = 1000.0f;  // skirmish defaults per player
    std::int32_t commander_mode = 0;   // 0 game continues, 1 commander's death ends its player, 2 deathmatch
    std::uint16_t unit_limit = 250;    // units per player
    bool fog_of_war = false, explore = false, true_sight = false;  // line of sight, mapping, LOS type
    bool target_everything = false;
    std::int32_t command_mode = 0;
    std::int32_t ai_difficulty = 2;
    std::uint8_t local_player = 0;     // the judged player (victory) and the viewed player
    bool net_game = false;
    // Random generators. The original seeds the battle generator from the high-resolution
    // clock; battle loading runs on its own thread whose C runtime rand() is seeded with
    // time(); the simulation thread's rand() is whatever the menus left. All three are inputs
    // (a capture of the original records them).
    std::uint32_t battle_rng_seed = 0;    // the value passed to the seeding routine (before its mix)
    std::uint32_t loading_rand_seed = 0;  // srand(time()) of the loading thread
    std::uint32_t crt_rand_state = 0;     // the simulation thread's rand() state at the first step
    // Runs once every player record is set up, before the commanders and the step-0 players'
    // stage (the importer reads the computer players' profile here).
    std::function<void(Battle&)> after_player_setup;
    MissionStart mission;  // provider 1 only
    // A saved game being continued: the original's start with a saved state skips the start
    // positions, commanders, starting resources and a mission's placed units, and loads the
    // saved state right after the line-of-sight rebuild (then the step-0 players' stage runs
    // at the saved step). Returns false (with a reason) when the state cannot be loaded.
    std::function<bool(Battle&, std::string&)> restore;
};

// Runs the original's battle set-up on a battle whose static data is loaded. Returns false
// (with a reason) when the description is unusable.
bool start_battle(Battle& battle, const BattleStart& start, std::string& error);

// The skirmish start-position shuffle on the loading thread's rand(): `ids` in player order.
// Fewer than three players: one draw decides whether to shuffle at all. The shuffle swaps
// position p with one of the positions before it (never itself).
void shuffle_start_positions(std::vector<std::int32_t>& ids, std::uint32_t& loading_rand_state);

}  // namespace sim
