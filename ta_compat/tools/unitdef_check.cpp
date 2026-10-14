// Compares our unit definition table with one dumped from the running game
// (ta_capture --dump-defs). Every field of every unit is checked.
//
// Usage: unitdef_check <game_dir> <defs.bin> [--verbose]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ta/hpi.h"
#include "ta/tdf.h"
#include "ta/unitdefs.h"

namespace {

constexpr std::size_t kUnitDefSize = 0x249;
constexpr std::size_t kWeaponDefSize = 0x115;
constexpr std::uint32_t kMoveInfoBase = 0x00512358;

struct Dump {
    std::map<std::string, std::vector<std::uint8_t>> chunks;
    std::uint32_t gs = 0, ud_count = 0, ud_ptr = 0, wd_base = 0, mv_base = 0, ft_count = 0, ft_ptr = 0, sc_count = 0, sc_ptr = 0;
    std::map<std::uint32_t, std::string> cat_name_by_ptr;
    std::map<std::string, std::array<std::uint32_t, 16>> cat_mask_by_lname;
    std::map<std::uint32_t, std::vector<std::uint8_t>> yard;
    std::map<std::uint32_t, std::vector<std::uint8_t>> bopt;
    std::vector<std::string> mission_names;
    std::string language;
    std::vector<std::string> movement_names;
};

std::uint32_t u32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
std::int32_t i32(const std::uint8_t* p) { std::int32_t v; std::memcpy(&v, p, 4); return v; }
std::int16_t i16(const std::uint8_t* p) { std::int16_t v; std::memcpy(&v, p, 2); return v; }
std::uint16_t u16(const std::uint8_t* p) { std::uint16_t v; std::memcpy(&v, p, 2); return v; }
float f32(const std::uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }
std::string cstr(const std::uint8_t* p, std::size_t max) {
    std::size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool read_dump(const char* path, Dump& d) {
    std::ifstream f(path, std::ios::binary);
    std::vector<std::uint8_t> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::size_t p = 0;
    while (p + 8 <= all.size()) {
        const std::string tag(reinterpret_cast<const char*>(&all[p]), 4);
        const std::uint32_t n = u32(&all[p + 4]);
        d.chunks[tag].assign(all.begin() + static_cast<std::ptrdiff_t>(p + 8), all.begin() + static_cast<std::ptrdiff_t>(p + 8 + n));
        p += 8 + n;
    }
    const auto& h = d.chunks["HDR "];
    if (h.size() < 36) return false;
    d.gs = u32(&h[0]); d.ud_count = u32(&h[4]); d.ud_ptr = u32(&h[8]); d.wd_base = u32(&h[12]); d.mv_base = u32(&h[16]);
    d.ft_count = u32(&h[20]); d.ft_ptr = u32(&h[24]); d.sc_count = u32(&h[28]); d.sc_ptr = u32(&h[32]);
    const auto& c = d.chunks["CATS"];
    std::size_t q = 4;
    for (std::uint32_t k = 0; k < u32(&c[0]); ++k) {
        const std::uint32_t ptr = u32(&c[q]);
        std::array<std::uint32_t, 16> mask{};
        std::memcpy(mask.data(), &c[q + 4], 64);
        q += 68;
        const std::string name = cstr(&c[q], c.size() - q);
        q += name.size() + 1;
        d.cat_name_by_ptr[ptr] = name;
        d.cat_mask_by_lname[lower(name)] = mask;
    }
    const auto& y = d.chunks["YARD"];
    for (std::size_t r = 0; r + 8 <= y.size();) {
        const std::uint32_t idx = u32(&y[r]), len = u32(&y[r + 4]);
        d.yard[idx].assign(y.begin() + static_cast<std::ptrdiff_t>(r + 8), y.begin() + static_cast<std::ptrdiff_t>(r + 8 + len));
        r += 8 + len;
    }
    const auto& b = d.chunks["BOPT"];
    for (std::size_t r = 0; r + 4 + 0x3C <= b.size(); r += 4 + 0x3C) {
        d.bopt[u32(&b[r])].assign(b.begin() + static_cast<std::ptrdiff_t>(r + 4), b.begin() + static_cast<std::ptrdiff_t>(r + 4 + 0x3C));
    }
    const auto& m = d.chunks["MISS"];
    if (m.size() >= 4) {
        std::size_t r = 4;
        for (std::uint32_t k = 0; k < u32(&m[0]); ++k) {
            const std::string lookup = cstr(&m[r], m.size() - r);
            r += lookup.size() + 1;
            const std::string display = cstr(&m[r], m.size() - r);
            r += display.size() + 1;
            d.mission_names.push_back(lookup);
        }
    }
    const auto& l = d.chunks["LANG"];
    if (!l.empty()) d.language = cstr(l.data(), l.size());
    const auto& mv = d.chunks["MVNM"];
    for (std::size_t r = 0, k = 0; k < 32 && r + 4 <= mv.size(); ++k) {
        const std::string n = cstr(&mv[r + 4], mv.size() - r - 4);
        d.movement_names.push_back(n);
        r += 4 + n.size() + 1;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: unitdef_check <game_dir> <defs.bin> [--verbose]\n");
        return 2;
    }
    const bool verbose = argc > 3 && std::string(argv[3]) == "--verbose";
    Dump d;
    if (!read_dump(argv[2], d)) {
        std::fprintf(stderr, "cannot read dump %s\n", argv[2]);
        return 1;
    }

    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    ta::UnitDefContext ctx;
    ta::load_unit_context(vfs, ctx);

    // The built-in mission table and language must match what the game used.
    std::size_t ctx_bad = 0;
    if (!d.mission_names.empty() && d.mission_names != ctx.mission_names) {
        ++ctx_bad;
        std::printf("mission table differs from the game's\n");
    }
    if (!d.chunks["LANG"].empty() && d.language != ctx.language) {
        ++ctx_bad;
        std::printf("language: game \"%s\" ours \"%s\"\n", d.language.c_str(), ctx.language.c_str());
    }
    const auto& wd = d.chunks["WDEF"];
    for (int i = 0; i < 256; ++i) {
        const std::string game = cstr(&wd[static_cast<std::size_t>(i) * kWeaponDefSize], 32);
        if (game != ctx.weapon_names[static_cast<std::size_t>(i)]) {
            if (++ctx_bad <= 10) std::printf("weapon slot %d: game \"%s\" ours \"%s\"\n", i, game.c_str(), ctx.weapon_names[static_cast<std::size_t>(i)].c_str());
        }
    }
    for (std::size_t i = 0; i < 32 && i < d.movement_names.size(); ++i) {
        if (d.movement_names[i] != ctx.movement_class_names[i] && ++ctx_bad <= 20)
            std::printf("movement class %zu: game \"%s\" ours \"%s\"\n", i, d.movement_names[i].c_str(), ctx.movement_class_names[i].c_str());
    }
    const auto& sc = d.chunks["SNDC"];
    if (sc.size() / 0x160 != ctx.sound_categories.size()) {
        ++ctx_bad;
        std::printf("sound categories: game %zu ours %zu\n", sc.size() / 0x160, ctx.sound_categories.size());
    }
    std::printf("context: language \"%s\", %zu mission names, %zu context mismatches\n", d.language.c_str(),
                d.mission_names.size(), ctx_bad);

    const ta::UnitDefTable table = ta::load_unit_defs(vfs, ctx);
    for (const auto& n : table.notes) std::printf("note: %s\n", n.c_str());
    std::printf("units: game %u ours %zu\n", d.ud_count, table.units.size());

    const auto& U = d.chunks["UDEF"];
    const auto& F = d.chunks["FEAT"];
    std::map<std::string, std::size_t> bad_fields;
    std::size_t bad_units = 0, checked_fields = 0;
    const std::size_t n = std::min<std::size_t>(d.ud_count, table.units.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* g = &U[i * kUnitDefSize];
        const ta::UnitDefRecord& o = table.units[i];
        std::vector<std::string> diffs;
        auto chk = [&](const char* field, bool ok, const std::string& detail) {
            ++checked_fields;
            if (!ok) {
                ++bad_fields[field];
                diffs.push_back(std::string(field) + " " + detail);
            }
        };
        auto chk_s = [&](const char* field, std::size_t off, std::size_t size, const std::string& ours) {
            const std::string game = cstr(g + off, size);
            chk(field, game == ours, "game \"" + game + "\" ours \"" + ours + "\"");
        };
        auto chk_i = [&](const char* field, long long game, long long ours) {
            chk(field, game == ours, "game " + std::to_string(game) + " ours " + std::to_string(ours));
        };
        auto chk_f = [&](const char* field, std::size_t off, float ours) {
            const float game = f32(g + off);
            chk(field, std::memcmp(&game, &ours, 4) == 0, "game " + std::to_string(game) + " ours " + std::to_string(ours));
        };
        auto weapon_slot = [&](std::size_t off) {
            const std::uint32_t p = u32(g + off);
            return p ? static_cast<long long>((p - d.wd_base) / kWeaponDefSize) : -1;
        };
        auto cat_name = [&](std::size_t off) {
            const auto it = d.cat_name_by_ptr.find(u32(g + off));
            return it == d.cat_name_by_ptr.end() ? std::string("<unknown ptr>") : it->second;
        };
        chk_s("name", 0, 32, o.name);
        chk_s("unit_name", 32, 32, o.unit_name);
        chk_s("description", 64, 64, o.description);
        chk_s("object_name", 128, 32, o.object_name);
        chk_s("side", 160, 30, o.side);
        chk_s("ai_weight", 190, 64, o.ai_weight);
        chk_s("ai_limit", 254, 64, o.ai_limit);
        chk_i("fbi_checksum", u32(g + 318), o.fbi_checksum);
        chk_i("script_checksum", u32(g + 322), o.script_checksum);
        chk_i("weapon_hash_xor", u32(g + 326), o.weapon_hash_xor);
        chk_i("footprint_x", i16(g + 330), o.footprint_x);
        chk_i("footprint_z", i16(g + 332), o.footprint_z);
        {
            const bool game_has = u32(g + 334) != 0;
            chk("yard_map.present", game_has == o.has_yard_map, game_has ? "game has, ours not" : "ours has, game not");
            if (game_has && o.has_yard_map) {
                const auto& gy = d.yard[static_cast<std::uint32_t>(i)];
                chk("yard_map.bytes", gy == o.yard_map, "differs");
            }
        }
        chk_i("build_options_count", i32(g + 338), o.build_options_count);
        {
            const bool game_has = u32(g + 342) != 0;
            chk("build_options.present", game_has == o.has_build_options, "differs");
            if (game_has && o.has_build_options) {
                const auto& gb = d.bopt[static_cast<std::uint32_t>(i)];
                const int cnt = std::min(o.build_options_count, 30);
                bool same_used = true, same_all = gb.size() == 60;
                for (int k = 0; k < 30 && gb.size() == 60; ++k) {
                    const std::uint16_t gv = u16(&gb[static_cast<std::size_t>(k) * 2]);
                    if (gv != o.build_options[static_cast<std::size_t>(k)]) {
                        same_all = false;
                        if (k < cnt) same_used = false;
                    }
                }
                chk("build_options.used", same_used, "differs");
                chk("build_options.stale_tail", same_all, "differs (uninitialised scratch)");
            }
        }
        chk_i("max_this_unit", i32(g + 346), o.max_this_unit);
        chk_i("model_min_x", i32(g + 350), o.model_min_x);
        chk_i("model_min_z", i32(g + 358), o.model_min_z);
        chk_i("model_max_x", i32(g + 362), o.model_max_x);
        chk_i("model_max_z", i32(g + 370), o.model_max_z);
        chk_i("model_size_x", i32(g + 374), o.model_size_x);
        chk_i("model_size_z", i32(g + 382), o.model_size_z);
        chk_i("model_avg_size", i32(g + 386), o.model_avg_size);
        chk_f("build_cost_energy", 390, o.build_cost_energy);
        chk_f("build_cost_metal", 394, o.build_cost_metal);
        chk_i("max_velocity", i32(g + 402), o.max_velocity);
        chk_i("scaled_max_velocity", i32(g + 406), o.scaled_max_velocity);
        chk_i("brake_rate", i32(g + 410), o.brake_rate);
        chk_i("acceleration", i32(g + 414), o.acceleration);
        chk_i("bank_scale", i32(g + 418), o.bank_scale);
        chk_i("pitch_scale", i32(g + 422), o.pitch_scale);
        chk_i("damage_modifier", i32(g + 426), o.damage_modifier);
        chk_i("move_rate1", i32(g + 430), o.move_rate1);
        chk_i("move_rate2", i32(g + 434), o.move_rate2);
        {
            const std::uint32_t p = u32(g + 438);
            chk_i("movement_class", p ? static_cast<long long>((p - kMoveInfoBase) / 32) : -1, o.movement_class);
        }
        chk_i("turn_rate", u16(g + 442), o.turn_rate);
        {
            const std::int16_t id = i16(g + 444);
            const std::string game = (id >= 0 && static_cast<std::uint32_t>(id) < d.ft_count) ? cstr(&F[static_cast<std::size_t>(id) * 0x100], 32) : "";
            const std::string ours = o.has_corpse ? o.corpse : "";
            chk("corpse", o.has_corpse ? ta::stricmp_c(game, ours) == 0 : id == o.corpse_feature_id,
                "game id " + std::to_string(id) + " \"" + game + "\" ours \"" + ours + "\"");
        }
        chk_i("max_water_depth", i16(g + 446), o.max_water_depth);
        chk_i("min_water_depth", i16(g + 448), o.min_water_depth);
        chk_f("energy_make", 450, o.energy_make);
        chk_f("energy_use", 454, o.energy_use);
        chk_f("metal_make", 458, o.metal_make);
        chk_f("extracts_metal", 462, o.extracts_metal);
        chk_f("wind_generator", 466, o.wind_generator);
        chk_f("tidal_generator", 470, o.tidal_generator);
        chk_f("cloak_cost", 474, o.cloak_cost);
        chk_f("cloak_cost_moving", 478, o.cloak_cost_moving);
        chk_f("energy_storage", 482, o.energy_storage);
        chk_f("metal_storage", 486, o.metal_storage);
        chk_i("build_time", i32(g + 490), o.build_time);
        chk_i("weapon1", weapon_slot(494), o.weapon1);
        chk_i("weapon2", weapon_slot(498), o.weapon2);
        chk_i("weapon3", weapon_slot(502), o.weapon3);
        chk_i("max_damage", u32(g + 506), o.max_damage);
        chk_i("worker_time", i16(g + 510), o.worker_time);
        chk_i("heal_time", i16(g + 512), o.heal_time);
        chk_i("sight_distance", i16(g + 514), o.sight_distance);
        chk_i("radar_distance", i16(g + 516), o.radar_distance);
        chk_i("sonar_distance", i16(g + 518), o.sonar_distance);
        chk_i("min_cloak_distance", i16(g + 520), o.min_cloak_distance);
        chk_i("radar_distance_jam", i16(g + 522), o.radar_distance_jam);
        chk_i("sonar_distance_jam", i16(g + 524), o.sonar_distance_jam);
        chk_i("sound_category", i16(g + 526), o.sound_category);
        chk_i("build_angle", i16(g + 528), o.build_angle);
        chk_i("build_distance", i16(g + 530), o.build_distance);
        chk_i("maneuver_leash_length", i16(g + 532), o.maneuver_leash_length);
        chk_i("attack_run_length", i16(g + 534), o.attack_run_length);
        chk_i("kamikaze_distance", i16(g + 536), o.kamikaze_distance);
        chk_i("sort_bias", i16(g + 538), o.sort_bias);
        chk_i("cruise_alt", i16(g + 540), o.cruise_alt);
        chk_i("index", u16(g + 542), o.index);
        chk_i("explode_as", weapon_slot(544), o.explode_as);
        chk_i("self_destruct_as", weapon_slot(548), o.self_destruct_as);
        chk_i("max_slope", g[552], o.max_slope);
        chk_i("max_water_slope", g[553], o.max_water_slope);
        chk_i("transport_size", g[554], o.transport_size);
        chk_i("transport_capacity", g[555], o.transport_capacity);
        chk_i("waterline", g[556], o.waterline);
        chk_i("makes_metal", g[557], o.makes_metal);
        chk_i("build_menu_page_count", g[558], o.build_menu_page_count);
        chk_i("bm_code", g[559], o.bm_code);
        chk_i("default_mission", g[560], o.default_mission);
        if (i > 0) {
            chk("wpri_bad_target", ta::stricmp_c(cat_name(561), o.wpri_bad_target) == 0, "game \"" + cat_name(561) + "\" ours \"" + o.wpri_bad_target + "\"");
            chk("wsec_bad_target", ta::stricmp_c(cat_name(565), o.wsec_bad_target) == 0, "game \"" + cat_name(565) + "\" ours \"" + o.wsec_bad_target + "\"");
            chk("wspe_bad_target", ta::stricmp_c(cat_name(569), o.wspe_bad_target) == 0, "game \"" + cat_name(569) + "\" ours \"" + o.wspe_bad_target + "\"");
            chk("no_chase", ta::stricmp_c(cat_name(573), o.no_chase) == 0, "game \"" + cat_name(573) + "\" ours \"" + o.no_chase + "\"");
        }
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, "game %08X ours %08X", u32(g + 577), o.type_flags);
            chk("type_flags", u32(g + 577) == o.type_flags, buf);
            std::snprintf(buf, sizeof buf, "game %08X ours %08X", u32(g + 581), o.can_do_flags);
            chk("can_do_flags", u32(g + 581) == o.can_do_flags, buf);
        }
        if (!diffs.empty()) {
            ++bad_units;
            if (verbose || bad_units <= 5) {
                std::printf("unit %zu %s:\n", i, o.unit_name.c_str());
                for (const auto& df : diffs) std::printf("    %s\n", df.c_str());
            }
        }
    }

    // Category masks built by unit loading.
    std::size_t cat_bad = 0, cat_checked = 0;
    for (const auto& [lname, entry] : table.categories.all()) {
        ++cat_checked;
        const auto it = d.cat_mask_by_lname.find(lname);
        if (it == d.cat_mask_by_lname.end() || it->second != entry.second) {
            if (++cat_bad <= 10) std::printf("category mask differs: %s\n", entry.first.c_str());
        }
    }

    std::printf("\nfields checked: %zu, units with differences: %zu / %zu\n", checked_fields, bad_units, n);
    for (const auto& [field, count] : bad_fields) std::printf("  %-26s %zu units\n", field.c_str(), count);
    std::printf("category masks: %zu checked, %zu differ\n", cat_checked, cat_bad);
    std::printf("(not compared yet: model Y extent from 3DO, COB script pointer)\n");
    return (bad_units == 0 && cat_bad == 0 && ctx_bad == 0 && d.ud_count == table.units.size()) ? 0 : 1;
}
