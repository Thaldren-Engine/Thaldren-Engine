#include "ta/net_setup.h"

#include "ta/tdf.h"
#include "ta/translate.h"

namespace ta {

std::vector<std::pair<std::uint32_t, std::uint32_t>> unit_sync_checks(Vfs& vfs, const UnitDefTable& defs) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    std::vector<std::uint8_t> bytes;
    auto sum_of = [&](const std::string& path) -> std::uint32_t {
        bytes.clear();
        if (!vfs.read(path, bytes) || bytes.empty()) return 0;
        return simple_checksum(bytes.data(), static_cast<std::int32_t>(bytes.size()));
    };
    for (std::size_t k = 1; k < defs.units.size(); ++k) {
        const UnitDefRecord& d = defs.units[k];
        std::uint32_t v = 0;
        v ^= sum_of("scripts\\" + d.unit_name + ".cob");
        for (const std::string& f : vfs.enumerate("guis\\" + d.unit_name + "*.gui")) {
            const std::size_t dot = f.find_last_of('.');
            v ^= sum_of("guis\\" + (dot == std::string::npos ? f : f.substr(0, dot)) + ".gui");
        }
        v ^= sum_of("download\\" + d.unit_name + ".tdf");
        v ^= d.weapon_hash_xor;
        out.push_back({d.fbi_checksum, v});
    }
    return out;
}

std::uint32_t map_sync_checksum(Vfs& vfs, const std::string& map) {
    std::vector<std::uint8_t> tnt;
    const std::string file = map_file_name(vfs, map);  // (the map info's name, 0x435DA0)
    if (!vfs.read(vfs.virtual_path("maps", file, "tnt"), tnt) || tnt.size() < 0x40) return 0;
    auto u32 = [&](std::size_t at) -> std::uint32_t {
        return at + 4 <= tnt.size() ? static_cast<std::uint32_t>(tnt[at]) | (static_cast<std::uint32_t>(tnt[at + 1]) << 8) |
                                          (static_cast<std::uint32_t>(tnt[at + 2]) << 16) | (static_cast<std::uint32_t>(tnt[at + 3]) << 24)
                                    : 0u;
    };
    if (u32(0) != 0x2000) return 0;
    auto sum = [&](std::size_t at, std::size_t n) -> std::uint32_t {
        if (at > tnt.size()) return simple_checksum(nullptr, 0);
        if (n > tnt.size() - at) n = tnt.size() - at;  // the original reads past the end of the file (short read)
        return simple_checksum(tnt.data() + at, static_cast<std::int32_t>(n));
    };
    std::uint32_t v = sum(0, 0x40);
    v ^= sum(u32(0x10), static_cast<std::size_t>(u32(4)) * u32(8) * 4);
    const std::size_t features = static_cast<std::size_t>(u32(0x1C)) * 0x84;
    if (static_cast<std::int32_t>(u32(0x1C) * 0x84) > 0) v ^= sum(u32(0x20), features);
    std::vector<std::uint8_t> ota;
    if (vfs.read(vfs.virtual_path("maps", file, "ota"), ota)) {
        const TdfParseResult r = tdf_parse(std::string_view(reinterpret_cast<const char*>(ota.data()), ota.size()));
        if (const TdfSection* gh = r.root ? r.root->child("GlobalHeader") : nullptr) v ^= gh->checksum();
    }
    return v;
}

void set_network_weapons(sim::Battle& battle, const std::vector<sim::WeaponType>& weapons) {
    auto& w = battle.net_state().weapons;
    for (const sim::WeaponType& wt : weapons) {
        if (w.size() <= wt.index) w.resize(static_cast<std::size_t>(wt.index) + 1, nullptr);
        if (!w[wt.index]) w[wt.index] = &wt;
    }
}

}  // namespace ta
