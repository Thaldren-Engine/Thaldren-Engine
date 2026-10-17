#include "sim/x87_math.h"

#include <cmath>
#include <cstring>

#include "bigfix.h"

namespace sim::x87 {
namespace {

// Minimal 256-bit unsigned integer (little-endian 64-bit limbs).
struct U256 {
    std::uint64_t w[4] = {0, 0, 0, 0};
};

U256 from64(std::uint64_t v) {
    U256 r;
    r.w[0] = v;
    return r;
}

int bitlen(const U256& a) {
    for (int i = 3; i >= 0; --i) {
        if (!a.w[i]) continue;
        int n = 0;
        for (std::uint64_t t = a.w[i]; t; t >>= 1) ++n;
        return i * 64 + n;
    }
    return 0;
}

bool bit(const U256& a, int i) { return i >= 0 && i < 256 && ((a.w[i / 64] >> (i % 64)) & 1) != 0; }

U256 shl(const U256& a, int n) {
    U256 r;
    if (n >= 256) return r;
    const int words = n / 64, bits = n % 64;
    for (int i = 3; i >= words; --i) {
        std::uint64_t v = a.w[i - words] << bits;
        if (bits && i - words - 1 >= 0) v |= a.w[i - words - 1] >> (64 - bits);
        r.w[i] = v;
    }
    return r;
}

U256 shr(const U256& a, int n) {
    U256 r;
    if (n >= 256) return r;
    const int words = n / 64, bits = n % 64;
    for (int i = 0; i + words < 4; ++i) {
        std::uint64_t v = a.w[i + words] >> bits;
        if (bits && i + words + 1 < 4) v |= a.w[i + words + 1] << (64 - bits);
        r.w[i] = v;
    }
    return r;
}

// True if any bit below position n is set.
bool any_below(const U256& a, int n) {
    for (int i = 0; i < 4 && n > 0; ++i, n -= 64) {
        const std::uint64_t mask = n >= 64 ? ~0ull : ((1ull << n) - 1);
        if (a.w[i] & mask) return true;
    }
    return false;
}

int cmp(const U256& a, const U256& b) {
    for (int i = 3; i >= 0; --i) {
        if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
    }
    return 0;
}

U256 add(const U256& a, const U256& b) {
    U256 r;
    std::uint64_t c = 0;
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t s = a.w[i] + b.w[i];
        const std::uint64_t c1 = s < a.w[i];
        r.w[i] = s + c;
        const std::uint64_t c2 = r.w[i] < s;
        c = c1 | c2;
    }
    return r;
}

U256 sub(const U256& a, const U256& b) {  // a >= b
    U256 r;
    std::uint64_t borrow = 0;
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t d = a.w[i] - b.w[i];
        const std::uint64_t b1 = a.w[i] < b.w[i];
        r.w[i] = d - borrow;
        const std::uint64_t b2 = d < borrow;
        borrow = b1 | b2;
    }
    return r;
}

U256 mul64x64(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    const std::uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const std::uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    U256 r;
    r.w[0] = (p00 & 0xFFFFFFFFu) | (mid << 32);
    r.w[1] = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}

// Round v * 2^e (with an extra sticky bit below v) to `bits` significant bits, nearest even.
// Returns the significand (top bit at bits-1) and adjusts e to match.
std::uint64_t round_bits(const U256& v, int& e, int bits, bool sticky_in) {
    const int len = bitlen(v);
    if (len == 0) return 0;
    if (len <= bits) {
        // Callers keep enough bits that a sticky input can only occur with len > bits.
        e -= bits - len;
        return shl(v, bits - len).w[0];
    }
    const int shift = len - bits;
    std::uint64_t m = shr(v, shift).w[0];
    const bool half = bit(v, shift - 1);
    const bool rest = sticky_in || any_below(v, shift - 1);
    e += shift;
    if (half && (rest || (m & 1))) {
        ++m;
        if (bits < 64 ? m == (1ull << bits) : m == 0) {
            m = 1ull << (bits - 1);
            ++e;
        }
    }
    return m;
}

