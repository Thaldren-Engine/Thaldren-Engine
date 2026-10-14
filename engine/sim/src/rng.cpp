#include "sim/rng.h"

namespace sim {

std::uint32_t BattleRng::next_mod(std::uint32_t n) {
    if (static_cast<std::int32_t>(n) < 2) {
        return 0;
    }
    const std::uint32_t s = state_;
    const std::uint32_t q = s / 127773u;
    std::uint32_t next = s * 16807u - q * 0x7FFFFFFFu;
    if (static_cast<std::int32_t>(next) <= 0) {
        next += 0x7FFFFFFFu;
    }
    state_ = next;
    return next % n;
}

std::uint32_t Lcg15::next() {
    state_ = state_ * 214013u + 2531011u;
    return (state_ >> 16) & 0x7FFFu;
}

} // namespace sim
