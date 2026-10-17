#include "sim/x87_trig.h"

#include "bigfix.h"

#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>

namespace sim::x87 {
using namespace detail;
namespace {

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

namespace {

struct CacheEntry {
    Ext80 s, c;
    bool ready = false;
};

FpuProfile g_profile = FpuProfile::Model66Pi;
std::vector<CacheEntry> g_cache(65536);          // per angle, for the active profile
std::vector<std::array<Ext80, 2>> g_captured;    // per angle (uint16 index), from a capture file

Ext80 ext_from_tbyte(const std::uint8_t* b) {
    Ext80 v;
    std::memcpy(&v.mantissa, b, 8);
    std::uint16_t se;
    std::memcpy(&se, b + 8, 2);
    v.negative = (se & 0x8000) != 0;
    v.exponent = static_cast<int>(se & 0x7FFF) - 16383 - 63;
    if (v.mantissa == 0) v = Ext80{};
    return v;
}

} // namespace

FpuProfile fpu_profile() { return g_profile; }

bool set_fpu_profile(FpuProfile profile) {
    if (profile == FpuProfile::Captured && g_captured.empty()) return false;
    if (profile != g_profile) {
        g_profile = profile;
        for (auto& e : g_cache) e.ready = false;
    }
    return true;
}

// Capture file (tools/fpu_capture): 64-byte header
//   "FPUCAP1" NUL, u32 angle count (65535), u32 CPUID(1).EAX, char vendor[12], u32 has_cpuid, 32 reserved
// then for angle = -32768..32767 except 0: 10-byte FSINCOS sine, 10-byte cosine (x87 tbyte).
bool load_fsincos_capture(const std::uint8_t* data, std::size_t size, std::string* cpu_description) {
    constexpr std::size_t kHeader = 64, kCount = 65535;
    if (size != kHeader + kCount * 20 || std::memcmp(data, "FPUCAP1", 8) != 0) return false;
    std::uint32_t count, signature, has_cpuid;
    std::memcpy(&count, data + 8, 4);
    std::memcpy(&signature, data + 12, 4);
    std::memcpy(&has_cpuid, data + 28, 4);
    if (count != kCount) return false;
    std::vector<std::array<Ext80, 2>> table(65536);
    table[0][0] = Ext80{};                                 // sin 0 (angle 0 is never rotated)
    table[0][1] = Ext80{0x8000000000000000ull, -63, false}; // cos 0 = 1
    const std::uint8_t* p = data + kHeader;
    for (int angle = -32768; angle <= 32767; ++angle) {
        if (angle == 0) continue;
        auto& t = table[static_cast<std::uint16_t>(angle)];
        t[0] = ext_from_tbyte(p);
        t[1] = ext_from_tbyte(p + 10);
        p += 20;
    }
    g_captured = std::move(table);
    if (cpu_description) {
        if (has_cpuid) {
            const std::string vendor(reinterpret_cast<const char*>(data + 16), 12);
            // Display family/model include the extended fields (family 6 and 15).
            unsigned family = (signature >> 8) & 0xF, model = (signature >> 4) & 0xF;
            if (family == 15) family += (signature >> 20) & 0xFF;
            if (family == 6 || family >= 15) model += ((signature >> 16) & 0xF) << 4;
            *cpu_description = vendor + " family " + std::to_string(family) + " model " + std::to_string(model) +
                               " stepping " + std::to_string(signature & 0xF);
        } else {
            *cpu_description = "no CPUID (386/486-class)";
        }
    }
    if (g_profile == FpuProfile::Captured)
        for (auto& e : g_cache) e.ready = false;
    return true;
}

void sincos_for_angle(std::int16_t angle, Ext80& sine, Ext80& cosine) {
    // Computed once per angle on first use (the series is slow); not thread-safe.
    CacheEntry& e = g_cache[static_cast<std::uint16_t>(angle)];
    if (!e.ready) {
        if (g_profile == FpuProfile::Captured) {
            e.s = g_captured[static_cast<std::uint16_t>(angle)][0];
            e.c = g_captured[static_cast<std::uint16_t>(angle)][1];
        } else {
            sincos_x87_model(static_cast<double>(angle) * angle_step(), e.s, e.c);
            if (g_profile == FpuProfile::IntelP6) {
                for (const std::uint32_t x : kExceptions) {
                    if (x == 0xFFFFFFFFu || static_cast<std::uint16_t>(x) != static_cast<std::uint16_t>(angle)) continue;
                    Ext80& v = (x >> 16) & 1 ? e.c : e.s;
                    const int delta = static_cast<int>((x >> 17) & 0xFF) - 128;
                    v.mantissa += static_cast<std::uint64_t>(static_cast<std::int64_t>(delta));
                }
            }
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
    // Keep the top 53 bits, round to nearest even on the rest. The product is below 2^96
    // (|x| <= 2^31, mantissa < 2^64), so at most 43 low bits are dropped: every bit that
    // goes lies in the low word, and the shifts below stay in range.
    const int width = hi ? 128 - std::countl_zero(hi) : 64 - std::countl_zero(lo);  // significant bits
    const int drop = width > 53 ? width - 53 : 0;
    std::uint64_t keep;
    bool half = false, sticky = false;
    if (drop == 0) {
        keep = lo;
    } else {
        keep = (lo >> drop) | (hi << (64 - drop));
        half = ((lo >> (drop - 1)) & 1u) != 0;
        sticky = (lo & ((1ull << (drop - 1)) - 1u)) != 0;
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
    const double f = std::floor(v);
    const double diff = v - f;  // exact for these magnitudes
    std::int64_t r = static_cast<std::int64_t>(f);  // exact: |f| < 2^32
    if (diff > 0.5 || (diff == 0.5 && (r & 1) != 0)) ++r;
    if (r > INT32_MAX || r < INT32_MIN) return INT32_MIN;  // the integer indefinite
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
