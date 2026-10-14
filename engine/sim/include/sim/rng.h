#pragma once
// The simulation's two random streams. Their exact arithmetic, draw count and
// draw order are part of the simulation contract.

#include <cstdint>

namespace sim {

// Multiplicative generator (multiplier 16807, modulus 2^31-1 style) with 32-bit
// wrapping in the multiply step. This is deliberately not a textbook
// Park-Miller generator: the product is allowed to overflow.
class BattleRng {
public:
    static constexpr std::uint32_t kSeedMask = 0x66E29572u;

    void seed(std::uint32_t seed) { state_ = (seed ^ kSeedMask) | 1u; }

    // Uniform-ish value in [0, n). n <= 1 or n >= 2^31 returns 0 and leaves the state untouched.
    std::uint32_t next_mod(std::uint32_t n);

    std::uint32_t state() const { return state_; }
    void set_state(std::uint32_t s) { state_ = s; }

private:
    std::uint32_t state_ = 1;
};

// Linear congruential generator (a = 214013, c = 2531011) returning 15 bits.
class Lcg15 {
public:
    void seed(std::uint32_t seed) { state_ = seed; }
    std::uint32_t next();

    std::uint32_t state() const { return state_; }
    void set_state(std::uint32_t s) { state_ = s; }

private:
    std::uint32_t state_ = 1;
};

} // namespace sim
