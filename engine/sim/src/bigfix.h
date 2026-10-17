#pragma once
// Internal: signed multi-precision fixed point used to compute exactly
// rounded x87 transcendental results (not part of the public API).

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

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
// (Bits kFrac.. of the exact product: any exact multiplication gives the same value; with a
// 64 x 64 -> 128-bit multiply it runs on 64-bit limbs.)
#if (defined(_MSC_VER) && defined(_M_X64)) || defined(__SIZEOF_INT128__)
inline std::uint64_t fix_mul64(std::uint64_t a, std::uint64_t b, std::uint64_t& hi) {
#if defined(_MSC_VER) && !defined(__clang__)
    return _umul128(a, b, &hi);
#else
    const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
    hi = static_cast<std::uint64_t>(p >> 64);
    return static_cast<std::uint64_t>(p);
#endif
}
inline Fix fix_mul(const Fix& a, const Fix& b) {
    constexpr int n = kLimbs / 2;
    std::uint64_t a64[n], b64[n], wide[2 * n] = {};
    for (int i = 0; i < n; ++i) {
        a64[i] = a[2 * i] | static_cast<std::uint64_t>(a[2 * i + 1]) << 32;
        b64[i] = b[2 * i] | static_cast<std::uint64_t>(b[2 * i + 1]) << 32;
    }
    for (int i = 0; i < n; ++i) {
        if (a64[i] == 0) continue;
        std::uint64_t carry = 0;
        for (int j = 0; j < n; ++j) {
            std::uint64_t hi;
            const std::uint64_t lo = fix_mul64(a64[i], b64[j], hi);
            const std::uint64_t s1 = wide[i + j] + lo;
            const std::uint64_t s2 = s1 + carry;
            carry = hi + (s1 < lo) + (s2 < s1);  // hi <= 2^64 - 2: no overflow
            wide[i + j] = s2;
        }
        wide[i + n] = carry;  // not written by earlier rows
    }
    Fix r{};
    constexpr int shift = kFrac / 64;
    static_assert(kFrac % 64 == 0 && kLimbs % 2 == 0, "64-bit limbs");
    for (int i = 0; i < n; ++i) {
        r[2 * i] = static_cast<std::uint32_t>(wide[i + shift]);
        r[2 * i + 1] = i + shift < 2 * n ? static_cast<std::uint32_t>(wide[i + shift] >> 32) : 0;
    }
    return r;
}
#else
inline Fix fix_mul(const Fix& a, const Fix& b) {
    std::array<std::uint64_t, 2 * kLimbs + 1> wide{};
    for (int i = 0; i < kLimbs; ++i) {
        if (a[i] == 0) continue;  // adds nothing (series terms are mostly zero limbs)
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
#endif

// Unsigned a / d for a small divisor.
inline Fix fix_div_small(const Fix& a, std::uint32_t d) {
    Fix r{};
    std::uint64_t rem = 0;
#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
    // The same long division on 64-bit limbs (128 / 64 bits per limb).
    for (int i = kLimbs / 2 - 1; i >= 0; --i) {
        const std::uint64_t limb = a[2 * i] | static_cast<std::uint64_t>(a[2 * i + 1]) << 32;
        if (limb == 0 && rem == 0) continue;
        const std::uint64_t q = _udiv128(rem, limb, d, &rem);
        r[2 * i] = static_cast<std::uint32_t>(q);
        r[2 * i + 1] = static_cast<std::uint32_t>(q >> 32);
    }
#else
    int i = kLimbs - 1;
    while (i >= 0 && a[i] == 0) --i;  // leading zero limbs: zero quotient limbs, no remainder
    for (; i >= 0; --i) {
        const std::uint64_t cur = (rem << 32) | a[i];
        r[i] = static_cast<std::uint32_t>(cur / d);
        rem = cur % d;
    }
#endif
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

// Rounds a fixed value to a `bits`-bit significand (kept left-aligned in the 64-bit
// mantissa), nearest even.
inline Ext80 fix_to_ext(Fix a, int bits = 64) {
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
    for (int i = 0; i < bits; ++i) mant |= bit(top - i) << (bits - 1 - i);
    const int low = top - bits;  // first discarded bit
    const bool half = bit(low) != 0;
    bool sticky = false;
    for (int i = low - 1; i >= 0 && !sticky; --i) sticky = bit(i) != 0;
    int exponent = top - (bits - 1) - kFrac;
    if (half && (sticky || (mant & 1))) {
        ++mant;
        if (bits < 64 ? mant == (1ull << bits) : mant == 0) {  // carried out of the significand
            mant = 1ull << (bits - 1);
            ++exponent;
        }
    }
    // Left-align in the 64-bit mantissa.
    mant <<= 64 - bits;
    exponent -= 64 - bits;
    out.mantissa = mant;
    out.exponent = exponent;
    return out;
}

// Rounds a fixed value to `bits` significant bits (nearest even), staying fixed point.
inline Fix fix_round_bits(Fix a, int bits) {
    const bool neg = fix_is_neg(a);
    if (neg) fix_neg(a);
    int top = -1;
    for (int i = kLimbs * 32 - 1; i >= 0; --i)
        if ((a[static_cast<std::size_t>(i / 32)] >> (i % 32)) & 1) {
            top = i;
            break;
        }
    const int low = top - bits;  // first discarded bit
    if (top >= 0 && low >= 0) {
        auto bit = [&](int i) { return i >= 0 && ((a[static_cast<std::size_t>(i / 32)] >> (i % 32)) & 1) != 0; };
        bool sticky = false;
        for (int i = low - 1; i >= 0 && !sticky; --i) sticky = bit(i);
        const bool round_up = bit(low) && (sticky || bit(low + 1));
        for (int i = 0; i <= low; ++i) a[static_cast<std::size_t>(i / 32)] &= ~(1u << (i % 32));
        if (round_up) {
            Fix ulp{};
            ulp[static_cast<std::size_t>((low + 1) / 32)] = 1u << ((low + 1) % 32);
            fix_add(a, ulp);
        }
    }
    if (neg) fix_neg(a);
    return a;
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
        // (Two truncating divisions of a non-negative value equal one by the product.)
        term_s = fix_div_small(fix_mul(term_s, x2), (2 * n) * (2 * n + 1));
        term_c = fix_div_small(fix_mul(term_c, x2), (2 * n - 1) * (2 * n));
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
