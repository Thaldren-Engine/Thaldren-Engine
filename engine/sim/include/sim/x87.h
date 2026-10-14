#pragma once
// Floating-point conversions with fully defined results, matching an x87 FPU
// configured for 53-bit precision and round-to-nearest.

#include <cmath>
#include <cstdint>

namespace sim::x87 {

// Truncating float -> int64. NaN or out-of-range yields INT64_MIN
// (the x87 "integer indefinite" value).
inline std::int64_t ftol64(double v) {
    constexpr double kLimit = 9223372036854775808.0; // 2^63
    // Doubles next to -2^63 are 2048 apart, so v >= -2^63 is exactly the
    // condition for trunc(v) to fit. NaN fails both comparisons.
    if (!(v >= -kLimit && v < kLimit)) {
        return INT64_MIN;
    }
    return static_cast<std::int64_t>(std::trunc(v));
}

// Callers that keep only the low 32 bits of the result.
inline std::int32_t ftol32(double v) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(ftol64(v))));
}

} // namespace sim::x87
