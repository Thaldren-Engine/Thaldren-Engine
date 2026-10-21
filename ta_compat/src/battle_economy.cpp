#include "ta/battle_economy.h"

#include <cstring>

#include "sim/economy.h"
#include "sim/x87.h"
#include "ta/tdf.h"

namespace ta {
namespace {

template <class T> bool rd(const std::vector<std::uint8_t>& b, std::size_t at, T& v) {
    if (at + sizeof(T) > b.size()) return false;
    std::memcpy(&v, &b[at], sizeof(T));
    return true;
}

} // namespace

bool read_map_economy(const std::vector<std::uint8_t>& ota, int schema, MapEconomy& out) {
    const TdfParseResult parsed = tdf_parse(std::string_view(reinterpret_cast<const char*>(ota.data()), ota.size()));
    if (!parsed.root) return false;
    const TdfSection* header = parsed.root->child("GlobalHeader");
    if (!header) return false;
    out.min_wind = header->get_int("minwindspeed", 0);
    out.max_wind = header->get_int("maxwindspeed", 0);
    out.tidal_strength = static_cast<float>(header->get_float("tidalstrength", 0.0));
    if (const TdfSection* s = header->child("Schema " + std::to_string(schema))) out.surface_metal = s->get_int("SurfaceMetal", 0);
    return true;
}

void init_map_economy(sim::Battle& b, const std::vector<std::uint8_t>& tnt, const MapEconomy& m, const FeatureDefTable& features) {
    std::uint32_t h[12] = {};
    for (int i = 0; i < 12; ++i)
        if (!rd(tnt, static_cast<std::size_t>(i) * 4, h[i])) return;
    const std::uint32_t version = h[0];
    const bool new_format = version >= 0x2000;
    // Wind range and tide: the OTA's values unless missing (negative); older maps keep the
    // TNT header's wind, newer ones default to 100..2000; the tide defaults to 0.5.
    const std::int32_t tnt_min = version == 0x1020 ? static_cast<std::int32_t>(h[10]) : 100;
    const std::int32_t tnt_max = version == 0x1020 ? static_cast<std::int32_t>(h[11]) : 2000;
    b.wind.min_speed = (m.min_wind < 0 || !new_format) ? tnt_min : m.min_wind;
    b.wind.max_speed = (m.max_wind < 0 || !new_format) ? tnt_max : m.max_wind;
    // (0x483854: "test ah,1", so an unordered value also takes the default.)
    b.settings.tidal_strength = sim::x87::less(m.tidal_strength, 0.0) ? 0.5f : m.tidal_strength;
    b.settings.surface_metal = m.surface_metal;  // the map info's value (the computer player's site choice)
    b.wind.denominator = 5000;
    b.wind.next_change_tick = 0;
    // Metal in the ground: the schema's surface metal everywhere (newer maps), or each
    // cell's own attribute byte (0x1020 maps).
    const std::size_t cells = b.map.cells.size();
    const auto surface = static_cast<std::uint8_t>((m.surface_metal < 0 || !new_format) ? 0 : m.surface_metal);
    for (sim::MapCell& c : b.map.cells) c.metal = surface;
    if (version == 0x1020)
        for (std::size_t i = 0; i < cells; ++i) rd(tnt, h[4] + i * 8 + 6, b.map.cells[i].metal);
    // Indestructible features with metal (metal patches) stamp it under their footprint.
    std::vector<float> metal;
    metal.reserve(features.records().size());
    for (const FeatureDefRecord& r : features.records()) metal.push_back(r.metal);
    sim::stamp_feature_metal(b.map, b.features, metal);
}

} // namespace ta
