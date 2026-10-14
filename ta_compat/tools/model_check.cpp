// Compares our 3DO loading with the model trees dumped from the running game
// (ta_capture --dump-defs, MODL/WMDL chunks): structure, names, offsets,
// vertices, primitive order and indices of every unit and weapon model.
//
// Usage: model_check <game_dir> <defs.bin>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "ta/hpi.h"
#include "ta/model3do.h"
#include "ta/unitdefs.h"
#include "ta/weapondefs.h"
#include "model_dump_format.h"

namespace {

std::uint32_t u32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: model_check <game_dir> <defs.bin>\n");
        return 2;
    }
    std::ifstream f(argv[2], std::ios::binary);
    std::vector<std::uint8_t> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::map<std::string, std::vector<std::uint8_t>> ch;
    for (std::size_t p = 0; p + 8 <= all.size();) {
        const std::string tag(reinterpret_cast<const char*>(&all[p]), 4);
        const std::uint32_t n = u32(&all[p + 4]);
        ch[tag].assign(all.begin() + static_cast<std::ptrdiff_t>(p + 8), all.begin() + static_cast<std::ptrdiff_t>(p + 8 + n));
        p += 8 + n;
    }
    if (!ch.count("MODL") || !ch.count("WMDL")) {
        std::fprintf(stderr, "dump lacks MODL/WMDL chunks\n");
        return 1;
    }

    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    ta::UnitDefContext ctx;
    ta::load_unit_context(vfs, ctx);
    const ta::UnitDefTable units = ta::load_unit_defs(vfs, ctx);
    const ta::WeaponDefTable weapons = ta::load_weapon_defs(vfs, ta::WeaponDefOptions{});

    auto expected_for = [&](const std::string& model_name, std::vector<std::uint8_t>& out) -> bool {
        std::string path = "objects3d\\" + model_name;
        const auto dot = path.rfind('.');
        if (dot != std::string::npos && dot > path.rfind('\\')) path.resize(dot);
        path += ".3DO";
        std::vector<std::uint8_t> data;
        if (!vfs.read(path, data)) return false;
        const auto m = ta::load_3do(data, true);
        if (!m.root) return false;
        tools::serialize_model(out, m.root.get());
        return true;
    };

    // Unit models: chunk = u32 count, then per unit: u32 ptr, tree (if ptr).
    std::size_t unit_ok = 0, unit_bad = 0;
    {
        const auto& d = ch["MODL"];
        std::size_t p = 4;
        const std::uint32_t n = u32(&d[0]);
        for (std::uint32_t i = 0; i < n && i < units.units.size(); ++i) {
            const std::uint32_t ptr = u32(&d[p]);
            p += 4;
            if (!ptr) continue;
            std::vector<std::uint8_t> ours;
            const bool have = expected_for(units.units[i].object_name.substr(0, 31), ours);
            const bool same = have && p + ours.size() <= d.size() && std::memcmp(&d[p], ours.data(), ours.size()) == 0;
            if (same) {
                ++unit_ok;
                p += ours.size();
            } else {
                ++unit_bad;
                std::printf("unit %u %s: model differs%s\n", i, units.units[i].unit_name.c_str(), have ? "" : " (ours missing)");
                return 1;  // cannot resynchronise reliably after a mismatch
            }
        }
    }
    // Weapon models: per slot u32 ptr, tree (if ptr). Shared models are dumped once per slot.
    std::size_t weapon_ok = 0, weapon_bad = 0;
    {
        const auto& d = ch["WMDL"];
        std::size_t p = 0;
        for (int i = 0; i < 256; ++i) {
            const std::uint32_t ptr = u32(&d[p]);
            p += 4;
            if (!ptr) continue;
            const auto& w = weapons.slots[static_cast<std::size_t>(i)];
            const int src = w.model_slot;
            std::vector<std::uint8_t> ours;
            const bool have = src >= 0 && expected_for(weapons.slots[static_cast<std::size_t>(src)].model_name, ours);
            const bool same = have && p + ours.size() <= d.size() && std::memcmp(&d[p], ours.data(), ours.size()) == 0;
            if (same) {
                ++weapon_ok;
                p += ours.size();
            } else {
                ++weapon_bad;
                std::printf("weapon slot %d %s: model differs%s\n", i, w.name.c_str(), have ? "" : " (ours missing)");
                return 1;
            }
        }
    }
    std::printf("unit models identical: %zu, different: %zu\n", unit_ok, unit_bad);
    std::printf("weapon models identical: %zu, different: %zu\n", weapon_ok, weapon_bad);
    return (unit_bad || weapon_bad) ? 1 : 0;
}
