#include "ta/crt_strtod.h"

#include <cstring>

namespace ta {
namespace {

constexpr std::uint8_t kPow10[58][12] = {
#include "pow10_table.inc"
};

// ---- little-endian field access on the 12-byte value ---------------------------------
std::uint16_t get16(const std::uint8_t* p, int off) { return static_cast<std::uint16_t>(p[off] | (p[off + 1] << 8)); }
std::uint32_t get32(const std::uint8_t* p, int off) {
    return static_cast<std::uint32_t>(p[off]) | (static_cast<std::uint32_t>(p[off + 1]) << 8) |
           (static_cast<std::uint32_t>(p[off + 2]) << 16) | (static_cast<std::uint32_t>(p[off + 3]) << 24);
}
void put16(std::uint8_t* p, int off, std::uint16_t v) {
    p[off] = static_cast<std::uint8_t>(v);
    p[off + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, int off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// 96-bit integer as three little-endian dwords (d[0] lowest).
struct U96 {
    std::uint32_t d[3] = {0, 0, 0};
};

bool add32(std::uint32_t a, std::uint32_t b, std::uint32_t& out) {
    out = a + b;
    return out < a || out < b;
}

void u96_add(U96& a, const U96& b) {
    if (add32(a.d[0], b.d[0], a.d[0])) {
        if (add32(a.d[1], 1, a.d[1])) ++a.d[2];
    }
    if (add32(a.d[1], b.d[1], a.d[1])) ++a.d[2];
    add32(a.d[2], b.d[2], a.d[2]);
}

void u96_shl1(U96& a) {
    a.d[2] = (a.d[2] << 1) | (a.d[1] >> 31);
    a.d[1] = (a.d[1] << 1) | (a.d[0] >> 31);
    a.d[0] <<= 1;
}

void u96_shr1(U96& a) {
    a.d[0] = (a.d[0] >> 1) | (a.d[1] << 31);
    a.d[1] = (a.d[1] >> 1) | (a.d[2] << 31);
    a.d[2] >>= 1;
}

Ld12 to_ld12(const U96& v) {
    Ld12 r{};
    for (int i = 0; i < 3; ++i) put32(r.data(), 4 * i, v.d[i]);
    return r;
}
U96 from_ld12(const Ld12& v) {
    U96 r;
    for (int i = 0; i < 3; ++i) r.d[i] = get32(v.data(), 4 * i);
    return r;
}

// Decimal digits (values 0-9) -> normalized 12-byte value.
Ld12 digits_to_ld12(const char* digits, int n) {
    U96 m;
    std::uint16_t exp = 0x404E;
    for (int i = 0; i < n; ++i) {
        U96 t = m;
        u96_shl1(m);
        u96_shl1(m);
        u96_add(m, t);
        u96_shl1(m);
        U96 d;
        d.d[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<signed char>(digits[i])));
        u96_add(m, d);
    }
    while (m.d[2] == 0) {
        exp = static_cast<std::uint16_t>(exp - 16);
        m.d[2] = m.d[1] >> 16;
        m.d[1] = (m.d[0] >> 16) | (m.d[1] << 16);
        m.d[0] <<= 16;
    }
    while ((m.d[2] & 0x8000) == 0) {
        u96_shl1(m);
        exp = static_cast<std::uint16_t>(exp - 1);
    }
    Ld12 r = to_ld12(m);
    put16(r.data(), 10, exp);
    return r;
}

void set_inf(Ld12& a, std::uint32_t sign) {
    put32(a.data(), 0, 0);
    put32(a.data(), 4, 0);
    put32(a.data(), 8, (sign ? 0x80000000u : 0u) + 0x7FFF8000u);
}

// a *= b on 12-byte values, 16-bit partial products, truncated below the guard word.
void ld12_mul(Ld12& a, const Ld12& b) {
    const std::uint16_t ae = get16(a.data(), 10), be = get16(b.data(), 10);
    const std::uint32_t sign = (ae ^ be) & 0x8000u;
    const std::uint32_t ea = ae & 0x7FFFu, eb = be & 0x7FFFu;
    std::uint32_t ex = ea + eb;
    if (ea >= 0x7FFF || eb >= 0x7FFF || (ex & 0xFFFF) > 0xBFFD) {
        set_inf(a, sign);
        return;
    }
    if ((ex & 0xFFFF) <= 0x3FBF) {
        a.fill(0);
        return;
    }
    if (ea == 0) {
        ++ex;
        if ((get32(a.data(), 8) & 0x7FFFFFFFu) == 0 && get32(a.data(), 4) == 0 && get32(a.data(), 0) == 0) {
            put16(a.data(), 10, 0);
            return;
        }
    }
    if (eb == 0) {
        ++ex;
        if ((get32(b.data(), 8) & 0x7FFFFFFFu) == 0 && get32(b.data(), 4) == 0 && get32(b.data(), 0) == 0) {
            a.fill(0);
            return;
        }
    }

    std::uint8_t r[12] = {};
    for (int i = 0; i < 5; ++i) {
        for (int k = 0; i + k < 5; ++k) {
            const std::uint32_t prod = static_cast<std::uint32_t>(get16(a.data(), 2 * (i + k))) *
                                       static_cast<std::uint32_t>(get16(b.data(), 2 * (4 - k)));
            std::uint32_t sum = 0;
            const bool carry = add32(get32(r, 2 * i), prod, sum);
            put32(r, 2 * i, sum);
            if (carry) put16(r, 2 * i + 4, static_cast<std::uint16_t>(get16(r, 2 * i + 4) + 1));
        }
    }

    std::uint16_t e = static_cast<std::uint16_t>(ex + 0xC002u);
    U96 v;
    for (int i = 0; i < 3; ++i) v.d[i] = get32(r, 4 * i);
    while (static_cast<std::int16_t>(e) > 0 && (v.d[2] & 0x80000000u) == 0) {
        u96_shl1(v);
        e = static_cast<std::uint16_t>(e - 1);
    }
    if (static_cast<std::int16_t>(e) <= 0) {
        e = static_cast<std::uint16_t>(e - 1);
        if (static_cast<std::int16_t>(e) < 0) {
            int count = -static_cast<std::int16_t>(e);
            e = 0;
            int sticky = 0;
            while (count--) {
                if (v.d[0] & 1u) ++sticky;
                u96_shr1(v);
            }
            if (sticky) v.d[0] |= 1u;
        }
    }
    for (int i = 0; i < 3; ++i) put32(r, 4 * i, v.d[i]);

    // Round half to even on the low 16-bit guard word.
    if (get16(r, 0) > 0x8000 || (get32(r, 0) & 0x1FFFFu) == 0x18000u) {
        if (get32(r, 2) != 0xFFFFFFFFu) {
            put32(r, 2, get32(r, 2) + 1);
        } else {
            put32(r, 2, 0);
            if (get32(r, 6) != 0xFFFFFFFFu) {
                put32(r, 6, get32(r, 6) + 1);
            } else {
                put32(r, 6, 0);
                if (get16(r, 10) != 0xFFFF) {
                    put16(r, 10, static_cast<std::uint16_t>(get16(r, 10) + 1));
                } else {
                    put16(r, 10, 0x8000);
                    e = static_cast<std::uint16_t>(e + 1);
                }
            }
        }
    }
    if (e >= 0x7FFF) {
        set_inf(a, sign);
        return;
    }
    put16(a.data(), 0, get16(r, 2));
    put32(a.data(), 2, get32(r, 4));
    put32(a.data(), 6, get32(r, 8));
    put16(a.data(), 10, static_cast<std::uint16_t>(e | sign));
}

void mul_pow10(Ld12& v, int power) {
    if (power == 0) return;
    int table = 0;  // 0 = positive, 1 = negative
    if (power < 0) {
        power = -power;
        table = 1;
    }
    put16(v.data(), 0, 0);  // the low guard word is discarded first
    int group = 0;
    while (power != 0) {
        const int digit = power & 7;
        power >>= 3;
        if (digit != 0) {
            Ld12 p;
            std::memcpy(p.data(), kPow10[table * 29 + group * 7 + digit - 1], 12);
            if (get16(p.data(), 0) > 0x7FFF) {
                // The tabled 64-bit part was rounded up; undo that before multiplying.
                put32(p.data(), 2, get32(p.data(), 2) - 1);
            }
            ld12_mul(v, p);
        }
        ++group;
    }
}

// ---- rounding to IEEE double ---------------------------------------------------------
// Mantissa as three big-endian-ordered dwords (m[0] most significant); bit index 0 = MSB.
struct Man {
    std::uint32_t m[3];
};

bool tail_zero_after(const Man& x, int bit) {
    // Checks bits strictly after `bit` within its dword, then all following dwords.
    const int w = bit / 32;
    const std::uint32_t mask = ~(~0u << (31 - (bit % 32)));
    if (x.m[w] & mask) return false;
    for (int i = w + 1; i < 3; ++i)
        if (x.m[i]) return false;
    return true;
}

bool add_one_at(Man& x, int bit) {
    int w = bit / 32;
    std::uint32_t out = 0;
    bool carry = add32(x.m[w], 1u << (31 - (bit % 32)), out);
    x.m[w] = out;
    for (--w; w >= 0 && carry; --w) {
        carry = add32(x.m[w], 1u, out);
        x.m[w] = out;
    }
    return carry;
}

// Round to `prec` bits: up only if the first dropped bit is set and some bit after
// the NEXT one is set (bit prec+1 is never examined). Then clear from bit prec+1 on.
bool round_man(Man& x, int prec) {
    const int w = prec / 32;
    const int sh = 31 - (prec % 32);
    bool carry = false;
    if ((x.m[w] & (1u << sh)) && !tail_zero_after(x, prec + 1)) carry = add_one_at(x, prec - 1);
    x.m[w] &= ~0u << sh;
    for (int i = w + 1; i < 3; ++i) x.m[i] = 0;
    return carry;
}

void shr_man(Man& x, int n) {
    const int words = n / 32;
    const int bits = n % 32;
    std::uint32_t carry_in = 0;
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t v = x.m[i];
        x.m[i] = (bits ? (v >> bits) : v) | carry_in;
        carry_in = bits ? ((v & ~(~0u << bits)) << (32 - bits)) : 0;
    }
    for (int i = 2; i >= 0; --i) x.m[i] = (i < words) ? 0 : x.m[i - words];
}

} // namespace

Ld12 crt_pow10_entry(int power) {
    const int table = power < 0 ? 1 : 0;
    int p = power < 0 ? -power : power;
    int group = 0;
    while (p >= 8) {
        if (p & 7) return Ld12{};  // not a single-digit octal power
        p >>= 3;
        ++group;
    }
    Ld12 r{};
    if (p >= 1) std::memcpy(r.data(), kPow10[table * 29 + group * 7 + p - 1], 12);
    return r;
}

int crt_string_to_ld12(std::string_view s, Ld12& out) {
    std::size_t i = 0;
    auto at = [&](std::size_t k) -> char { return k < s.size() ? s[k] : '\0'; };
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    auto is_19 = [](char c) { return c >= '1' && c <= '9'; };
    while (at(i) == ' ' || at(i) == '\t' || at(i) == '\n' || at(i) == '\r') ++i;

    char digits[25] = {};
    int n = 0;            // stored significant digits
    int dec_exp = 0;      // decimal exponent adjustment from digit positions
    int exp_val = 0;      // explicit exponent value
    int exp_sign = 1;
    std::uint16_t sign = 0;
    bool any_digit = false;   // mantissa digits seen (zeros included)

    enum State { Start, Zeros, AfterSign, Int, Frac, LeadPoint, ExpMark, ExpSign, ExpZeros, ExpDigits, Done };
    State st = Start;
    while (st != Done) {
        const char c = at(i);
        switch (st) {
        case Start:
            if (is_19(c)) { st = Int; break; }
            ++i;
            if (c == '.') st = LeadPoint;
            else if (c == '+') { st = AfterSign; sign = 0; }
            else if (c == '-') { st = AfterSign; sign = 0x8000; }
            else if (c == '0') st = Zeros;
            else st = Done;
            break;
        case Zeros:
            any_digit = true;
            if (is_19(c)) { st = Int; break; }
            ++i;
            if (c == '.') st = Frac;
            else if (c == '0') st = Zeros;
            else if (c == 'D' || c == 'E' || c == 'd' || c == 'e') st = ExpMark;
            else st = Done;
            break;
        case AfterSign:
            if (is_19(c)) { st = Int; break; }
            ++i;
            if (c == '.') st = LeadPoint;
            else if (c == '0') st = Zeros;
            else st = Done;
            break;
        case Int:
            while (is_digit(at(i))) {
                any_digit = true;
                if (n < 25) digits[n++] = static_cast<char>(at(i) - '0');
                else ++dec_exp;
                ++i;
            }
            {
                const char d = at(i);
                if (d == '.') { ++i; st = Frac; }
                else if (d == 'D' || d == 'E' || d == 'd' || d == 'e') { ++i; any_digit = true; st = ExpMark; }
                else st = Done;
            }
            break;
        case Frac:
            any_digit = true;  // the runtime marks digits as seen on entering this state
            if (n == 0) {
                while (at(i) == '0') { --dec_exp; ++i; }
            }
            while (is_digit(at(i))) {
                any_digit = true;
                if (n < 25) { digits[n++] = static_cast<char>(at(i) - '0'); --dec_exp; }
                ++i;
            }
            {
                const char d = at(i);
                if (d == 'D' || d == 'E' || d == 'd' || d == 'e') { ++i; any_digit = true; st = ExpMark; }
                else st = Done;
            }
            break;
        case LeadPoint:
            st = is_digit(c) ? Frac : Done;
            break;
        case ExpMark:
            if (is_19(c)) { st = ExpDigits; break; }
            ++i;
            if (c == '+') st = ExpSign;
            else if (c == '-') { st = ExpSign; exp_sign = -1; }
            else if (c == '0') st = ExpZeros;
            else st = Done;
            break;
        case ExpSign:
            if (is_19(c)) { st = ExpDigits; break; }
            ++i;
            st = (c == '0') ? ExpZeros : Done;
            break;
        case ExpZeros:
            while (at(i) == '0') ++i;
            st = is_19(at(i)) ? ExpDigits : Done;
            break;
        case ExpDigits:
            exp_val = 0;
            while (is_digit(at(i))) {
                exp_val = exp_val * 10 + (at(i) - '0');
                ++i;
                if (exp_val > 0x1450) {
                    exp_val = 0x1451;
                    while (is_digit(at(i))) ++i;
                    break;
                }
            }
            st = Done;
            break;
        case Done:
            break;
        }
    }

    out.fill(0);
    if (!any_digit) return 4;
    if (n > 24) {
        --n;
        ++dec_exp;
    }
    if (n == 0) {
        put16(out.data(), 10, sign);
        return 0;
    }
    while (digits[n - 1] == 0) {
        --n;
        ++dec_exp;
    }
    out = digits_to_ld12(digits, n);
    const int total = exp_sign * exp_val + dec_exp;
    int flags = 0;
    if (total >= 0x1451) {
        out.fill(0);
        put16(out.data(), 8, 0x8000);  // mantissa 0x8000.. with exponent 0x7FFF
        put16(out.data(), 10, 0x7FFF);
        flags = 2;
    } else if (total <= -0x1451) {
        out.fill(0);
        flags = 1;
    } else {
        mul_pow10(out, total);
    }
    put16(out.data(), 10, static_cast<std::uint16_t>(get16(out.data(), 10) | sign));
    return flags;
}

std::uint64_t crt_ld12_to_double_bits(const Ld12& v) {
    constexpr int kMaxExp = 1024, kMinExp = -1023, kPrec = 53, kExpWidth = 11, kBias = 1023;
    const std::uint16_t se = get16(v.data(), 10);
    Man x{{get32(v.data(), 6), get32(v.data(), 2), static_cast<std::uint32_t>(get16(v.data(), 0)) << 16}};
    const int biased = se & 0x7FFF;
    int e = biased - 0x3FFF;
    if (biased == 0) {
        e = 0;
        x = Man{{0, 0, 0}};
    } else {
        const Man saved = x;
        if (round_man(x, kPrec)) e = biased - 0x3FFE;
        if (e < kMinExp - kPrec) {
            x = Man{{0, 0, 0}};
            e = 0;
        } else if (e <= kMinExp) {
            x = saved;
            shr_man(x, kMinExp - e);
            round_man(x, kPrec);
            shr_man(x, kExpWidth + 1);
            e = 0;
        } else if (e >= kMaxExp) {
            x = Man{{0x80000000u, 0, 0}};
            shr_man(x, kExpWidth);
            e = kBias + kMaxExp;
        } else {
            e += kBias;
            x.m[0] &= 0x7FFFFFFFu;
            shr_man(x, kExpWidth);
        }
    }
    const std::uint32_t hi = (static_cast<std::uint32_t>(e) << (31 - kExpWidth)) | ((se & 0x8000) ? 0x80000000u : 0u) | x.m[0];
    return (static_cast<std::uint64_t>(hi) << 32) | x.m[1];
}

double crt_atof(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))) ++i;
    Ld12 v{};
    if (crt_string_to_ld12(s.substr(i), v) & 4) return 0.0;
    const std::uint64_t bits = crt_ld12_to_double_bits(v);
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

} // namespace ta
