#pragma once
// Converts elapsed wall-clock time into a number of simulation steps for one
// frame, with automatic game-speed back-off under load.

#include <cstdint>

namespace sim {

constexpr std::uint32_t kTickRateHz = 30;
constexpr int kMaxStepsPerFrame = 5;

struct StepBudget {
    // Persistent state.
    std::uint32_t last_scaled_tick = 0;  // wall clock in 1/kTickRateHz units
    float step_fraction = 0.0f;          // carried sub-step remainder
    std::uint16_t game_speed = 10;       // requested speed; 10 = normal
    std::uint16_t effective_speed = 10;  // speed actually used
    std::int16_t hysteresis = 0;
    bool paused = false;

    // Outputs of the last update (for display).
    bool lag_throttled = false;
    bool speed_reduced = false;

    // Wall-clock milliseconds -> scaled ticks, as 32-bit unsigned arithmetic.
    static std::uint32_t scale_ms(std::uint32_t ms) { return ms * kTickRateHz / 1000u; }

    // remote_lag_ticks: how far the slowest remote-controlled player is behind
    // (0 when there is none). Returns the number of steps to run this frame.
    int update(std::uint32_t now_ms, std::int32_t remote_lag_ticks = 0);
};

} // namespace sim
