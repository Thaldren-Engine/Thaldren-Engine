#include "sim/trig.h"

namespace sim::trig {
namespace {

constexpr std::int16_t kTable[kSteps + kTail] = {
#include "sin_table.inc"
};

// Index used by the rounded multiply helpers: (angle + bias) >> 6, masked to an
// even byte offset within the 512-entry turn, halved to an entry index.
// sin bias = half a step (0x20); cos bias = a quarter turn plus half a step (0x4020).
// Both wrap inside the first 512 entries (the tail is never used here).
constexpr int rounded_index(Angle a, std::uint32_t bias) {
    return static_cast<int>((((static_cast<std::uint32_t>(a) + bias) >> 6) & 0x3FEu) >> 1);
}

std::int32_t mul_q13_round(std::int32_t t, std::int32_t magnitude) {
    const std::int64_t p = static_cast<std::int64_t>(t) * magnitude + 0x1000;
    // SHRD 13 of the 64-bit product: keep the low 32 bits of p >> 13.
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> kQ));
}

} // namespace

std::span<const std::int16_t, kSteps + kTail> table() { return std::span<const std::int16_t, kSteps + kTail>(kTable); }

std::int32_t sin_q13(Angle a) { return kTable[a >> 7]; }
std::int32_t cos_q13(Angle a) { return kTable[(a >> 7) + kTail]; }

std::int32_t mul_sin(Angle a, std::int32_t magnitude) {
    return mul_q13_round(kTable[rounded_index(a, 0x20u)], magnitude);
}

std::int32_t mul_cos(Angle a, std::int32_t magnitude) {
    return mul_q13_round(kTable[rounded_index(a, 0x4020u)], magnitude);
}

} // namespace sim::trig