Ext80 make_ext(const U256& v, int e, bool negative, bool sticky = false) {
    Ext80 r;
    r.mantissa = round_bits(v, e, 64, sticky);
    r.exponent = e;
    r.negative = negative && r.mantissa != 0;
    return r;
}

void split(double d, std::uint64_t& m, int& e) {  // |d| = m * 2^e, m < 2^53
    if (d == 0) {
        m = 0;
        e = 0;
        return;
    }
    int ex = 0;
    const double fr = std::frexp(std::fabs(d), &ex);
    m = static_cast<std::uint64_t>(std::ldexp(fr, 53));
    e = ex - 53;
}

// Long division of a 256-bit value by a 64-bit divisor.
U256 divmod(const U256& n, std::uint64_t d, bool& nonzero_rem) {
    // Byte-wise long division; d < 2^53 keeps (rem << 8) | byte below 2^61.
    U256 q;
    std::uint64_t rem = 0;
    for (int i = 31; i >= 0; --i) {
        const std::uint64_t cur = (rem << 8) | ((n.w[i / 8] >> ((i % 8) * 8)) & 0xFF);
        q.w[i / 8] |= (cur / d) << ((i % 8) * 8);
        rem = cur % d;
    }
    nonzero_rem = rem != 0;
    return q;
}

U256 isqrt(const U256& n, bool& inexact) {
    U256 root, rem = n;
    int top = bitlen(n);
    top += top & 1;
    U256 b = shl(from64(1), top > 0 ? top - 2 : 0);
    while (bitlen(b)) {
        const U256 rb = add(root, b);
        if (cmp(rem, rb) >= 0) {
            rem = sub(rem, rb);
            root = add(shr(root, 1), b);
        } else {
            root = shr(root, 1);
        }
        b = shr(b, 2);
    }
    inexact = bitlen(rem) != 0;
    return root;
}

} // namespace

Ext80 div64(double a, double b) {
    std::uint64_t ma, mb;
    int ea, eb;
    split(a, ma, ea);
    split(b, mb, eb);
    if (ma == 0) return Ext80{};
    bool rem = false;
    const U256 q = divmod(shl(from64(ma), 74), mb, rem);
    return make_ext(q, ea - eb - 74, (a < 0) != (b < 0), rem);
}

Ext80 mul64(double a, double b) {
    std::uint64_t ma, mb;
    int ea, eb;
    split(a, ma, ea);
    split(b, mb, eb);
    if (ma == 0 || mb == 0) return Ext80{};
    return make_ext(mul64x64(ma, mb), ea + eb, (a < 0) != (b < 0));
}

Ext80 add64(const Ext80& a, const Ext80& b) {
    if (a.mantissa == 0) return b;
    if (b.mantissa == 0) return a;
    const bool a_big = a.exponent >= b.exponent;
    const Ext80& hi = a_big ? a : b;
    const Ext80& lo = a_big ? b : a;
    const int diff = hi.exponent - lo.exponent;
    if (diff > 130) return hi;  // the smaller operand is far below half an ulp
    const U256 big = shl(from64(hi.mantissa), diff);
    const U256 small = from64(lo.mantissa);
    if (hi.negative == lo.negative) return make_ext(add(big, small), lo.exponent, hi.negative);
    const int c = cmp(big, small);
    if (c == 0) return Ext80{};
    return c > 0 ? make_ext(sub(big, small), lo.exponent, hi.negative) : make_ext(sub(small, big), lo.exponent, lo.negative);
}

