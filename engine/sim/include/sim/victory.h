#pragma once
// Victory, defeat and the end of the game (docs/RE_MEMORY.md, "Victory").
//
// Each machine judges only its own (local) player, in that player's 30-step turn of the
// players' stage, before the economy settlement:
//  - skirmish and network games: defeated when the local player has no live units; else it
//    has won when every other player that is not its ally has none (skirmish) or every
//    remaining player is a mutual "allied victory" ally (network);
//  - campaigns: the mission's conditions (not built; see RE_MEMORY).
// A decision starts the end countdown (5 more turns, 150 steps); while it runs no player's
// economy is settled. When it runs out: a win or loss ends the game (end-game flags), the
// deathmatch mode puts a new commander on the map instead, and a network game may turn the
// loser into a watcher.
// A commander's death in the commander game modes destroys all of its player's units.

#include <array>
#include <cstdint>

namespace sim {

enum class Outcome : std::uint8_t { Playing = 0, Won = 1, Lost = 2 };

namespace end_game_flags {
constexpr std::uint16_t kEnding = 4, kWon = 0x10, kWonMovie = 0x20, kLost = 0x40;
}

// Per player, what the original keeps in the players' side records (network games).
struct VictorySide {
    bool watching = false;       // put in watch mode / joined as a watcher (side option 0x40)
    bool allied_victory = false; // the player accepts an allied victory (side flags bit 1)
    bool wants_watch = false;    // the local player asked to keep watching (side option 0x80)
    bool human = false;          // the side record's controller is a human (watchers of AI hosts)
    std::uint8_t lobby_status = 0;
    std::uint16_t energy_start = 0, metal_start = 0;  // network start resources in hundreds
};

struct VictoryState {
    std::array<Outcome, 10> outcome{};  // by player slot (only the local player is judged)
    bool over = false;                  // the game has ended (the end flags are set)
    std::uint32_t decided_tick = 0;     // step at which the end countdown started (0: not yet)
    std::array<VictorySide, 10> sides{};
    std::uint32_t cd_force_tick = 0;    // copy-protection mismatch: forced defeat step (0: none drawn)
    std::uint32_t respawns = 0;         // deathmatch commanders placed
};

}  // namespace sim
