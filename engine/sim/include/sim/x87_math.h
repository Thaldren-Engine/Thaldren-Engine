#pragma once
// Exact emulation of the simulation's other x87 arithmetic: the 1998 C runtime
// _hypot (64-bit precision internally, results double-rounded into doubles)
// and the FPATAN-based scaled atan2. Portable; no x87 needed at run time.

#include <cstdint>
#include <optional>

#include "sim/x87_trig.h"

namespace sim::x87 {

// IEEE operations on doubles rounded to a 64-bit significand (x87 precision
// control 64, round to nearest even); finite non-zero results only (zeros pass through).
Ext80 div64(double a, double b);
Ext80 mul64(double a, double b);
Ext80 add64(const Ext80& a, const Ext80& b);
Ext80 sqrt64(double a);  // a >= 0
// Store to a double (round to nearest even 53-bit significand).
double to_double(const Ext80& v);

// The C runtime's _hypot(x, y) as linked into the original (finite inputs,
// no overflow/underflow paths).
double crt_hypot(double x, double y);

// FPATAN(y over x) = atan2(y, x) rounded to 64 bits (nearest even).
Ext80 fpatan(std::int32_t y, std::int32_t x);
// With `bits` = 53: the result under precision control 53, the 64-bit result rounded
// again to 53 bits. (The hardware's own 64-bit rounding is wrong when the exact value
// lies within about 1/8 unit of a 64-bit halfway point; only then can it differ.)
Ext80 fpatan(double y, double x, int bits = 64);  // finite, |y|, |x| below 2^62
// The exact FPATAN value: its top 128 bits (truncated) and the position of the top bit.
void fpatan_debug(double y, double x, std::uint64_t& hi, std::uint64_t& lo, int& top);

// The 1998 C runtime acos(c) as linked into the original, at the game's precision
// control 53: for |c| < 1, FPATAN(sqrt((1 + c)(1 - c)), c) with every step rounded
// to a double (the FPATAN result too); 0 for c = 1 and pi (FLDPI, rounded) for c = -1;
// NaN (nullopt) otherwise.
std::optional<Ext80> crt_acos(double c);

// Exact comparison of an extended value with a double: -1, 0 or 1.
int compare(const Ext80& a, double b);

// The original's angle helper: FISTP(round53(FPATAN(y, x) * k)), k = 65536/(2 pi)
// as stored by the original (0x40C45F306DC9C78A).
constexpr std::uint64_t kAtanScaleBits = 0x40C45F306DC9C78Aull;
std::int32_t atan2_scaled(std::int32_t y, std::int32_t x);

} // namespace sim::x87
