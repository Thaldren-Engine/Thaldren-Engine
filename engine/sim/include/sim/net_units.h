#pragma once
// Network play inside the battle (docs/RE_MEMORY.md, "Network and sync"): each machine
// simulates only its own players' units; the units of players on other machines
// (controller 3) run their scripts and are driven by received packets. The Battle members
// that build and apply those packets are declared in sim/battle.h ("Network play") and
// implemented in engine/sim/src/net_units.cpp.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sim {

class OrderGoal;
struct MissionOrder;

// Inputs the original takes from fresh heap memory: a remote aircraft's goal received from
// the network does not set its position unless the sender's goal had one (flag 0x20), nor
// its formation offset.
struct NetUnitInputs {
    std::array<std::int32_t, 3> fresh_goal_pos{};
    std::int32_t fresh_goal_offset = 0;
};

// Goals of remote aircraft (owned here: the original's remote flight goal owns them itself).
struct NetUnitState {
    std::vector<std::shared_ptr<OrderGoal>> remote_goals;  // by unit id
    std::vector<const struct WeaponType*> weapons;         // by weapon index (fire packets name weapons by index)
    std::string fault;                                     // where the original would have crashed
    std::uint64_t states_written = 0, states_read = 0, full_states = 0, units_made = 0;
};

// Bits of a unit type index in the state packets (0x42D65B): the bit length of the type count.
int unit_type_bits(std::size_t type_count);

// The sender's encoding of a build fraction (1 + the integer part of fraction x 254; 0 for a
// finished unit) and the receiver's decoding (x 1/255 as a float).
std::uint32_t encode_build_fraction(float fraction_left);
double decode_build_fraction(std::uint32_t byte);

}  // namespace sim
