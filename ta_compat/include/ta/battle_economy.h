#pragma once
// The map's economy for a battle (sim/economy.h): metal in the ground, wind range and
// tidal strength, set up the way the original loads a map (see docs/RE_MEMORY.md, "Economy").

#include <cstdint>
#include <string>
#include <vector>

#include "sim/battle.h"
#include "ta/featuredefs.h"

namespace ta {

// Values from the map's OTA file. The original reads the wind and tide keys from
// [GlobalHeader] and SurfaceMetal from the schema the game selects (a [Schema N] section in
// it). Without an OTA every value stays -1 and the TNT's defaults apply.
struct MapEconomy {
    std::int32_t surface_metal = -1;
    std::int32_t min_wind = -1, max_wind = -1;
    float tidal_strength = -1.0f;
};
// `schema`: index N of the [Schema N] section to read SurfaceMetal from. False if the file
// has no [GlobalHeader].
bool read_map_economy(const std::vector<std::uint8_t>& ota, int schema, MapEconomy& out);

// Fills the battle's map metal, wind range and tidal strength (0x483610's economy parts)
// and stamps indestructible features' metal (0x422040). Call after the map's features are
// placed (place_tnt_features). The wind starts calm with its first change due at once;
// call Battle::step_wind once at the loading step as the original does.
void init_map_economy(sim::Battle& b, const std::vector<std::uint8_t>& tnt, const MapEconomy& m, const FeatureDefTable& features);

} // namespace ta
