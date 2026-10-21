#pragma once
// Floating-point conversions with fully defined results, matching an x87 FPU
// configured for 53-bit precision and round-to-nearest.

#include <cmath>
#include <cstdint>

namespace sim::x87 {

// The FPU's invalid-operation trap unmasked (a program switch; off by default): an
// out-of-range conversion then faults, as the x87's FISTP does with the trap unmasked.
inline bool& invalid_traps() {
    static bool on = false;
    return on;
}
// Raises the invalid operation on the floating-point unit (a fault when its trap is unmasked).
inline void raise_invalid() {
    volatile double z = 0.0;
    volatile double r = z / z;
    (void)r;
}

// Truncating float -> int64. NaN or out-of-range yields INT64_MIN
// (the x87 "integer indefinite" value).
inline std::int64_t ftol64(double v) {
    constexpr double kLimit = 9223372036854775808.0; // 2^63
    // Doubles next to -2^63 are 2048 apart, so v >= -2^63 is exactly the
    // condition for trunc(v) to fit. NaN fails both comparisons.
    if (!(v >= -kLimit && v < kLimit)) {
        if (invalid_traps()) raise_invalid();
        return INT64_MIN;
    }
    return static_cast<std::int64_t>(std::trunc(v));
}

// Callers that keep only the low 32 bits of the result.
inline std::int32_t ftol32(double v) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(ftol64(v))));
}

// FCOM/FCOMP + FNSTSW conditions as the original tests them. An unordered (NaN)
// compare sets C3, C2 and C0 together, so it counts as "equal" and as "less".
// "test ah,40h" (C3): equal, or unordered.
inline bool equal(double a, double b) { return !(a < b || a > b); }
// "test ah,01h" (C0): a below b, or unordered.
inline bool less(double a, double b) { return !(a >= b); }

} // namespace sim::x87
