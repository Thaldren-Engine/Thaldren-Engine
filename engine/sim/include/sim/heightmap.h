#pragma once
// Terrain height grid: one height byte per 16x16 world-unit cell.
// (see docs/RE_MEMORY.md, "Heightmap").

#include <cstdint>
#include <vector>

namespace sim {

class HeightMap {
public:
    HeightMap() = default;
    HeightMap(std::int32_t width, std::int32_t height) : width_(width), height_(height), cells_(static_cast<std::size_t>(width) * height) {}

    std::int32_t width() const { return width_; }    // cells
    std::int32_t height() const { return height_; }  // cells
    std::uint8_t& at(std::int32_t x, std::int32_t z) { return cells_[static_cast<std::size_t>(z) * width_ + x]; }
    std::uint8_t at(std::int32_t x, std::int32_t z) const { return cells_[static_cast<std::size_t>(z) * width_ + x]; }

    // Height byte of a cell, 0 outside the map.
    std::uint8_t cell_height(std::int32_t x, std::int32_t z) const;

    // Bilinear height at a world position given in whole world units (the
    // integer parts of 16.16 coordinates, as 16-bit values). Returns -1 when the
    // cell or its +1 neighbours fall outside the map.
    std::int32_t sample(std::int16_t world_x, std::int16_t world_z) const;

private:
    std::int32_t width_ = 0, height_ = 0;
    std::vector<std::uint8_t> cells_;
};

} // namespace sim
