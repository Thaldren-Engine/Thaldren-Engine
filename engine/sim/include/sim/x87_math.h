#pragma once
// Exact emulation of the simulation's other x87 arithmetic: the 1998 C runtime
// _hypot (64-bit precision internally, results double-rounded into doubles)
// and the FPATAN-based scaled atan2. Portable; no x87 needed at run time.

#include <cstdint>

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

// The original's angle helper: FISTP(round53(FPATAN(y, x) * k)), k = 65536/(2 pi)
// as stored by the original (0x40C45F306DC9C78A).
constexpr std::uint64_t kAtanScaleBits = 0x40C45F306DC9C78Aull;
std::int32_t atan2_scaled(std::int32_t y, std::int32_t x);

} // namespace sim::x87
