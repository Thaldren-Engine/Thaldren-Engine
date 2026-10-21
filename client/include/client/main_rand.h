#pragma once
// The program's main-thread C runtime rand() (spec: docs/RE_MEMORY.md, "Numeric model", "RNGs"
// and "One main-thread rand() stream"). The C runtime keeps rand()'s state per thread (slot 5 of
// the thread's data, 0x4E4870); the original seeds the main thread's once at start-up
// (srand(time(NULL)) in WinMain, 0x49E8BB) and every main-thread consumer draws from that one
// state: the menus (the main menu's sparks 0x425B80, the briefing's wind 0x478E80 / 0x478790),
// the network room (the host's start shuffle 0x4568C0, the debug send loss 0x44FCC9), the CD
// music (the pick 0x4CDB40), and in a battle the simulation (wind, meteors, features, effects,
// out-of-units lines ...), the speech queue (0x47FD70) and the lightning drawing (0x49BE60).
// A battle's loading thread has its own state (srand at 0x4971AE) and so has the music
// library's thread (client/track_file_device.h); neither is this one.
//
// The battle keeps its stream as sim::Battle::crt_rand (replays and tools run it alone). While
// a battle runs, this stream IS the battle's: enter() hands it the main state, leave() takes the
// state back, so every consumer draws in the original's order from one sequence.

#include <cstdint>

#include "sim/rng.h"

namespace client {

class MainRand {
public:
    // srand(seed) (the original: time(NULL) at start-up).
    explicit MainRand(std::uint32_t seed = 1) { own_.seed(seed); }
    MainRand(const MainRand&) = delete;
    MainRand& operator=(const MainRand&) = delete;

    // The stream the main thread draws from now: the running battle's, else the program's own.
    sim::Lcg15& stream() { return battle_ ? *battle_ : own_; }
    std::uint32_t next() { return stream().next(); }
    std::uint32_t state() const { return battle_ ? battle_->state() : own_.state(); }

    // A battle's first step comes: its stream continues the main thread's (the loading thread's
    // draws are left behind).
    void enter(sim::Lcg15& battle) {
        if (battle_ == &battle) return;
        if (battle_) leave();
        battle.set_state(own_.state());
        battle_ = &battle;
    }
    // The battle is gone: the main thread goes on from where the battle's stream is.
    void leave() {
        if (!battle_) return;
        own_.set_state(battle_->state());
        battle_ = nullptr;
    }
    bool in_battle() const { return battle_ != nullptr; }
    // The program's own stream (outside battles the one drawn from).
    sim::Lcg15& own() { return own_; }

private:
    sim::Lcg15 own_;
    sim::Lcg15* battle_ = nullptr;
};

}  // namespace client
