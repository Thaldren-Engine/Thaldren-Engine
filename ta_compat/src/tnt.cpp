#include "ta/tnt.h"

#include <cstring>

namespace ta {

// Header (u32): version, width, height, map data offset, attribute offset,
// tile graphics offset, tile count, feature count, feature table offset, then
// (0x2000) sea level, minimap offset, flags. Attribute cells follow row by row:
// 4 bytes each in 0x2000 maps (height, u16 feature, pad) and 8 bytes each in
// 0x1020 maps (height first). The game copies each height byte into its map cell.
bool tnt_load_heights(const std::vector<std::uint8_t>& tnt, sim::HeightMap& heights, TntInfo* info) {
    if (tnt.size() < 64) return false;
    std::uint32_t h[16];
    std::memcpy(h, tnt.data(), sizeof h);
    TntInfo t;
    t.version = h[0];
    if (t.version != 0x2000 && t.version != 0x1020) return false;
    t.width = static_cast<std::int32_t>(h[1]);
    t.height = static_cast<std::int32_t>(h[2]);
    t.tile_count = h[6];
    t.feature_count = h[7];
    t.sea_level = t.version == 0x2000 ? static_cast<std::int32_t>(h[9]) : 0;
    const std::size_t stride = t.version == 0x2000 ? 4 : 8;
    if (t.width <= 0 || t.height <= 0) return false;
    const std::size_t cells = static_cast<std::size_t>(t.width) * static_cast<std::size_t>(t.height);
    if (h[4] > tnt.size() || (tnt.size() - h[4]) / stride < cells) return false;
    heights = sim::HeightMap(t.width, t.height);
    for (std::int32_t z = 0; z < t.height; ++z)
        for (std::int32_t x = 0; x < t.width; ++x)
            heights.at(x, z) = tnt[h[4] + (static_cast<std::size_t>(z) * static_cast<std::size_t>(t.width) + static_cast<std::size_t>(x)) * stride];
    if (info) *info = t;
    return true;
}

} // namespace ta