Ext80 sqrt64(double a) {
    std::uint64_t m;
    int e;
    split(a, m, e);
    if (m == 0) return Ext80{};
    U256 n = from64(m);
    if (e & 1) {
        n = shl(n, 1);
        e -= 1;
    }
    const std::uint64_t m2 = n.w[0];  // below 2^55
    n = shl(n, 80);
    bool inexact = false;
    // The root (below 2^68) from a double estimate (off by far less than 2^18), finished bit
    // by bit from 2^18 below it; checked, with the plain digit-by-digit root as the fallback.
    auto square = [](const U256& c) {  // c < 2^68
        const std::uint64_t w0 = c.w[0], w1 = c.w[1];
        return add(add(mul64x64(w0, w0), shl(mul64x64(w0, 2 * w1), 64)), shl(from64(w1 * w1), 128));
    };
    std::uint64_t em;
    int ee;
    split(std::sqrt(static_cast<double>(m2)) * 1099511627776.0, em, ee);  // n = m2 2^80: sqrt(m2) 2^40
    U256 root = ee >= 0 ? shl(from64(em), ee) : from64(em >> -ee);
    const U256 margin = from64(1ull << 18);
    root = cmp(root, margin) > 0 ? sub(root, margin) : U256{};
    for (int b = 19; b >= 0; --b) {
        const U256 c = add(root, shl(from64(1), b));
        if (cmp(square(c), n) <= 0) root = c;
    }
    const U256 sq = square(root);
    if (bitlen(root) <= 68 && cmp(sq, n) <= 0 && cmp(square(add(root, from64(1))), n) > 0) {
        inexact = cmp(sq, n) != 0;
    } else {
        root = isqrt(n, inexact);
    }
    return make_ext(root, (e - 80) / 2, false, inexact);
}

double to_double(const Ext80& v) {
    if (v.mantissa == 0) return v.negative ? -0.0 : 0.0;
    int e = v.exponent;
    const std::uint64_t m = round_bits(from64(v.mantissa), e, 53, false);
    const double r = std::ldexp(static_cast<double>(m), e);
    return v.negative ? -r : r;
}

double crt_hypot(double x, double y) {
    x = std::fabs(x);
    y = std::fabs(y);
    const double m = y > x ? y : x;
    if (m == 0) return 0.0;
    const double xs = to_double(div64(x, m));
    const double ys = to_double(div64(y, m));
    const double s = to_double(add64(mul64(xs, xs), mul64(ys, ys)));
    const double q = to_double(sqrt64(s));
    // The original scales mantissas with frexp/ldexp; the rounding is the same as m * q.
    return to_double(mul64(m, q));
}

namespace {

// atan2(y, x) in multi-precision fixed point (Newton iterations on f(t) = x sin t - y cos t).
detail::Fix fpatan_fix(double y, double x) {
    using namespace detail;
    auto fix_signed = [](double v) {
        Fix f = fix_from_double(v);
        if (v < 0) fix_neg(f);
        return f;
    };
    auto smul = [](Fix a, Fix b) {
        const bool na = fix_is_neg(a), nb = fix_is_neg(b);
        if (na) fix_neg(a);
        if (nb) fix_neg(b);
        Fix r = fix_mul(a, b);
        if (na != nb) fix_neg(r);
        return r;
    };
    auto to_dbl = [](const Fix& f) {
        const Ext80 e = fix_to_ext(f);
        if (e.mantissa == 0) return 0.0;
        const double r = std::ldexp(static_cast<double>(e.mantissa), e.exponent);
        return e.negative ? -r : r;
    };
    Fix theta = fix_signed(std::atan2(y, x));
    const Fix fx = fix_signed(x), fy = fix_signed(y);
    for (int it = 0; it < 3; ++it) {
        Fix mag = theta;
        const bool neg = fix_is_neg(mag);
        if (neg) fix_neg(mag);
        Fix s, c;
        series(mag, s, c);
        if (neg) fix_neg(s);
        Fix num = smul(fx, s), t = smul(fy, c);
        fix_neg(t);
        fix_add(num, t);
        Fix den = smul(fx, c);
        fix_add(den, smul(fy, s));
        const double delta = to_dbl(num) / to_dbl(den);
        if (delta == 0) break;
        Fix d = fix_signed(delta);
        fix_neg(d);
        fix_add(theta, d);
    }
    return theta;
}

} // namespace

void fpatan_debug(double y, double x, std::uint64_t& hi, std::uint64_t& lo, int& top_out) {
    using namespace detail;
    Fix a = fpatan_fix(y, x);
    if (fix_is_neg(a)) fix_neg(a);
    int top = -1;
    for (int i = kLimbs * 32 - 1; i >= 0; --i)
        if ((a[static_cast<std::size_t>(i / 32)] >> (i % 32)) & 1) {
            top = i;
            break;
        }
    hi = lo = 0;
    auto bit = [&](int i) -> std::uint64_t { return i < 0 ? 0 : (a[static_cast<std::size_t>(i / 32)] >> (i % 32)) & 1; };
    for (int i = 0; i < 64; ++i) hi |= bit(top - i) << (63 - i);
    for (int i = 0; i < 64; ++i) lo |= bit(top - 64 - i) << (63 - i);
    top_out = top - kFrac;
}

