#pragma once
// The client's commands to the simulation. Adapter: until the game-flow command
// layer lands, commands call sim::Battle's order functions directly; this header
// and commands.cpp are the only place that does (see docs/RE_MEMORY.md, "Client").

#include <array>
#include <cstdint>
#include <optional>

#include "sim/battle.h"

namespace client {

using WorldPos = std::array<std::int32_t, 3>;

// A player's command to every selected unit of player `player` (unit flag kSelected),
// except `target` itself: `intent` (sim::intent values; kDefault for the right click)
// resolves per unit into a mission kind; units grouped around their centre keep their
// offsets when the mission is positional. `queued` (shift) appends.
// Returns the number of units that took an order.
int command_selected(sim::Battle& b, int player, std::uint8_t intent, sim::Unit* target, const WorldPos* pos, bool queued);

// The same with a fixed mission kind (standing orders, switches, stop) and parameter.
int command_selected_kind(sim::Battle& b, int player, std::uint8_t kind, std::int32_t param, bool queued);

// Ctrl+D (0x495E90 case 0xAD): every selected unit of the player (its unit range) loses its
// self-destruct order (the list the kind's default flags name, the first of that kind); when
// none had one, the selected units are ordered to self-destruct.
void toggle_self_destruct(sim::Battle& b, int player);

// Factory build queue: `delta` more (or fewer) units of battle type `type`.
void change_build_count(sim::Battle& b, sim::Unit& factory, std::uint16_t type, std::int32_t delta);
// Queued count of `type` in the unit's build orders.
std::int32_t build_count(const sim::Unit& factory, std::uint16_t type);

// Mobile builders among the selected units build `type` at `pos` (snapped to the footprint).
int command_build(sim::Battle& b, int player, std::uint16_t type, const WorldPos& pos, bool queued);

// Whether a structure of `type` fits with its footprint's first cell at (cell_x, cell_z).
bool structure_fits(sim::Battle& b, std::uint16_t type, int cell_x, int cell_z);

// One simulation step in the original's stage order (Battle::step).
void step_battle(sim::Battle& b);
// After a frame's steps (even none): the per-frame work (lingering sight expiry).
void end_frame(sim::Battle& b);

} // namespace client
