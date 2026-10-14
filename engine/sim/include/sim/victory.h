#pragma once
// Victory, defeat and the end of the game (docs/RE_MEMORY.md, "Victory").
//
// Each player's turn of the players' stage (every 30 steps) first runs the local player's
// victory and defeat checks; a commander's death ends its player in the commander game mode;
// a player without units is out. When the game is decided, the end countdown runs and the
// battle ends.

#include <array>
#include <cstdint>

namespace sim {

enum class Outcome : std::uint8_t { Playing = 0, Won = 1, Lost = 2 };

struct VictoryState {
    std::array<Outcome, 10> outcome{};  // by player slot
    bool over = false;                  // the game has ended (the end countdown ran out)
    std::uint32_t decided_tick = 0;     // step at which the outcome was decided (0: not yet)
};

}  // namespace sim
