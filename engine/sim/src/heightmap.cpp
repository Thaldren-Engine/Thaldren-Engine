#include "sim/heightmap.h"

namespace sim {
namespace {
// C-style division by 16 (truncates toward zero).
std::int32_t div16(std::int32_t v) { return v / 16; }
}  // namespace

std::uint8_t HeightMap::cell_height(std::int32_t x, std::int32_t z) const {
    if (x < 0 || x >= width_ || z < 0 || z >= height_) return 0;
    return at(x, z);
}

std::int32_t HeightMap::sample(std::int16_t world_x, std::int16_t world_z) const {
    const std::int32_t cx = world_x >> 4, cz = world_z >> 4;
    const std::int32_t fx = world_x & 15, fz = world_z & 15;
    if (cx < 0 || cx + 1 >= width_ || cz < 0 || cz + 1 >= height_) return -1;
    const std::int32_t h00 = at(cx, cz), h10 = at(cx + 1, cz), h01 = at(cx, cz + 1), h11 = at(cx + 1, cz + 1);
    // Interpolate along x on both rows, then along z.
    const std::int32_t top = div16((h10 - h00) * fx) + h00;
    const std::int32_t bottom = div16((h11 - h01) * fx) + h01;
    return div16((bottom - top) * fz) + top;
}

} // namespace sim
