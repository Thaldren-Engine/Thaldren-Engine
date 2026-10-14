#pragma once
// Fixed-point and integer helpers with explicitly defined wraparound.
// The simulation never relies on signed overflow (undefined in C++); it routes
// arithmetic that may wrap through these helpers instead.

#include <cstdint>

namespace sim {

// World coordinates are 16.16 fixed point.
using Fixed16 = std::int32_t;
constexpr int kFixedShift = 16;
constexpr Fixed16 kFixedOne = 1 << kFixedShift;

constexpr Fixed16 to_fixed(std::int32_t whole) {
    return static_cast<Fixed16>(static_cast<std::uint32_t>(whole) << kFixedShift);
}

// Angles: 16 bits per full turn.
using Angle = std::uint16_t;

// Two's-complement wrapping arithmetic on 32-bit values.
constexpr std::int32_t wrap_add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
constexpr std::int32_t wrap_sub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
constexpr std::int32_t wrap_mul(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}

// Arithmetic right shift (well defined for negative values since C++20).
constexpr std::int32_t sar(std::int32_t v, int n) { return v >> n; }
constexpr std::int64_t sar64(std::int64_t v, int n) { return v >> n; }

} // namespace sim
