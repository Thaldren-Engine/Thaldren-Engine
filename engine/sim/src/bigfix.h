#pragma once
// Internal: signed multi-precision fixed point used to compute exactly
// rounded x87 transcendental results (not part of the public API).

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "sim/x87_trig.h"

namespace sim::x87::detail {

// Signed fixed point, two's complement, kLimbs x 32 bits, kFrac fractional bits.
constexpr int kLimbs = 12;
constexpr int kFrac = 320;
using Fix = std::array<std::uint32_t, kLimbs>;  // little-endian limbs

inline void fix_add(Fix& a, const Fix& b) {
    std::uint64_t c = 0;
    for (int i = 0; i < kLimbs; ++i) {
        c += static_cast<std::uint64_t>(a[i]) + b[i];
        a[i] = static_cast<std::uint32_t>(c);
        c >>= 32;
    }
}

inline void fix_neg(Fix& a) {
    for (auto& l : a) l = ~l;
    Fix one{};
    one[0] = 1;
    fix_add(a, one);
}

inline bool fix_is_neg(const Fix& a) { return (a[kLimbs - 1] >> 31) != 0; }

// Unsigned (a * b) >> kFrac, both non-negative and small enough not to overflow.
inline Fix fix_mul(const Fix& a, const Fix& b) {
    std::vector<std::uint64_t> wide(2 * kLimbs + 1, 0);
    for (int i = 0; i < kLimbs; ++i) {
        std::uint64_t carry = 0;
        for (int j = 0; j < kLimbs; ++j) {
            const std::uint64_t cur = wide[static_cast<std::size_t>(i + j)] + static_cast<std::uint64_t>(a[i]) * b[j] + carry;
            wide[static_cast<std::size_t>(i + j)] = cur & 0xFFFFFFFFu;
            carry = cur >> 32;
        }
        wide[static_cast<std::size_t>(i + kLimbs)] += carry;
    }
    Fix r{};
    constexpr int shift = kFrac / 32;  // kFrac is a multiple of 32
    for (int i = 0; i < kLimbs; ++i) r[i] = static_cast<std::uint32_t>(wide[static_cast<std::size_t>(i + shift)]);
    return r;
}

// Unsigned a / d for a small divisor.
inline Fix fix_div_small(const Fix& a, std::uint32_t d) {
    Fix r{};
    std::uint64_t rem = 0;
    for (int i = kLimbs - 1; i >= 0; --i) {
        const std::uint64_t cur = (rem << 32) | a[i];
        r[i] = static_cast<std::uint32_t>(cur / d);
        rem = cur % d;
    }
    return r;
}

inline bool fix_is_zero(const Fix& a) {
    for (auto l : a)
        if (l) return false;
    return true;
}

// |double| as fixed point (exact for the magnitudes used here).
inline Fix fix_from_double(double v) {
    int e = 0;
    const double m = std::frexp(std::fabs(v), &e);  // v = m * 2^e, 0.5 <= m < 1
    const std::uint64_t mant = static_cast<std::uint64_t>(std::ldexp(m, 53));  // exact
    // value = mant * 2^(e - 53); fixed = mant << (kFrac + e - 53)
    Fix r{};
    const int shift = kFrac + e - 53;
    for (int bit = 0; bit < 53; ++bit) {
        if (!((mant >> bit) & 1)) continue;
        const int at = bit + shift;
        if (at >= 0 && at < kLimbs * 32) r[static_cast<std::size_t>(at / 32)] |= 1u << (at % 32);
    }
    return r;
}

// Rounds a fixed value to a 64-bit mantissa, nearest even.
inline Ext80 fix_to_ext(Fix a) {
    Ext80 out;
    out.negative = fix_is_neg(a);
    if (out.negative) fix_neg(a);
    int top = -1;
    for (int i = kLimbs * 32 - 1; i >= 0; --i) {
        if ((a[static_cast<std::size_t>(i / 32)] >> (i % 32)) & 1) {
            top = i;
            break;
        }
    }
    if (top < 0) return Ext80{0, 0, false};
    auto bit = [&](int i) -> std::uint64_t { return i < 0 ? 0 : (a[static_cast<std::size_t>(i / 32)] >> (i % 32)) & 1; };
    std::uint64_t mant = 0;
    for (int i = 0; i < 64; ++i) mant |= bit(top - i) << (63 - i);
    const int low = top - 64;  // first discarded bit
    const bool half = bit(low) != 0;
    bool sticky = false;
    for (int i = low - 1; i >= 0 && !sticky; --i) sticky = bit(i) != 0;
    int exponent = top - 63 - kFrac;
    if (half && (sticky || (mant & 1))) {
        ++mant;
        if (mant == 0) {  // carried out of 64 bits
            mant = 0x8000000000000000ull;
            ++exponent;
        }
    }
    out.mantissa = mant;
    out.exponent = exponent;
    return out;
}

// sin/cos series for |x| (x as fixed-point magnitude); returns sin(|x|), cos(|x|).
inline void series(const Fix& x, Fix& s, Fix& c) {
    const Fix x2 = fix_mul(x, x);
    s = x;
    c = Fix{};
    c[kFrac / 32] = 1;  // 1.0
    Fix term_s = x, term_c = c;
    for (std::uint32_t n = 1; n < 200; ++n) {
        // term_s_n = term_s_{n-1} * x^2 / ((2n)(2n+1)), term_c_n = term_c_{n-1} * x^2 / ((2n-1)(2n))
        term_s = fix_div_small(fix_div_small(fix_mul(term_s, x2), 2 * n), 2 * n + 1);
        term_c = fix_div_small(fix_div_small(fix_mul(term_c, x2), 2 * n - 1), 2 * n);
        if (fix_is_zero(term_s) && fix_is_zero(term_c)) break;
        Fix ts = term_s, tc = term_c;
        if (n % 2) {
            fix_neg(ts);
            fix_neg(tc);
        }
        fix_add(s, ts);
        fix_add(c, tc);
    }
}

} // namespace sim::x87::detail
