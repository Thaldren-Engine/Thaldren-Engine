#include "sim/weapons.h"

#include <cmath>
#include <cstring>

#include "sim/x87.h"
#include "sim/x87_math.h"

namespace sim {
namespace {

double from_bits(std::uint64_t b) {
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}

// The original's constants (as stored, slightly off pi/2, pi/4 and 1/pi).
const double kHalfPi = from_bits(0x3FF921FB54442D11ull);
const double kQuarterPi = from_bits(0x3FE921FB54442D11ull);
const double kInvPi = from_bits(0x3FD45F306DC9C889ull);

} // namespace

std::int16_t ballistic_pitch(std::int32_t dx, std::int32_t dy, std::int32_t dz, std::int32_t speed, float min_angle,
                             std::int32_t gravity) {
    // Every operation below rounds to a double, as the x87 does at 53-bit precision.
    const double g = gravity;
    const double gg = static_cast<std::int32_t>(static_cast<std::uint32_t>(gravity) * static_cast<std::uint32_t>(gravity));
    const double y = dy, v = speed;
    const double h = x87::crt_hypot(dx, dz);
    const double gdy = g * y;
    const double h2 = h * h, v2 = v * v, d2 = y * y;
    const double h4 = h2 * h2;
    const double t = v2 - gdy * -2.0;
    const double s = d2 + h2;
    const double x = t * v2 + d2 * gg;
    const double disc = x * h4 - h4 * gg * s;
    if (!(disc >= 0.0)) return -32768;
    const double r = std::sqrt(disc);
    const double b = (v2 + gdy) * h2;
    const double two_s = s + s;
    const double q_flat = (r + b) / two_s, q_steep = (b - r) / two_s;

    // Launch angle = acos(sqrt(q) / v); a non-positive q gives (about) 90 degrees.
    // The flat solution is stored as a double, the steep one stays extended.
    double flat = kHalfPi;
    bool flat_nan = false;
    if (q_flat > 0.0) {
        const auto a = x87::crt_acos(std::sqrt(q_flat) / v);
        if (a) flat = x87::to_double(*a);
        else flat_nan = true;
    }
    std::optional<x87::Ext80> steep;  // nullopt here = NaN
    bool steep_is_half_pi = true;
    if (q_steep > 0.0) {
        steep = x87::crt_acos(std::sqrt(q_steep) / v);
        steep_is_half_pi = false;
    }

    double chosen;
    // Unordered comparisons (NaN) count as "less" and "equal" on the x87.
    const bool min_nan = std::isnan(min_angle);
    const bool flat_above_min = flat_nan || min_nan || static_cast<double>(min_angle) < flat;
    const bool flat_within = flat_nan || !(flat > kQuarterPi);
    if (flat_above_min && flat_within) {
        chosen = flat_nan ? std::nan("") : flat;
    } else {
        if (steep_is_half_pi) {
            if (!(kHalfPi > static_cast<double>(min_angle)) || kHalfPi > kQuarterPi) return -32768;
            chosen = kHalfPi;
        } else {
            if (!steep || min_nan) return -32768;  // unordered against the minimum
            if (x87::compare(*steep, static_cast<double>(min_angle)) <= 0) return -32768;
            if (x87::compare(*steep, kQuarterPi) > 0) return -32768;
            chosen = x87::to_double(*steep);  // the multiply rounds the extended angle
        }
    }
    return static_cast<std::int16_t>(x87::ftol32(chosen * 32768.0 * kInvPi));
}

} // namespace sim
