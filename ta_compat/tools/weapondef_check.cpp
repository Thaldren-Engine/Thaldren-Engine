// Compares our weapon table with the one dumped from the running game
// (ta_capture --dump-defs). Every field of all 256 slots is checked.
//
// Usage: weapondef_check <game_dir> <defs.bin> [--verbose]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "ta/hpi.h"
#include "ta/tdf.h"
#include "ta/weapondefs.h"

namespace {

constexpr std::size_t kW = 0x115;

std::uint32_t u32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
std::int32_t i32(const std::uint8_t* p) { std::int32_t v; std::memcpy(&v, p, 4); return v; }
std::int16_t i16(const std::uint8_t* p) { std::int16_t v; std::memcpy(&v, p, 2); return v; }
std::string cstr(const std::uint8_t* p, std::size_t max) {
    std::size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: weapondef_check <game_dir> <defs.bin> [--verbose]\n");
        return 2;
    }
    const bool verbose = argc > 3 && std::string(argv[3]) == "--verbose";
    std::ifstream f(argv[2], std::ios::binary);
    std::vector<std::uint8_t> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::map<std::string, std::vector<std::uint8_t>> ch;
    for (std::size_t p = 0; p + 8 <= all.size();) {
        const std::string tag(reinterpret_cast<const char*>(&all[p]), 4);
        const std::uint32_t n = u32(&all[p + 4]);
        ch[tag].assign(all.begin() + static_cast<std::ptrdiff_t>(p + 8), all.begin() + static_cast<std::ptrdiff_t>(p + 8 + n));
        p += 8 + n;
    }
    if (!ch.count("WDMG") || !ch.count("WSEQ") || !ch.count("SBNK")) {
        std::fprintf(stderr, "dump lacks WDMG/WSEQ/SBNK chunks (re-capture with a newer ta_capture)\n");
        return 1;
    }
    const auto& W = ch["WDEF"];
    // Damage maps.
    std::map<std::uint32_t, std::vector<std::pair<std::string, std::int32_t>>> dmaps;
    {
        const auto& d = ch["WDMG"];
        for (std::size_t r = 0; r + 8 <= d.size();) {
            const std::uint32_t slot = u32(&d[r]), n = u32(&d[r + 4]);
            r += 8;
            auto& v = dmaps[slot];
            for (std::uint32_t k = 0; k < n; ++k) {
                const std::int32_t dmg = i32(&d[r]);
                r += 4;
                const std::string name = cstr(&d[r], d.size() - r);
                r += name.size() + 1;
                v.emplace_back(name, dmg);
            }
        }
    }
    std::vector<std::pair<std::string, std::string>> seqs(256);
    {
        const auto& s = ch["WSEQ"];
        for (std::size_t r = 0; r + 4 <= s.size();) {
            const std::uint32_t slot = u32(&s[r]);
            r += 4;
            const std::string a = cstr(&s[r], s.size() - r);
            r += a.size() + 1;
            const std::string b = cstr(&s[r], s.size() - r);
            r += b.size() + 1;
            if (slot < 256) seqs[slot] = {a, b};
        }
    }
    const auto& S = ch["SBNK"];
    // Sound bank layout (verified from SoundBank_EnsureNamed): +4 count, +8 slots[256],
    // +0x408 names[256][32], +0x2408 paths[256][32].
    auto bank_path = [&](int idx) {
        const std::size_t off = 0x2408 + static_cast<std::size_t>(idx) * 32;
        return (idx >= 0 && idx < 256 && off + 32 <= S.size()) ? cstr(&S[off], 32) : std::string("?");
    };

    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    const ta::WeaponDefTable t = ta::load_weapon_defs(vfs, ta::WeaponDefOptions{});
    for (const auto& n : t.notes) std::printf("note: %s\n", n.c_str());

