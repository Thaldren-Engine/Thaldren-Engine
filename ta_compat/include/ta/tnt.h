#pragma once
// TNT map files (maps\*.tnt): header and terrain heights
// (see docs/RE_MEMORY.md, "Heightmap").

#include <cstdint>
#include <vector>

#include "sim/heightmap.h"

namespace ta {

struct TntInfo {
    std::uint32_t version = 0;     // 0x2000 (Total Annihilation) or 0x1020
    std::int32_t width = 0;        // height cells (16 world units each)
    std::int32_t height = 0;
    std::int32_t sea_level = 0;    // TA maps; stored as a byte by the game
    std::uint32_t tile_count = 0;
    std::uint32_t feature_count = 0;
};

// Reads the header and fills `heights` with the per-cell height bytes.
bool tnt_load_heights(const std::vector<std::uint8_t>& tnt, sim::HeightMap& heights, TntInfo* info = nullptr);

} // namespace ta
