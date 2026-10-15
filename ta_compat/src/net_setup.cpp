#include "ta/net_setup.h"

#include "ta/tdf.h"

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

void set_network_weapons(sim::Battle& battle, const std::vector<sim::WeaponType>& weapons) {
    auto& w = battle.net_state().weapons;
    for (const sim::WeaponType& wt : weapons) {
        if (w.size() <= wt.index) w.resize(static_cast<std::size_t>(wt.index) + 1, nullptr);
        if (!w[wt.index]) w[wt.index] = &wt;
    }
}

}  // namespace ta