Ext80 fpatan(std::int32_t y, std::int32_t x) { return fpatan(static_cast<double>(y), static_cast<double>(x)); }

Ext80 fpatan(double y, double x, int bits) {
    using namespace detail;
    if (y == 0 && x >= 0) return Ext80{};
    // The multi-precision evaluation is slow and the same arguments recur (ballistic aims
    // at targets that keep still): the last results are kept, by argument bits.
    struct Memo {
        std::uint64_t y = 0, x = 0;
        int bits = 0;
        Ext80 r;
    };
    thread_local std::array<Memo, 1024> memo{};
    std::uint64_t yb, xb;
    std::memcpy(&yb, &y, 8);
    std::memcpy(&xb, &x, 8);
    std::uint64_t h = (yb * 0x9E3779B97F4A7C15ull) ^ (xb + 0x632BE59BD9B4E019ull + static_cast<std::uint64_t>(bits));
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    Memo& m = memo[static_cast<std::size_t>(h >> 54)];
    if (m.bits == bits && m.y == yb && m.x == xb) return m.r;
    const Fix theta = fpatan_fix(y, x);
    // With bits < 64 (precision control): the 64-bit result is rounded again (double rounding).
    const Ext80 r = bits == 64 ? fix_to_ext(theta, 64) : fix_to_ext(fix_round_bits(theta, 64), bits);
    m = {yb, xb, bits, r};
    return r;
}

std::optional<Ext80> crt_acos(double c) {
    if (std::isnan(c)) return std::nullopt;
    const double a = std::fabs(c);
    if (a < 1.0) {
        const double s = std::sqrt((1.0 + c) * (1.0 - c));  // each step rounded to a double
        return fpatan(s, c, 53);
    }
    if (a == 1.0) {
        if (c > 0) return Ext80{};
        return Ext80{0xC90FDAA22168C000ull, -62, false};  // FLDPI rounded to 53 bits
    }
    return std::nullopt;
}

int compare(const Ext80& a, double b) {
    const bool a_zero = a.mantissa == 0, b_zero = b == 0;
    if (a_zero && b_zero) return 0;
    const bool a_neg = !a_zero && a.negative, b_neg = !b_zero && b < 0;
    if (a_zero) return b_neg ? 1 : -1;
    if (b_zero) return a_neg ? -1 : 1;
    if (a_neg != b_neg) return a_neg ? -1 : 1;
    std::uint64_t mb;
    int eb;
    split(b, mb, eb);
    while (!(mb >> 63)) {  // normalise to a 64-bit significand like a's
        mb <<= 1;
        --eb;
    }
    int mag = 0;  // compare magnitudes
    if (a.exponent != eb) mag = a.exponent > eb ? 1 : -1;
    else if (a.mantissa != mb) mag = a.mantissa > mb ? 1 : -1;
    return a_neg ? -mag : mag;
}

std::int32_t atan2_scaled(std::int32_t y, std::int32_t x) {
    double k;
    std::memcpy(&k, &kAtanScaleBits, 8);
    if (y == 0 && x >= 0) return 0;
    // Fast path: far from a rounding tie, a double evaluation gives the same integer.
    const double v = std::atan2(static_cast<double>(y), static_cast<double>(x)) * k;
    const double frac = v - std::floor(v);
    if (std::fabs(frac - 0.5) > 1e-7) return fist_nearest(v);
    // Exact path: 64-bit FPATAN result times k rounded to 53 bits.
    const Ext80 r = fpatan(y, x);
    std::uint64_t mk;
    int ek;
    split(k, mk, ek);
    int e = r.exponent + ek;
    const std::uint64_t m = round_bits(mul64x64(r.mantissa, mk), e, 53, false);
    const double p = std::ldexp(static_cast<double>(m), e);
    return fist_nearest(r.negative ? -p : p);
}

} // namespace sim::x87
