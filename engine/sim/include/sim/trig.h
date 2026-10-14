#pragma once
// Table-driven fixed-point trigonometry (Q13, 512 steps per turn).

#include <cstdint>
#include <span>

#include "sim/fixed.h"

namespace sim::trig {

constexpr int kSteps = 512;        // table entries per full turn
constexpr int kTail = 128;         // quarter-turn tail so cos never wraps
constexpr int kQ = 13;             // 1.0 == 1 << kQ
constexpr std::int32_t kOne = 1 << kQ;

// The full 640-entry table (sin for 0..511, then a repeat of the first quarter).
std::span<const std::int16_t, kSteps + kTail> table();

// Unrounded lookups: index = angle >> 7.
std::int32_t sin_q13(Angle a);
std::int32_t cos_q13(Angle a);

// magnitude * sin/cos(angle), rounding both the angle step and the product:
// (table[idx] * magnitude + 0x1000) >> 13 with a 64-bit intermediate, where
// idx = ((angle + 0x20) >> 7) & 511 for sin and ((angle + 0x4020) >> 7) & 511 for cos.
std::int32_t mul_sin(Angle a, std::int32_t magnitude);
std::int32_t mul_cos(Angle a, std::int32_t magnitude);

} // namespace sim::trig
