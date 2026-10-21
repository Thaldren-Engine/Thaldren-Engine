#include "sim/step_budget.h"

#include <algorithm>
#include <cmath>

#include "sim/x87.h"

namespace sim {

int StepBudget::update(std::uint32_t now_ms, std::int32_t remote_lag_ticks) {
    const std::uint32_t now = scale_ms(now_ms);
    const auto elapsed = static_cast<std::int32_t>(now - last_scaled_tick);
    last_scaled_tick = now;

    speed_reduced = effective_speed < game_speed;

    double rate = static_cast<double>(effective_speed) * 0.1;
    if (remote_lag_ticks >= 900) {
        const std::int32_t lag = std::min<std::int32_t>(remote_lag_ticks, 3600);
        double throttle = static_cast<double>(3600 - lag) * 0.00037037037037037035;
        if (x87::less(throttle, 0.01)) throttle = 0.01;  // 0x495347 (test ah,1)
        rate = throttle * rate;
        lag_throttled = true;
    } else {
        lag_throttled = false;
    }

    // Every intermediate is rounded to double (53-bit precision), matching x87
    // in 53-bit mode; the sum is stored to a double before floor().
    const double wanted = static_cast<double>(elapsed) * rate + static_cast<double>(step_fraction);
    const double whole = std::floor(wanted);
    int steps = x87::ftol32(whole);
    step_fraction = static_cast<float>(wanted - whole);

    if (steps < 0) steps = 0;
    if (paused) return 0;

    if (steps > kMaxStepsPerFrame) {
        // Overloaded: drop the excess (it is not carried) and slow down.
        steps = kMaxStepsPerFrame;
        if (++hysteresis > 10) {
            hysteresis = 0;
            if (effective_speed > 1) --effective_speed;
        }
    } else if (--hysteresis < -100) {
        hysteresis = 0;
        if (effective_speed < game_speed) ++effective_speed;
    }
    return steps;
}

} // namespace sim
