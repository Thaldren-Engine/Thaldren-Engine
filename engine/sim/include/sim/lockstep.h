#pragma once
// Lockstep play, command recording and state checksums (docs/RE_MEMORY.md, "Network and
// sync").
//
// The engine's network model: every machine runs the whole simulation and only commands
// travel. A turn's commands from every player are applied, in player order, before the step
// they are due at; a step runs only when every player's commands for it have arrived. A
// checksum of the simulation state is exchanged so a divergence is caught at the step it
// happens. (The original's own model is different: each machine simulates its own units and
// sends their states; see RE_MEMORY.)

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "sim/commands.h"

namespace sim {

class Battle;

// A checksum over the simulation state that matters for play (units, orders, projectiles,
// players' resources, features, random generators, tick). Equal battles give equal sums.
std::uint32_t state_checksum(Battle& battle);

// A recorded game: the commands, the step each was applied before, and optional checksums.
struct CommandLog {
    std::vector<command::Timed> commands;      // in application order
    std::map<std::uint32_t, std::uint32_t> checksums;  // tick -> state_checksum after that step
    void encode(std::vector<std::uint8_t>& out) const;
    bool decode(const std::vector<std::uint8_t>& in);
};

// Applies the log's commands that are due before the battle's next step, then runs the step;
// records/compares checksums every `check_every` steps (0: never). Returns false at the first
// checksum mismatch (the step's tick in `mismatch_tick`).
class Replayer {
public:
    explicit Replayer(const CommandLog& log) : log_(log) {}
    bool step(Battle& battle, std::uint32_t check_every = 0);
    std::uint32_t mismatch_tick = 0;

private:
    const CommandLog& log_;
    std::size_t next_ = 0;
};

// Lockstep exchange between machines. Commands a player gives at step t are due at step
// t + delay. Each turn message carries one player's commands for one tick (possibly none)
// and that player's checksum of the tick before.
struct TurnMessage {
    std::uint8_t player = 0;
    std::uint32_t tick = 0;  // the step the commands are due before
    std::uint32_t checksum_tick = 0, checksum = 0;  // state_checksum after step checksum_tick (0/0: none)
    std::vector<command::Command> commands;
    void encode(std::vector<std::uint8_t>& out) const;
    bool decode(const std::vector<std::uint8_t>& in);
};

class Lockstep {
public:
    // `players`: bit p set = player slot p takes part; `local`: this machine's slot.
    Lockstep(std::uint16_t players, std::uint8_t local, std::uint32_t delay = 2);
    // The local player's command for the next turn this machine sends.
    void queue_local(const command::Command& c);
    // Builds the local turn message for the step `battle` will run `delay` steps from now
    // (call once per step before try_step); `send` gets the encoded message.
    void send_turn(Battle& battle, const std::function<void(const std::vector<std::uint8_t>&)>& send);
    // A turn message from another machine.
    bool receive(const std::vector<std::uint8_t>& message);
    // Runs the next step if every player's commands for it are here. False: waiting.
    bool try_step(Battle& battle);
    // A checksum differed (the tick, else 0).
    std::uint32_t desync_tick() const { return desync_tick_; }
    // Every command applied, in order (for a recording).
    CommandLog log;

private:
    std::uint16_t players_;
    std::uint8_t local_;
    std::uint32_t delay_;
    std::vector<command::Command> pending_local_;
    std::map<std::uint32_t, std::map<std::uint8_t, TurnMessage>> turns_;  // tick -> player -> message
    std::map<std::uint32_t, std::uint32_t> own_checksums_;
    std::uint32_t sent_upto_ = 0;
    std::uint32_t desync_tick_ = 0;
};

}  // namespace sim
