#pragma once
// Weapons: aiming, ballistics, reload and firing (see docs/RE_MEMORY.md, "Weapons").

#include <cstdint>

namespace sim {

// Pitch for a ballistic shot from the weapon to a target offset (dx, dy, dz)
// at muzzle speed `speed` under `gravity`: the higher of the two solutions that
// lies above `min_angle` (radians) and at most 45 degrees. Returns the pitch in
// half-turns of 32768, or -32768 when no solution qualifies. Bit-exact with the
// original's double/x87 evaluation, including its NaN cases (pitch 0).
std::int16_t ballistic_pitch(std::int32_t dx, std::int32_t dy, std::int32_t dz, std::int32_t speed, float min_angle,
                             std::int32_t gravity);

} // namespace sim
