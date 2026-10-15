#pragma once
// The input journal of a battle: everything a running battle takes from outside after its
// start, in order, so that the same start plus the journal gives the same battle step for
// step (docs/RE_MEMORY.md, "Saved games").
//
// The simulation is deterministic given its start and these inputs:
//  - commands, each due before a given step (sim/commands.h);
//  - the frame structure: the frame tail (Battle::end_frame) runs once per drawn frame after
//    that frame's steps and changes state (lingering sight expiry, draw-time emitters);
//  - the real-time clock the hover bob reads (BattleOutside::clock_ticks).
// A saved game is the start description plus the journal up to the save. Loading rebuilds the
// battle from the start and replays the journal; the result is the exact state of the moment
// of the save (both random generators, unit scripts with their threads and locals, orders,
// motion, route search, computer players, features, effects, everything), checked against the
// checksum recorded at the save.

#include <cstdint>
#include <map>
#include <vector>

#include "sim/commands.h"

namespace sim {

class Battle;

struct Journal {
    std::vector<command::Timed> commands;        // in application order; tick = the step they precede
    std::vector<std::uint32_t> command_frames;    // per command: frame tails run before it
    std::vector<std::uint32_t> frame_ends;        // the battle's tick at each frame tail, in order
    std::vector<std::uint32_t> clock;             // clock values read, in order
    std::map<std::uint32_t, std::uint32_t> checksums;  // tick -> state checksum after that step (optional)
    void encode(std::vector<std::uint8_t>& out) const;
    bool decode(const std::vector<std::uint8_t>& in);
    // The part of the journal up to the end of step `tick` (a save made then).
    Journal until(std::uint32_t tick) const;
};

// Runs a battle while writing its journal: use these instead of command::apply, Battle::step,
// Battle::end_frame, and pass the clock through clock().
class JournalRecorder {
public:
    explicit JournalRecorder(Journal& j) : j_(j) {}
    bool apply(Battle& battle, const command::Command& c);
    void step(Battle& battle, bool record_checksum = false);
    void end_frame(Battle& battle);
    std::uint32_t clock(std::uint32_t live_value) {
        j_.clock.push_back(live_value);
        return live_value;
    }

private:
    Journal& j_;
};

// Replays a journal onto a battle made from the same start.
class JournalPlayer {
public:
    explicit JournalPlayer(const Journal& j) : j_(j) {}
    // Runs until the battle has done step `tick` and every frame tail recorded at it (tick 0:
    // only the frame tails before the first step). False when a recorded checksum differs
    // (the tick in mismatch_tick) or the journal ends first.
    bool run_to(Battle& battle, std::uint32_t tick);
    // The next recorded clock value (0 when the journal has no more).
    std::uint32_t clock();
    // After run_to at the save's tick: the commands given after the save's last step (due at
    // the next one), applied now as they were then.
    void apply_rest(Battle& battle);
    std::uint32_t mismatch_tick = 0;
    bool finished() const { return next_cmd_ >= j_.commands.size() && next_frame_ >= j_.frame_ends.size(); }

private:
    const Journal& j_;
    std::size_t next_cmd_ = 0, next_frame_ = 0, next_clock_ = 0;
};

}  // namespace sim
