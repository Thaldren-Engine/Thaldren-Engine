#pragma once
// Decimal string -> double conversion reproducing the 1998 Microsoft C runtime
// used by TotalA.exe bit for bit, including its quirks:
//   * at most 24 significant digits are used (extra digits are truncated, not rounded);
//   * scaling uses 80-bit intermediates multiplied by tabled powers of ten in
//     octal-digit steps, with truncated partial products;
//   * the final rounding to 53 bits ignores one sticky bit and rounds exact
//     halves down.
// See docs/RE_MEMORY.md, "atof".

#include <array>
#include <cstdint>
#include <string_view>

namespace ta {

// 80-bit mantissa + sign/exponent, little endian, as the runtime stores it:
// bytes 0-9 mantissa (bit 79 = explicit integer bit), bytes 10-11 sign | biased exponent.
using Ld12 = std::array<std::uint8_t, 12>;

// String -> 12-byte intermediate. Returns the runtime's flags
// (1 = underflow, 2 = overflow, 4 = no digits).
int crt_string_to_ld12(std::string_view s, Ld12& out);

// 12-byte intermediate -> IEEE double bits.
std::uint64_t crt_ld12_to_double_bits(const Ld12& v);

// 12-byte intermediate -> IEEE float bits, rounded straight from the 64-bit mantissa with
// the same rules (_ld12tof, 0x4F39B0). Not the same as rounding the double to a float.
std::uint32_t crt_ld12_to_float_bits(const Ld12& v);

// atof(): leading C-locale whitespace, then the conversion above.
double crt_atof(std::string_view s);

// sscanf "%f" into a float (_fassign -> _atoflt, 0x4F3AD0) of the characters the scanner
// collected: string -> 12-byte intermediate -> float directly.
float crt_scanf_float(std::string_view s);

// Power-of-ten table entry exactly as stored by the runtime (exposed for tests).
Ld12 crt_pow10_entry(int power);

} // namespace ta