    std::map<std::string, std::size_t> bad_fields;
    std::size_t bad_slots = 0, checked = 0, loaded = 0;
    for (int i = 0; i < 256; ++i) {
        const std::uint8_t* g = &W[static_cast<std::size_t>(i) * kW];
        const ta::WeaponDefRecord& o = t.slots[static_cast<std::size_t>(i)];
        if (o.loaded) ++loaded;
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
        auto chk_f = [&](const char* field, std::size_t off, float ours) {
            float game;
            std::memcpy(&game, g + off, 4);
            chk(field, std::memcmp(&game, &ours, 4) == 0, "game " + std::to_string(game) + " ours " + std::to_string(ours));
        };
        auto chk_sound = [&](const char* field, std::size_t off, const std::string& ours) {
            const int idx = i16(g + off);
            if (ours.empty()) {
                chk(field, idx == -1 || (!o.loaded && idx == 0), "game index " + std::to_string(idx) + " ours none");
            } else {
                const std::string gp = bank_path(idx);
                chk(field, ta::stricmp_c(gp, ours.substr(0, 32)) == 0, "game \"" + gp + "\" ours \"" + ours + "\"");
            }
        };
        const std::string gname = cstr(g, 32);
        chk("name", gname == (o.loaded ? o.name.substr(0, 31) : std::string()), "game \"" + gname + "\" ours \"" + o.name + "\"");
        if (!o.loaded) {
            // Untouched slot: everything zero except the index byte.
            bool zero = true;
            for (std::size_t k = 0; k < kW; ++k)
                if (k != 266 && g[k] != 0) zero = false;
            chk("unused_slot_zero", zero, "has nonzero bytes");
            chk_i("index", g[266], o.index);
        } else {
            chk("description", cstr(g + 32, 64) == o.description, "game \"" + cstr(g + 32, 64) + "\" ours \"" + o.description + "\"");
            static const std::uint32_t kFireAddr[] = {0, 0x0049D580, 0x0049DB70, 0x0049D9C0, 0x0049DD60};
            chk_i("fire_fn", u32(g + 96), kFireAddr[static_cast<int>(o.fire)]);
            {
                const bool game_has = u32(g + 100) != 0;
                chk("damage_map.present", game_has == o.has_damage_map, "differs");
                if (game_has) {
                    const auto& gm = dmaps[static_cast<std::uint32_t>(i)];
                    chk("damage_map.entries", gm == o.damage_overrides,
                        "game " + std::to_string(gm.size()) + " entries, ours " + std::to_string(o.damage_overrides.size()));
                }
            }
            chk_i("weapon_velocity", i32(g + 104), o.weapon_velocity);
            chk_i("start_velocity", i32(g + 108), o.start_velocity);
            chk_i("weapon_acceleration", i32(g + 112), o.weapon_acceleration);
            {
                const std::uint32_t mp = u32(g + 116);
                if (o.model_slot < 0) chk("model", mp == 0, "game has a model, ours none");
                else if (o.model_slot == i) chk("model", mp != 0, "game none, ours own model");
                else chk("model", mp == u32(&W[static_cast<std::size_t>(o.model_slot) * kW + 116]), "not shared with slot " + std::to_string(o.model_slot));
                const std::string gm = cstr(g + 128, 64);
                chk("model_name", gm == o.model_name, "game \"" + gm + "\" ours \"" + o.model_name + "\"");
            }
            chk("explosion_seq", ta::stricmp_c(seqs[static_cast<std::size_t>(i)].first, o.explosion_seq) == 0,
                "game \"" + seqs[static_cast<std::size_t>(i)].first + "\" ours \"" + o.explosion_seq + "\"");
            chk("water_explosion_seq", ta::stricmp_c(seqs[static_cast<std::size_t>(i)].second, o.water_explosion_seq) == 0,
                "game \"" + seqs[static_cast<std::size_t>(i)].second + "\" ours \"" + o.water_explosion_seq + "\"");
            chk_f("energy_per_shot", 192, o.energy_per_shot);
            chk_f("metal_per_shot", 196, o.metal_per_shot);
            chk_f("min_barrel_angle", 200, o.min_barrel_angle);
            chk_i("shake_magnitude", i32(g + 204), o.shake_magnitude);
            chk_i("shake_duration", i32(g + 208), o.shake_duration);
            chk_i("default_damage", i16(g + 212), o.default_damage);
            chk_i("area_of_effect", i16(g + 214), o.area_of_effect);
            chk_f("edge_effectiveness", 216, o.edge_effectiveness);
            chk_i("range", i32(g + 220), o.range);
            chk_i("coverage", i32(g + 224), o.coverage);
            chk_i("reload_time", i16(g + 228), o.reload_time);
            chk_i("weapon_timer", i16(g + 230), o.weapon_timer);
            chk_i("turn_rate", i16(g + 232), o.turn_rate);
            chk_i("burst", i16(g + 234), o.burst);
            chk_i("burst_rate", i16(g + 236), o.burst_rate);
            chk_i("spray_angle", i16(g + 238), o.spray_angle);
            chk_i("duration", i16(g + 240), o.duration);
            chk_i("random_decay", i16(g + 242), o.random_decay);
            chk_sound("sound_start", 244, o.sound_start);
            chk_sound("sound_hit", 246, o.sound_hit);
            chk_sound("sound_water", 248, o.sound_water);
            chk_i("smoke_delay", i16(g + 250), o.smoke_delay);
            chk_i("flight_time", i16(g + 252), o.flight_time);
            chk_i("hold_time", i16(g + 254), o.hold_time);
            chk_i("unused_0x100", u32(g + 256), 0);
            chk_i("accuracy", i16(g + 260), o.accuracy);
            chk_i("tolerance", i16(g + 262), o.tolerance);
            chk_i("pitch_tolerance", i16(g + 264), o.pitch_tolerance);
            chk_i("index", g[266], o.index);
            chk_i("fire_starter", g[267], o.fire_starter);
            chk_i("render_type", g[268], o.render_type);
            chk_i("color", g[269], o.color);
            chk_i("color2", g[270], o.color2);
            char buf[64];
            std::snprintf(buf, sizeof buf, "game %08X ours %08X", u32(g + 273), o.flags);
            chk("flags", u32(g + 273) == o.flags, buf);
        }
        if (!diffs.empty()) {
            ++bad_slots;
            if (verbose || bad_slots <= 6) {
                std::printf("slot %d %s:\n", i, o.name.c_str());
                for (const auto& df : diffs) std::printf("    %s\n", df.c_str());
            }
        }
    }
    std::printf("\nweapon slots: %zu loaded, fields checked %zu, slots with differences %zu / 256\n", loaded, checked, bad_slots);
    for (const auto& [field, count] : bad_fields) std::printf("  %-22s %zu slots\n", field.c_str(), count);
    return bad_slots == 0 ? 0 : 1;
}
