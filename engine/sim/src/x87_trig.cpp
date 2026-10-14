#include "sim/x87_trig.h"

#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>

namespace sim::x87 {
namespace {

// Signed fixed point, two's complement, kLimbs x 32 bits, kFrac fractional bits.
constexpr int kLimbs = 12;
constexpr int kFrac = 320;
using Fix = std::array<std::uint32_t, kLimbs>;  // little-endian limbs

void fix_add(Fix& a, const Fix& b) {
    std::uint64_t c = 0;
    for (int i = 0; i < kLimbs; ++i) {
        c += static_cast<std::uint64_t>(a[i]) + b[i];
        a[i] = static_cast<std::uint32_t>(c);
        c >>= 32;
    }
}

void fix_neg(Fix& a) {
    for (auto& l : a) l = ~l;
    Fix one{};
    one[0] = 1;
    fix_add(a, one);
}

bool fix_is_neg(const Fix& a) { return (a[kLimbs - 1] >> 31) != 0; }

// Unsigned (a * b) >> kFrac, both non-negative and small enough not to overflow.
Fix fix_mul(const Fix& a, const Fix& b) {
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
Fix fix_div_small(const Fix& a, std::uint32_t d) {
    Fix r{};
    std::uint64_t rem = 0;
    for (int i = kLimbs - 1; i >= 0; --i) {
        const std::uint64_t cur = (rem << 32) | a[i];
        r[i] = static_cast<std::uint32_t>(cur / d);
        rem = cur % d;
    }
    return r;
}

bool fix_is_zero(const Fix& a) {
    for (auto l : a)
        if (l) return false;
    return true;
}

// |double| as fixed point (exact for the magnitudes used here).
Fix fix_from_double(double v) {
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
Ext80 fix_to_ext(Fix a) {
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

// Hardware FSINCOS results that differ from sincos_x87_model (by a few units in
// the last place), found by checking every angle on the reference CPU; see docs/RE_MEMORY.md.
// Packed: bits 0-15 angle, bit 16 cosine, bits 17-24 mantissa delta + 128.
constexpr std::uint32_t kExceptions[] = {
#include "x87_sincos_exceptions.inc"
    0xFFFFFFFFu,  // terminator
};

} // namespace

double angle_step() {
    double d;
    std::memcpy(&d, &kAngleStepBits, 8);
    return d;
}

namespace {

// sin/cos series for |x| (x as fixed-point magnitude); returns sin(|x|), cos(|x|).
void series(const Fix& x, Fix& s, Fix& c) {
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

// pi/2 as the FPU knows it: the 66-bit pi 0xC90FDAA22168C234C * 2^-66, halved.
Fix half_pi_66() {
    static const std::uint64_t hi = 0xC90FDAA22ull, lo = 0x168C234Cull;  // 0xC90FDAA22'168C234C (68 bits)
    Fix r{};
    // value = (hi * 2^32 + lo) * 2^-67 ; fixed = value * 2^kFrac
    const int shift = kFrac - 67;
    auto set_bits = [&](std::uint64_t v, int at) {
        for (int bit = 0; bit < 64; ++bit) {
            if (!((v >> bit) & 1)) continue;
            const int pos = bit + at;
            r[static_cast<std::size_t>(pos / 32)] |= 1u << (pos % 32);
        }
    };
    set_bits(lo, shift);
    set_bits(hi, shift + 32);
    return r;
}

} // namespace

void sincos_correctly_rounded(double a, Ext80& sine, Ext80& cosine) {
    Fix s, c;
    series(fix_from_double(a), s, c);
    if (a < 0) fix_neg(s);  // sin is odd; the series ran on |a|
    sine = fix_to_ext(s);
    cosine = fix_to_ext(c);
}

void sincos_x87_model(double a, Ext80& sine, Ext80& cosine) {
    // Reduce |a| by multiples of the FPU's pi/2, evaluate on the remainder, then
    // map the quadrant back. The remainder is exact; only the final rounding remains.
    static const Fix hp = half_pi_66();
    const double mag = std::fabs(a);
    const int k = static_cast<int>(std::floor(mag / 1.5707963267948966 + 0.5));
    Fix r = fix_from_double(mag);
    Fix neg_hp = hp;
    fix_neg(neg_hp);
    for (int i = 0; i < k; ++i) fix_add(r, neg_hp);
    const bool r_neg = fix_is_neg(r);
    if (r_neg) fix_neg(r);
    Fix sr, cr;
    series(r, sr, cr);
    if (r_neg) fix_neg(sr);
    Fix s, c;
    switch (k & 3) {
    case 0: s = sr; c = cr; break;
    case 1: s = cr; c = sr; fix_neg(c); break;
    case 2: s = sr; fix_neg(s); c = cr; fix_neg(c); break;
    default: s = cr; fix_neg(s); c = sr; break;
    }
    if (a < 0) fix_neg(s);
    sine = fix_to_ext(s);
    cosine = fix_to_ext(c);
}

void sincos_for_angle(std::int16_t angle, Ext80& sine, Ext80& cosine) {
    // Computed once per angle on first use (the series is slow); not thread-safe.
    struct Entry {
        Ext80 s, c;
        bool ready = false;
    };
    static std::vector<Entry> cache(65536);
    Entry& e = cache[static_cast<std::uint16_t>(angle)];
    if (!e.ready) {
        sincos_x87_model(static_cast<double>(angle) * angle_step(), e.s, e.c);
        for (const std::uint32_t x : kExceptions) {
            if (x == 0xFFFFFFFFu || static_cast<std::uint16_t>(x) != static_cast<std::uint16_t>(angle)) continue;
            Ext80& v = (x >> 16) & 1 ? e.c : e.s;
            const int delta = static_cast<int>((x >> 17) & 0xFF) - 128;
            v.mantissa += static_cast<std::uint64_t>(static_cast<std::int64_t>(delta));
        }
        e.ready = true;
    }
    sine = e.s;
    cosine = e.c;
}

double mul_pc53(std::int32_t x, const Ext80& v) {
    if (x == 0 || v.mantissa == 0) return 0.0;
    const bool neg = (x < 0) != v.negative;
    const std::uint64_t ax = x < 0 ? 0ull - static_cast<std::uint64_t>(static_cast<std::int64_t>(x)) : static_cast<std::uint64_t>(x);
    // Exact 128-bit product ax * mantissa (ax < 2^32).
    const std::uint64_t m_lo = v.mantissa & 0xFFFFFFFFu, m_hi = v.mantissa >> 32;
    const std::uint64_t p_lo = ax * m_lo, p_hi = ax * m_hi;
    std::uint64_t lo = p_lo + (p_hi << 32);
    std::uint64_t hi = (p_hi >> 32) + (lo < p_lo ? 1 : 0);
    // Keep the top 53 bits, round to nearest even on the rest.
    int width = 0;  // number of significant bits
    for (std::uint64_t t = hi; t; t >>= 1) ++width;
    width = hi ? width + 64 : 0;
    if (!hi)
        for (std::uint64_t t = lo; t; t >>= 1) ++width;
    const int drop = width > 53 ? width - 53 : 0;
    std::uint64_t keep;
    bool half = false, sticky = false;
    if (drop == 0) {
        keep = lo;
    } else {
        auto bit = [&](int i) -> bool { return i >= 64 ? ((hi >> (i - 64)) & 1) != 0 : ((lo >> i) & 1) != 0; };
        keep = 0;
        for (int i = 0; i < 53; ++i)
            if (bit(drop + i)) keep |= 1ull << i;
        half = bit(drop - 1);
        for (int i = drop - 2; i >= 0 && !sticky; --i) sticky = bit(i);
    }
    int exponent = v.exponent + drop;
    if (half && (sticky || (keep & 1))) {
        ++keep;
        if (keep == (1ull << 53)) {
            keep >>= 1;
            ++exponent;
        }
    }
    const double r = std::ldexp(static_cast<double>(keep), exponent);
    return neg ? -r : r;
}

std::int32_t fist_nearest(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return INT32_MIN;  // also NaN
    double r = std::floor(v);
    const double diff = v - r;  // exact for these magnitudes
    if (diff > 0.5 || (diff == 0.5 && std::fmod(r, 2.0) != 0.0)) r += 1.0;
    if (r > 2147483647.0) return INT32_MIN;
    return static_cast<std::int32_t>(r);
}

void rotate_point_2d(std::int16_t angle, std::int32_t& x, std::int32_t& y) {
    if (angle == 0) return;
    Ext80 s, c;
    sincos_for_angle(angle, s, c);
    const double nx = mul_pc53(x, c) - mul_pc53(y, s);
    const double ny = mul_pc53(y, c) + mul_pc53(x, s);
    x = fist_nearest(nx);
    y = fist_nearest(ny);
}

void rotate_point_3d(const std::int32_t in[3], std::int32_t out[3], const std::int16_t angles[3]) {
    std::int32_t a = in[0], b = in[1], z = in[2];
    rotate_point_2d(angles[0], a, b);
    std::int32_t y = b;
    rotate_point_2d(angles[2], y, z);
    rotate_point_2d(angles[1], a, z);
    out[0] = a;
    out[1] = y;
    out[2] = z;
}

} // namespace sim::x87
