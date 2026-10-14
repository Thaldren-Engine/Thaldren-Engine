#pragma once
// Simulation world and the fixed per-step stage order.

#include <array>
#include <cstdint>
#include <functional>

#include "sim/rng.h"

namespace sim {

// Stages run once per step, in exactly this order. The order is part of the
// simulation contract: RNG draws and state changes depend on it.
enum class Stage : std::uint8_t {
    NetInput,        // apply remote commands (networked games only)
    Units,           // per-unit scripts, weapons, orders, movement
    Projectiles,
    WorldEffects,    // effects and debris (debris draws from the battle RNG)
    Players,         // AI, line of sight, economy, victory conditions
    Features,        // feature animation, spreading, burning
    AnimCursors,     // sprite animation cursors
    Wind,
    Meteors,
    EffectListeners,
    NetFlush,        // networked games only
    Count,
};

constexpr std::array<Stage, static_cast<std::size_t>(Stage::Count)> kStageOrder = {
    Stage::NetInput, Stage::Units,  Stage::Projectiles, Stage::WorldEffects,    Stage::Players, Stage::Features,
    Stage::AnimCursors, Stage::Wind, Stage::Meteors,   Stage::EffectListeners, Stage::NetFlush,
};

class World {
public:
    using StageFn = std::function<void(World&)>;

    std::uint32_t tick() const { return tick_; }
    BattleRng& battle_rng() { return battle_rng_; }
    Lcg15& rand() { return rand_; }

    void set_networked(bool v) { networked_ = v; }
    bool networked() const { return networked_; }

    // Install the implementation of one stage (empty stages are skipped).
    void set_stage(Stage s, StageFn fn) { stages_[static_cast<std::size_t>(s)] = std::move(fn); }

    // Advance one simulation step: increment the tick, then run every stage in order.
    void step();

    // Work that runs once per frame after that frame's steps, even when the
    // frame ran zero steps (deferred line-of-sight expiry).
    void set_frame_end(StageFn fn) { frame_end_ = std::move(fn); }

    // Run one frame: `count` steps (the frame's budget), then the frame-end work.
    void run_frame(int count) {
        for (int i = 0; i < count; ++i) step();
        if (frame_end_) frame_end_(*this);
    }

private:
    std::uint32_t tick_ = 0;
    bool networked_ = false;
    BattleRng battle_rng_;
    Lcg15 rand_;
    std::array<StageFn, static_cast<std::size_t>(Stage::Count)> stages_{};
    StageFn frame_end_;
};

} // namespace sim
