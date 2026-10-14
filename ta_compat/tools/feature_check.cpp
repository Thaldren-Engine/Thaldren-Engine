// Compares our feature definition table with the one the running game built
// for a battle (ta_capture --dump-defs: FEAT + FTX2 chunks), field by field,
// including model trees, GAF sequence names and every unit's corpse index.
//
// Usage: feature_check <game_dir> <defs.bin> <map name> [schema] [--verbose]
//   e.g. feature_check "C:\TA" defs.bin "Moon Quartet"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "ta/featuredefs.h"
#include "ta/hpi.h"
#include "ta/model3do.h"
#include "ta/unitdefs.h"
#include "model_dump_format.h"

namespace {

constexpr std::size_t kFeatureSize = 0x100;
constexpr std::size_t kUnitDefSize = 0x249;

template <class T> T rd(const std::uint8_t* p) { T v; std::memcpy(&v, p, sizeof v); return v; }

std::string cstr(const std::uint8_t* p, std::size_t max) {
    std::size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

struct Cursor {
    const std::vector<std::uint8_t>& d;
    std::size_t p = 0;
    bool ok = true;
    std::uint32_t u32() {
        if (p + 4 > d.size()) { ok = false; return 0; }
        const std::uint32_t v = rd<std::uint32_t>(&d[p]);
        p += 4;
        return v;
    }
    std::string str() {
        std::string s;
        while (p < d.size() && d[p]) s += static_cast<char>(d[p++]);
        if (p >= d.size()) ok = false;
        ++p;
        return s;
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: feature_check <game_dir> <defs.bin> <map name> [schema] [--verbose]\n");
        return 2;
    }
    const std::string map = argv[3];
    std::string schema = "Schema 0";
    bool verbose = false;
    for (int i = 4; i < argc; ++i) {
        if (std::string(argv[i]) == "--verbose") verbose = true;
        else schema = argv[i];
    }

    std::ifstream f(argv[2], std::ios::binary);
    std::vector<std::uint8_t> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::map<std::string, std::vector<std::uint8_t>> ch;
    for (std::size_t p = 0; p + 8 <= all.size();) {
        const std::string tag(reinterpret_cast<const char*>(&all[p]), 4);
        const std::uint32_t n = rd<std::uint32_t>(&all[p + 4]);
        ch[tag].assign(all.begin() + static_cast<std::ptrdiff_t>(p + 8), all.begin() + static_cast<std::ptrdiff_t>(p + 8 + n));
        p += 8 + n;
    }
    if (!ch.count("FEAT") || !ch.count("FTX2") || !ch.count("UDEF")) {
        std::fprintf(stderr, "dump lacks FEAT/FTX2/UDEF chunks\n");
        return 1;
    }

    // Battle loading order: feature library, map (TNT) features, schema features, units (corpses), chains.
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    ta::UnitDefContext ctx;
    ta::load_unit_context(vfs, ctx);
    ta::FeatureDefTable features;
    features.load_library(vfs, ctx.weapon_names);
    std::vector<std::uint8_t> data;
    std::vector<std::string> names;
    if (!vfs.read("maps\\" + map + ".tnt", data) || !ta::tnt_feature_names(data, names)) {
        std::fprintf(stderr, "cannot read maps\\%s.tnt\n", map.c_str());
        return 1;
    }
    features.load_map_features(names);
    if (vfs.read("maps\\" + map + ".ota", data)) {
        auto ota = ta::tdf_parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
        if (ota.root) features.load_schema_features(*ota.root, schema);
    }
    const ta::UnitDefTable units = ta::load_unit_defs(vfs, ctx, &features);
    features.resolve_chains();
    for (const auto& n : features.notes()) std::printf("note: %s\n", n.c_str());

    const auto& F = ch["FEAT"];
    const auto& recs = features.records();
    const std::size_t game_n = F.size() / kFeatureSize;
    std::printf("feature library: %zu files; map features: %zu; table: game %zu ours %zu\n",
                features.library_files().size(), names.size(), game_n, recs.size());

    Cursor x{ch["FTX2"]};
    std::map<std::string, std::size_t> bad_fields;
    std::size_t checked = 0, bad_records = 0;
    const std::size_t n = std::min(game_n, recs.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* g = &F[i * kFeatureSize];
        const ta::FeatureDefRecord& o = recs[i];
        std::vector<std::string> diffs;
        auto chk = [&](const char* field, bool ok, const std::string& detail) {
            ++checked;
            if (!ok) {
                ++bad_fields[field];
                diffs.push_back(std::string(field) + " " + detail);
            }
        };
        auto chk_i = [&](const char* field, long long game, long long ours) {
            chk(field, game == ours, "game " + std::to_string(game) + " ours " + std::to_string(ours));
        };
        auto chk_s = [&](const char* field, const std::string& game, const std::string& ours) {
            chk(field, game == ours, "game \"" + game + "\" ours \"" + ours + "\"");
        };
        auto chk_f = [&](const char* field, float game, float ours) {
            chk(field, std::memcmp(&game, &ours, 4) == 0, "game " + std::to_string(game) + " ours " + std::to_string(ours));
        };
        const std::uint16_t flags = rd<std::uint16_t>(g + 0xFE);
        chk_s("name", cstr(g, 128), o.name);
        chk_s("description", cstr(g + 0x80, 20), o.description);
        chk_i("footprint_x", rd<std::int16_t>(g + 0x94), o.footprint_x);
        chk_i("footprint_z", rd<std::int16_t>(g + 0x96), o.footprint_z);
        chk_i("height", g[0xFA], o.height);
        chk_i("flags", flags & ta::feature_flags::kDefinedMask, o.flags & ta::feature_flags::kDefinedMask);
        chk_i("spread_chance", g[0xFB], o.spread_chance);
        chk_i("reproduce", g[0xFC], o.reproduce);
        chk_i("reproduce_area", g[0xFD], o.reproduce_area);
        chk_f("metal", rd<float>(g + 0xF0), o.metal);
        chk_f("energy", rd<float>(g + 0xEC), o.energy);
        chk_i("damage", rd<std::int16_t>(g + 0xEA), o.damage);
        chk_i("spark_time", rd<std::int16_t>(g + 0xE8), o.spark_time);
        chk_i("dead", rd<std::int16_t>(g + 0xF4), o.dead);
        chk_i("burnt", rd<std::int16_t>(g + 0xF6), o.burnt);
        chk_i("reclamate", rd<std::int16_t>(g + 0xF8), o.reclamate);
        if (flags & ta::feature_flags::kGaf) {
            chk_s("gaf_file", cstr(g + 0x98, 16), o.gaf_file);
            chk_i("gaf_loaded_here", x.u32() != 0, o.gaf_owner == static_cast<int>(i));
            static const char* const kSeq[8] = {"seq", "seq_shadow", "seq_burn", "seq_burn_shadow",
                                                "seq_die", "seq_die_shadow", "seq_reclamate", "seq_reclamate_shadow"};
            for (int k = 0; k < 8; ++k) chk_s(kSeq[k], x.str(), o.sequences[static_cast<std::size_t>(k)]);
        } else {
            std::vector<std::uint8_t> ours;
            if (vfs.read("objects3d\\" + o.object + ".3DO", data)) {
                const auto m = ta::load_3do(data, true);
                if (m.root) tools::serialize_model(ours, m.root.get());
            }
            // The dump format does not mark child chains, so compare our serialization as a prefix
            // and stop on a mismatch (the stream cannot be resynchronised).
            const std::uint32_t ptr = x.u32();
            const bool same = ptr && x.p + ours.size() <= x.d.size() &&
                              std::memcmp(&x.d[x.p], ours.data(), ours.size()) == 0;
            if (ptr && !same) {
                std::printf("feature %zu %s: model differs (object \"%s\"); stopping\n", i, o.name.c_str(), o.object.c_str());
                return 1;
            }
            if (ptr) x.p += ours.size();
            chk("model", same, "object \"" + o.object + "\"");
        }
        chk_i("burn_weapon", static_cast<std::int32_t>(x.u32()), o.burn_weapon);
        if (!x.ok) {
            std::printf("FTX2 chunk ended early at feature %zu\n", i);
            return 1;
        }
        if (!diffs.empty()) {
            ++bad_records;
            if (verbose || bad_records <= 10) {
                std::printf("feature %zu %s:\n", i, o.name.c_str());
                for (const auto& s : diffs) std::printf("  %s\n", s.c_str());
            }
        }
    }

    // Each unit's corpse index into this table.
    const auto& U = ch["UDEF"];
    std::size_t corpse_bad = 0, corpse_checked = 0;
    for (std::size_t i = 0; i < units.units.size() && (i + 1) * kUnitDefSize <= U.size(); ++i) {
        const std::int16_t game = rd<std::int16_t>(&U[i * kUnitDefSize + 444]);
        ++corpse_checked;
        if (game != units.units[i].corpse_feature_id && ++corpse_bad <= 10)
            std::printf("unit %zu %s corpse: game %d ours %d\n", i, units.units[i].unit_name.c_str(), game,
                        units.units[i].corpse_feature_id);
    }

    for (const auto& [k, v] : bad_fields) std::printf("field %s: %zu differ\n", k.c_str(), v);
    std::printf("features: %zu fields checked, %zu of %zu records differ; unit corpses: %zu checked, %zu differ\n",
                checked, bad_records, n, corpse_checked, corpse_bad);
    return (bad_records || corpse_bad || game_n != recs.size()) ? 1 : 0;
}
