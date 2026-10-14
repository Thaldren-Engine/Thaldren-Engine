#pragma once
// Bit-exact emulation of the simulation's one x87 transcendental: a 2D point
// rotation computed with FSINCOS on extended precision, products and sums
// rounded to 53 bits (precision control 53), and a round-to-nearest-even
// integer store. Portable: no x87 needed at run time.

#include <cstdint>

namespace sim::x87 {

// An x87 extended value: (-1)^negative * mantissa * 2^exponent (mantissa has its top bit set unless zero).
struct Ext80 {
    std::uint64_t mantissa = 0;
    std::int32_t exponent = 0;
    bool negative = false;
};

// Angle unit used by the rotation: radians per 1/65536 turn as stored by the
// original (close to, but not exactly, 2*pi/65536).
constexpr std::uint64_t kAngleStepBits = 0x3F1921FB54442C5Aull;
double angle_step();

// FSINCOS results for the argument (double)angle * angle_step(), rounded to
// 64-bit mantissas like the hardware this was verified on.
void sincos_for_angle(std::int16_t angle, Ext80& sine, Ext80& cosine);

// Correctly rounded 64-bit-mantissa sin/cos of an exact double (|a| <= 4).
void sincos_correctly_rounded(double a, Ext80& sine, Ext80& cosine);
// Model of FSINCOS: argument reduced exactly by the FPU's 66-bit pi/2, then the
// result correctly rounded (|a| <= 4). The hardware differs from this only in rare last bits.
void sincos_x87_model(double a, Ext80& sine, Ext80& cosine);

// x * v rounded to 53 significant bits (nearest even), as an exact double.
double mul_pc53(std::int32_t x, const Ext80& v);

// FISTP with round-to-nearest-even; out-of-range or NaN gives INT32_MIN.
std::int32_t fist_nearest(double v);

// Rotates (x, y) by `angle` (1/65536 turns): x' = x cos - y sin, y' = y cos + x sin.
// angle == 0 leaves the point unchanged (the original skips the FPU work).
void rotate_point_2d(std::int16_t angle, std::int32_t& x, std::int32_t& y);

// Rotates a 3D point: the (x, y) pair by angles[0], then (y, z) by angles[2],
// then (x, z) by angles[1].
void rotate_point_3d(const std::int32_t in[3], std::int32_t out[3], const std::int16_t angles[3]);

} // namespace sim::x87
