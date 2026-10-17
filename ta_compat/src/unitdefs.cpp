#include "ta/unitdefs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "sim/x87.h"
#include "ta/featuredefs.h"
#include "ta/model3do.h"
#include "ta/tdf.h"

namespace ta {
namespace {

constexpr char kCopyrightTemplate[] = "Copyright 0000 Humongous Entertainment. All rights reserved.";
constexpr std::size_t kCopyrightYearOffset = 10;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string truncate(std::string s, std::size_t buf) {
    if (s.size() > buf - 1) s.resize(buf - 1);
    return s;
}

// base + "\" + name, extension (after the last '.') replaced by ext.
std::string virtual_path(const std::string& base, const std::string& name, const std::string& ext) {
    std::string p = base + "\\" + name;
    if (!ext.empty()) {
        const auto dot = p.rfind('.');
        if (dot != std::string::npos) p.resize(dot);
        p += "." + ext;
    }
    return p;
}

std::string strip_extension(std::string s) {
    const auto dot = s.rfind('.');
    if (dot != std::string::npos) s.resize(dot);
    return s;
}

std::unique_ptr<TdfSection> load_tdf(Vfs& vfs, const std::string& path, bool* loose = nullptr) {
    std::vector<std::uint8_t> data;
    if (!vfs.read(path, data) || data.empty()) return nullptr;
    if (loose) *loose = vfs.is_loose(path);
    auto r = tdf_parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    return std::move(r.root);
}

// Localized string: try "<language><key>" first, then "<key>".
std::string get_localized(const TdfSection& s, const std::string& lang, const std::string& key, std::size_t buf,
                          bool* found = nullptr) {
    bool f = false;
    std::string v = s.get_string(lang + key, buf, "", &f);
    if (!f) v = s.get_string(key, buf, "", &f);
    if (found) *found = f;
    return v;
}

int find_weapon(const UnitDefContext& ctx, const std::string& name) {
    if (name.empty()) return -1;
    for (int i = 0; i < 256; ++i)
        if (stricmp_c(ctx.weapon_names[static_cast<std::size_t>(i)], name) == 0) return i;
    return -1;
}

std::uint8_t find_mission(const UnitDefContext& ctx, const std::string& name) {
    const auto& t = ctx.mission_names;
    std::size_t lo = 0, hi = t.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (stricmp_c(t[mid], name) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo != t.size() && stricmp_c(t[lo], name) == 0) return static_cast<std::uint8_t>(lo);
    return 0;
}

void set_bit(std::uint32_t& flags, int bit, std::int32_t v) {
    flags = (flags & ~(1u << bit)) | ((static_cast<std::uint32_t>(v) & 1u) << bit);
}

// MoveInfo key loader (defaults = current values).
void load_move_info(const TdfSection& s, UnitDefContext::MoveInfo& m) {
    m.footprint_x = static_cast<std::int16_t>(s.get_int("FootPrintX", 0));
    m.footprint_z = static_cast<std::int16_t>(s.get_int("FootPrintZ", 0));
    m.max_water_depth = static_cast<std::int16_t>(s.get_int("maxwaterdepth", m.max_water_depth));
    m.min_water_depth = static_cast<std::int16_t>(s.get_int("minwaterdepth", m.min_water_depth));
    const std::uint32_t max_slope = static_cast<std::uint32_t>(s.get_int("maxslope", m.max_slope));
    m.max_slope = static_cast<std::uint8_t>(max_slope);
    m.bad_slope = static_cast<std::uint8_t>(s.get_int("badslope", static_cast<std::int32_t>((max_slope & 0xFF) >> 1)));
    const std::uint32_t max_wslope = static_cast<std::uint32_t>(s.get_int("maxwaterslope", m.max_water_slope));
    m.max_water_slope = static_cast<std::uint8_t>(max_wslope);
    const std::uint8_t bad_wslope =
        static_cast<std::uint8_t>(s.get_int("badwaterslope", static_cast<std::int32_t>((max_wslope & 0xFF) >> 1)));
    m.bad_water_slope = bad_wslope;
    if (m.max_water_slope < m.max_slope) m.max_slope = m.max_water_slope;
    if (m.max_slope < m.bad_slope) m.bad_slope = m.max_slope;
    if (m.max_water_slope < bad_wslope) m.bad_water_slope = m.max_water_slope;
}

// Whitespace tokens as read by the runtime's " %s" scanning.
std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    auto sp = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    while (true) {
        while (i < s.size() && sp(s[i])) ++i;
        if (i >= s.size()) break;
        std::size_t j = i;
        while (j < s.size() && !sp(s[j])) ++j;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

void apply_category(CategoryRegistry& reg, const std::string& name, std::uint16_t idx) {
    reg.lookup_or_create(name)[idx >> 5] |= 1u << (idx & 31);
}

struct WeaponFiles {
    std::vector<std::unique_ptr<TdfSection>> roots;  // loaded weapon TDF roots, enumeration order
};

WeaponFiles load_weapon_files(Vfs& vfs, const UnitDefContext& ctx) {
    WeaponFiles w;
    for (const auto& name : vfs.enumerate("Weapons\\*.tdf")) {
        bool loose = false;
        auto root = load_tdf(vfs, virtual_path("Weapons", name, "TDF"), &loose);
        if (root && (!loose || !ctx.cd_check_enabled)) w.roots.push_back(std::move(root));
    }
    return w;
}

// Checksum of the first weapon section named `name` across the weapon files (0 if none).
std::uint32_t weapon_section_checksum(const WeaponFiles& w, const std::string* name) {
    if (!name || name->empty()) return 0;
    for (const auto& root : w.roots) {
        if (const TdfSection* s = root->child(*name)) return s->checksum();
    }
    return 0;
}

// Fills the stub fields read while enumerating; returns false if the unit is incompatible.
bool load_stub(Vfs& vfs, const UnitDefContext& ctx, const WeaponFiles& weapons, const std::string& file,
               UnitDefRecord& u, bool& aborted) {
    const std::string path = virtual_path("units", file, "FBI");
    std::vector<std::uint8_t> data;
    if (!vfs.read(path, data)) return true;  // unopenable: record stays blank
    u.source_file = file;
    u.fbi_checksum = simple_checksum(data.data(), static_cast<std::int32_t>(data.size()));
    auto r = tdf_parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    const TdfSection* s = r.root ? r.root->child("UNITINFO") : nullptr;
    if (!s) {
        aborted = true;  // the game abandons enumeration here
        return true;
    }
    u.name = get_localized(*s, ctx.language, "name", 0x20);
    u.unit_name = s->get_string("unitname", 0x20, "");
    u.side = s->get_string("side", 0x1E, "");
    u.ai_weight = s->get_string("ai_weight", 0x40, "");
    u.ai_limit = s->get_string("ai_limit", 0x40, "");
    bool found = false;
    u.object_name = s->get_string("objectname", 0x20, "", &found);
    if (!found) u.object_name = u.unit_name;
    u.build_cost_energy = static_cast<float>(s->get_int("buildcostenergy", 0));
    u.build_cost_metal = static_cast<float>(s->get_int("buildcostmetal", 0));
    set_bit(u.can_do_flags, 15, s->get_int("norestrict", 0));
    set_bit(u.can_do_flags, 16, s->get_int("wacky", 0));
    for (const char* key : {"weapon1", "weapon2", "weapon3", "explodeas", "selfdestructas"}) {
        u.weapon_hash_xor ^= weapon_section_checksum(weapons, s->raw(key));
    }
    // Version: compatible if major < game major, or equal major and minor <= game minor.
    const double v = s->get_float("Version", 0.0);
    const double major_f = std::floor(v);
    const std::int32_t major = sim::x87::ftol32(major_f);
    const std::int32_t minor = sim::x87::ftol32(std::floor((v - static_cast<double>(major)) * 10.0));
    bool compatible = major < ctx.game_version_major || (major == ctx.game_version_major && minor <= ctx.game_version_minor);
    if (vfs.is_loose(path) && ctx.cd_check_enabled) compatible = false;
    std::string copyright = s->get_string("Copyright", 0x80, "Run to the Village!  Warn your brother!");
    if (copyright.size() >= kCopyrightYearOffset + 4) {
        copyright.replace(kCopyrightYearOffset, 4, "0000");
    } else {
        // The game writes the year digits past the string's end; it can never match then.
        copyright += '\x01';
    }
    if (copyright != kCopyrightTemplate) compatible = false;
    u.type_flags = compatible ? (u.type_flags | unit_flags::kVersionCompatible) : (u.type_flags & ~unit_flags::kVersionCompatible);
    u.max_this_unit = -1;
    return compatible;
}

void load_properties(Vfs& vfs, const UnitDefContext& ctx, CategoryRegistry& cats, const std::string& path,
                     UnitDefRecord& u, std::string& yard_buffer, FeatureDefTable* features) {
    auto root = load_tdf(vfs, path);
    const TdfSection* s = root ? root->child("UNITINFO") : nullptr;
    if (!s) return;
    u.unit_name = s->get_string("unitname", 0x20, "");
    u.name = get_localized(*s, ctx.language, "name", 0x20);
    u.description = get_localized(*s, ctx.language, "description", 0x40);
    u.default_mission = find_mission(ctx, s->get_string("defaultmissiontype", 100, ""));
    u.wpri_bad_target = s->get_string("wpri_badTargetCategory", 100, "none");
    cats.lookup_or_create(u.wpri_bad_target);
    u.wsec_bad_target = s->get_string("wsec_badTargetCategory", 100, "none");
    cats.lookup_or_create(u.wsec_bad_target);
    u.wspe_bad_target = s->get_string("wspe_badTargetCategory", 100, "none");
    cats.lookup_or_create(u.wspe_bad_target);
    u.no_chase = s->get_string("noChaseCategory", 100, "none");
    cats.lookup_or_create(u.no_chase);
    bool found = false;
    u.object_name = s->get_string("objectname", 0x20, "", &found);
    if (!found) u.object_name = u.unit_name;

    u.build_cost_energy = static_cast<float>(s->get_int("buildcostenergy", 0));
    u.build_cost_metal = static_cast<float>(s->get_int("buildcostmetal", 0));
    u.max_velocity = s->get_fixed("maxvelocity", 0);
    u.brake_rate = s->get_fixed("brakerate", 0);
    u.acceleration = s->get_fixed("acceleration", 0);
    u.bank_scale = s->get_fixed("bankscale", 0x10000);
    u.pitch_scale = s->get_fixed("pitchscale", 0);
    u.damage_modifier = s->get_fixed("damagemodifier", 0x10000);
    u.move_rate1 = s->get_fixed("moverate1", static_cast<std::int32_t>(static_cast<std::uint32_t>(u.max_velocity) << 1));
    u.move_rate2 = s->get_fixed("moverate2", static_cast<std::int32_t>(static_cast<std::uint32_t>(u.max_velocity) << 1));
    u.turn_rate = static_cast<std::uint16_t>(s->get_int("turnrate", 0));
    u.waterline = static_cast<std::uint8_t>(s->get_int("waterline", 0));
    u.transport_size = static_cast<std::uint8_t>(s->get_int("transportsize", 0));
    u.transport_capacity = static_cast<std::uint8_t>(s->get_int("transportcapacity", 0));
    u.energy_make = static_cast<float>(s->get_float("energymake", 0.0));
    u.energy_use = static_cast<float>(s->get_float("energyuse", 0.0));
    u.metal_make = static_cast<float>(s->get_float("metalmake", 0.0));
    u.extracts_metal = static_cast<float>(s->get_float("extractsmetal", 0.0));
    u.makes_metal = static_cast<std::uint8_t>(s->get_int("makesmetal", 0));
    u.wind_generator = static_cast<float>(s->get_float("windgenerator", 0.0));
    u.tidal_generator = static_cast<float>(s->get_float("tidalgenerator", 0.0));
    u.energy_storage = static_cast<float>(s->get_float("energystorage", 0.0));
    u.metal_storage = static_cast<float>(s->get_float("metalstorage", 0.0));
    u.build_time = s->get_int("buildtime", 0);
    u.worker_time = static_cast<std::int16_t>(s->get_int("workertime", 0));
    u.heal_time = static_cast<std::int16_t>(s->get_int("healtime", 0));
    u.max_damage = static_cast<std::uint32_t>(s->get_int("maxdamage", 0));
    u.sight_distance = static_cast<std::int16_t>(s->get_int("sightdistance", 0));
    u.radar_distance = static_cast<std::int16_t>(s->get_int("radardistance", 0));
    u.sonar_distance = static_cast<std::int16_t>(s->get_int("sonardistance", 0));
    u.radar_distance_jam = static_cast<std::int16_t>(s->get_int("radardistancejam", 0));
    u.sonar_distance_jam = static_cast<std::int16_t>(s->get_int("sonardistancejam", 0));
    u.bm_code = static_cast<std::uint8_t>(s->get_int("bmcode", 0));

    std::uint32_t& t = u.type_flags;
    t = (t & ~3u) | (static_cast<std::uint32_t>(s->get_int("standingmoveorder", 2)) & 3u);
    t = (t & ~0xCu) | ((static_cast<std::uint32_t>(s->get_int("standingfireorder", 2)) & 3u) << 2);
    set_bit(t, 4, s->get_int("init_cloaked", 0));
    set_bit(t, 5, s->get_int("downloadable", 0));
    set_bit(t, 6, s->get_int("builder", 0));
    set_bit(t, 8, s->get_int("stealth", 0));
    u.cloak_cost = static_cast<float>(s->get_int("cloakcost", 0));
    u.cloak_cost_moving = static_cast<float>(s->get_int("cloakcostmoving", sim::x87::ftol32(u.cloak_cost)));
    u.min_cloak_distance = static_cast<std::int16_t>(s->get_int("mincloakdistance", 0));
    u.build_angle = static_cast<std::int16_t>(s->get_int("buildangle", 0));
    u.build_distance = static_cast<std::int16_t>(s->get_int("builddistance", 0));
    u.sort_bias = static_cast<std::int16_t>(s->get_int("sortbias", 0));
    u.cruise_alt = static_cast<std::int16_t>(s->get_int("cruisealt", 0));
    set_bit(t, 7, s->get_int("zbuffer", 0));
    set_bit(t, 9, s->get_int("isairbase", 0));
    set_bit(t, 10, s->get_int("istargetingupgrade", 0));
    set_bit(t, 13, s->get_int("teleporter", 0));
    set_bit(t, 14, s->get_int("hidedamage", 0));
    set_bit(t, 15, s->get_int("shootme", 0));
    set_bit(t, 17, s->get_int("armoredstate", 0));
    set_bit(t, 18, s->get_int("activatewhenbuilt", 0));
    set_bit(t, 11, s->get_int("canfly", 0));
    set_bit(t, 12, s->get_int("canhover", 0));
    set_bit(t, 20, s->get_int("upright", 0));
    set_bit(t, 19, s->get_int("floater", 0));
    set_bit(t, 21, s->get_int("amphibious", 0));
    set_bit(t, 24, s->get_int("isfeature", 0));
    set_bit(t, 25, s->get_int("noshadow", 0));
    set_bit(t, 26, s->get_int("immunetoparalyzer", 0));
    set_bit(t, 27, s->get_int("hoverattack", 0));
    set_bit(t, 29, s->get_int("antiweapons", 0));
    set_bit(t, 30, s->get_int("digger", 0));

    std::uint32_t& c = u.can_do_flags;
    set_bit(c, 2, s->get_int("onoffable", 0));
    set_bit(c, 0, s->get_int("mobilestandorders", 0));
    set_bit(c, 1, s->get_int("firestandorders", 0));
    set_bit(c, 3, s->get_int("canstop", 0));
    set_bit(c, 4, s->get_int("canattack", 0));
    set_bit(c, 5, s->get_int("canguard", 0));
    set_bit(c, 6, s->get_int("canpatrol", 0));
    set_bit(c, 7, s->get_int("canmove", 0));
    set_bit(c, 8, s->get_int("canload", 0));
    set_bit(c, 10, s->get_int("canreclamate", 0));
    {
        // canresurrect also copies CANRECLAMATE (bit 10) into CANREPAIR (bit 9).
        const std::uint32_t res = static_cast<std::uint32_t>(s->get_int("canresurrect", 0));
        c = ((c & (1u << 10)) >> 1) | ((res & 1u) << 11) | (c & ~((1u << 11) | (1u << 9)));
    }
    set_bit(c, 12, s->get_int("cancapture", 0));
    c = (c & ~unit_flags::kCanCloakFromCost) | ((u.cloak_cost > 0.0f) ? unit_flags::kCanCloakFromCost : 0u);
    set_bit(c, 14, s->get_int("candgun", 0));
    u.maneuver_leash_length = static_cast<std::int16_t>(s->get_int("maneuverleashlength", 0));
    u.attack_run_length = static_cast<std::int16_t>(s->get_int("attackrunlength", 0));
    set_bit(t, 28, s->get_int("kamikaze", 0));
    u.kamikaze_distance = static_cast<std::int16_t>(s->get_int("kamikazedistance", 0));
    set_bit(c, 15, s->get_int("norestrict", 0));
    set_bit(c, 17, s->get_int("showplayername", 0));
    set_bit(c, 18, s->get_int("commander", 0));
    set_bit(c, 19, s->get_int("cantbetransported", 0));
    if (const std::string* sd = s->raw("selfdestructcountdown")) {
        c = (c & ~(7u << 20)) | ((static_cast<std::uint32_t>(msvc_atoi(*sd)) & 7u) << 20);
    } else {
        c = (c & 0xFFDFFFFFu) | 0x500000u;
    }

    const std::string category = s->get_string("category", 100, "");
    for (const auto& tok : split_ws(category)) apply_category(cats, tok, u.index);
    apply_category(cats, "ALL", u.index);

    bool has_sound = false;
    const std::string sound = s->get_string("soundcategory", 100, "", &has_sound);
    u.sound_category = 0;
    if (has_sound) {
        std::int16_t idx = -1;
        for (std::size_t i = 0; i < ctx.sound_categories.size(); ++i) {
            if (stricmp_c(ctx.sound_categories[i], sound) == 0) {
                idx = static_cast<std::int16_t>(i);
                break;
            }
        }
        u.sound_category = idx >= 0 ? idx : static_cast<std::int16_t>(msvc_atoi(sound));
    }

    u.corpse = s->get_string("corpse", 100, "", &u.has_corpse);
    // The corpse is looked up (or appended) in the battle's feature table right away.
    if (u.has_corpse && features) u.corpse_feature_id = static_cast<std::int16_t>(features->find_or_load(u.corpse));

    u.movement_class = -1;
    bool has_mc = false;
    const std::string mc = s->get_string("movementclass", 100, "", &has_mc);
    if (has_mc) {
        for (int i = 0; i < 32; ++i) {
            const auto& n = ctx.movement_class_names[static_cast<std::size_t>(i)];
            if (!n.empty() && stricmp_c(n, mc) == 0) {
                u.movement_class = i;
                break;
            }
        }
    }
    UnitDefContext::MoveInfo local;
    const UnitDefContext::MoveInfo* m = nullptr;
    if (u.movement_class >= 0) {
        m = &ctx.movement_classes[static_cast<std::size_t>(u.movement_class)];
    } else {
        load_move_info(*s, local);
        m = &local;
    }
    u.footprint_x = m->footprint_x;
    u.footprint_z = m->footprint_z;
    u.max_water_depth = m->max_water_depth;
    u.min_water_depth = m->min_water_depth;
    u.max_slope = m->max_slope;
    u.max_water_slope = m->max_water_slope;
    u.scaled_max_velocity = static_cast<std::int32_t>(
        (static_cast<std::int64_t>(u.max_velocity) * 65536) / (static_cast<std::int64_t>(u.max_slope + 1) * 65536));

    auto weapon_slot = [&](const char* key) {
        const int w = find_weapon(ctx, s->get_string(key, 0x80, ""));
        return w < 0 ? 0 : w;
    };
    u.weapon1 = weapon_slot("weapon1");
    u.weapon2 = weapon_slot("weapon2");
    u.weapon3 = weapon_slot("weapon3");
    u.explode_as = weapon_slot("explodeas");
    u.self_destruct_as = weapon_slot("selfdestructas");
    if (u.weapon1 == 0 && u.weapon2 == 0 && u.weapon3 == 0) t &= ~unit_flags::kHasWeapons;
    else t |= unit_flags::kHasWeapons;

    u.has_yard_map = false;
    u.yard_map.clear();
    if (u.bm_code == 0) {
        // The game reads the key into a 1024-byte stack buffer: strncpy zero-pads when
        // the key exists; when it is missing only a NUL is written and the rest of the
        // buffer keeps whatever an earlier call left there (modelled by yard_buffer).
        if (const std::string* ym = s->raw("YardMap")) {
            yard_buffer.assign(1024, '\0');
            std::memcpy(yard_buffer.data(), ym->data(), std::min<std::size_t>(ym->size(), 1023));
        } else {
            yard_buffer.resize(1024, '\0');
            yard_buffer[0] = '\0';
        }
        u.has_yard_map = true;
        const int fx = u.footprint_x, fz = u.footprint_z;
        u.yard_map.assign(static_cast<std::size_t>(std::max(0, fx * fz)), 0);
        std::size_t p = 0, cell = 0;
        for (int row = 0; row < fz; ++row) {
            int col = 0;
            while (col < fx) {
                const char ch = p < yard_buffer.size() ? yard_buffer[p] : '\0';
                std::uint8_t v = 0;
                bool known = true;
                switch (ch) {
                case '.': v = 0x00; break;
                case 'C': v = 0x35; break;
                case 'G': v = 0x8F; break;
                case 'O': v = 0x2B; break;
                case 'Y': v = 0x31; break;
                case 'c': v = 0x2D; break;
                case 'f': v = 0x6F; break;
                case 'o': v = 0x2F; break;
                case 'w': v = 0x37; break;
                case 'y': v = 0x29; break;
                default: known = false; break;
                }
                if (!known) {
                    ++p;  // unrecognised characters are skipped
                    if (p > yard_buffer.size() + 4096) return;  // safety: the original would read stack garbage
                    continue;
                }
                if (cell < u.yard_map.size()) u.yard_map[cell] = v;
                ++col;
                ++cell;
                const char next = p + 1 < yard_buffer.size() ? yard_buffer[p + 1] : '\0';
                if (next != '\0') ++p;  // at the end the last character repeats
            }
        }
    }

    u.model_min_x = (u.footprint_x * -0x100000) / 2;
    u.model_min_z = (u.footprint_z * -0x100000) / 2;
    u.model_max_x = (static_cast<std::int32_t>(u.footprint_x) << 20) / 2;
    u.model_max_z = (static_cast<std::int32_t>(u.footprint_z) << 20) / 2;
    u.model_size_x = u.model_max_x - u.model_min_x;
    u.model_size_y = u.model_max_y - u.model_min_y;
    u.model_size_z = u.model_max_z - u.model_min_z;
    u.model_avg_size = (u.model_size_z + u.model_size_x) / 3;

    if ((c & unit_flags::kCanCloakFromCost) && u.min_cloak_distance == 0) u.min_cloak_distance = 0x50;
}

} // namespace

std::vector<std::string> UnitDefContext::default_mission_names() {
    // Index = mission type number. Entry 0 ("Ready") has an empty lookup name.
    return {"",
            "Activate", "AirStrike", "AirToAir", "AirToGround", "AirToGroundHover", "Attack_Chase", "Attack_Kamikaze",
            "Attack_NoMove", "AttackSpecial", "AttackUType", "BeCarried", "BuildingBuild", "BuildWeapon", "Capture",
            "Cloak_Off", "Cloak_On", "Deactivate", "Follow_Ground", "GetBuilt", "Ground_Pickup", "Ground_Unload",
            "Guard_NoMove", "HelpBuild", "MakeSelectable", "MobileBuild", "Move_Ground", "Paralyze", "Park", "Patrol",
            "QMove", "QPatrol", "Reclaim", "ReclaimUnit", "RepairPatrol", "RepairUnit", "RepairUnitNoMove", "Resurrect",
            "SelfDestruct", "SelfDestructFG", "SelfRepair", "Standby", "Standby_Mine", "Standing_FireOrder",
            "Standing_MoveOrder", "Stop", "Suppress", "Teleport", "VTOL_Evade", "VTOL_Follow", "VTOL_GetRepaired",
            "VTOL_HelpBuild", "VTOL_LandIfCan", "VTOL_Landing", "VTOL_MobileBuild", "VTOL_Move", "VTOL_Patrol",
            "VTOL_Pickup", "VTOL_Reclaim", "VTOL_ReclaimUnit", "VTOL_RepairPatrol", "VTOL_RepairUnit",
            "VTOL_SeekAttack", "VTOL_SeekGuard", "VTOL_Standby", "VTOL_Unload", "Wait", "WaitForAttack"};
}

std::array<std::uint32_t, 16>& CategoryRegistry::lookup_or_create(const std::string& name) {
    auto& e = masks_[lower(name)];
    if (e.first.empty() && !name.empty()) e.first = name;
    return e.second;
}

const std::array<std::uint32_t, 16>* CategoryRegistry::find(const std::string& name) const {
    const auto it = masks_.find(lower(name));
    return it == masks_.end() ? nullptr : &it->second.second;
}

void load_unit_context(Vfs& vfs, UnitDefContext& ctx) {
    ctx.weapon_names.fill("");
    for (const auto& name : vfs.enumerate("Weapons\\*.tdf")) {
        bool loose = false;
        auto root = load_tdf(vfs, virtual_path("Weapons", name, "TDF"), &loose);
        if (!root || (loose && ctx.cd_check_enabled)) continue;
        for (const auto& sec : root->children()) {
            const std::int32_t id = sec->get_int("ID", -1);
            if (id >= 0 && id < 256) ctx.weapon_names[static_cast<std::size_t>(id)] = sec->name();
        }
    }

    ctx.movement_class_names.fill("");
    ctx.movement_classes.fill(UnitDefContext::MoveInfo{});
    if (auto root = load_tdf(vfs, "gamedata\\moveinfo.TDF")) {
        for (int i = 0; i < 32; ++i) {
            const TdfSection* s = root->child("CLASS" + std::to_string(i));
            if (!s) continue;
            ctx.movement_class_names[static_cast<std::size_t>(i)] = s->get_string("name", 100, "");
            load_move_info(*s, ctx.movement_classes[static_cast<std::size_t>(i)]);
        }
    }

    ctx.sound_categories.clear();
    if (auto root = load_tdf(vfs, "gamedata\\sound.TDF")) {
        for (const auto& sec : root->children()) ctx.sound_categories.push_back(truncate(sec->name(), 0x40));
    }
}

UnitDefTable load_unit_defs(Vfs& vfs, const UnitDefContext& ctx, FeatureDefTable* features) {
    UnitDefTable table;
    const WeaponFiles weapons = load_weapon_files(vfs, ctx);
    const auto files = vfs.enumerate("units\\*.FBI");

    // Record 0 is zero-filled apart from its unit name and the compatible flag.
    UnitDefRecord none;
    none.unit_name = "None";
    none.type_flags = unit_flags::kVersionCompatible;
    none.weapon1 = none.weapon2 = none.weapon3 = none.explode_as = none.self_destruct_as = -1;
    none.corpse_feature_id = 0;
    std::vector<UnitDefRecord> units;
    units.push_back(none);
    bool aborted = false;
    for (std::size_t i = 0; i < files.size() && !aborted; ++i) {
        UnitDefRecord u;
        u.index = static_cast<std::uint16_t>(i + 1);
        load_stub(vfs, ctx, weapons, files[i], u, aborted);
        units.push_back(std::move(u));
    }
    if (aborted) table.notes.push_back("an FBI without [UNITINFO] aborted enumeration (game behaviour)");

    // Drop incompatible units, then sort the rest by unit name (case-insensitive).
    std::vector<UnitDefRecord> kept;
    kept.push_back(units[0]);
    for (std::size_t i = 1; i < units.size(); ++i) {
        if (units[i].type_flags & unit_flags::kVersionCompatible) kept.push_back(std::move(units[i]));
        else table.notes.push_back("incompatible: " + units[i].source_file);
    }
    std::stable_sort(kept.begin() + 1, kept.end(),
                     [](const UnitDefRecord& a, const UnitDefRecord& b) { return stricmp_c(a.unit_name, b.unit_name) < 0; });
    for (std::size_t i = 2; i < kept.size(); ++i) {
        if (stricmp_c(kept[i - 1].unit_name, kept[i].unit_name) == 0)
            table.notes.push_back("duplicate unit name (order not yet reproduced exactly): " + kept[i].unit_name);
    }
    for (std::size_t i = 0; i < kept.size(); ++i) kept[i].index = static_cast<std::uint16_t>(i);

    std::string yard_buffer(1024, '\0');
    for (std::size_t i = 1; i < kept.size(); ++i) {
        UnitDefRecord& u = kept[i];
        const std::string path = virtual_path("units", u.unit_name, "FBI");
        if (vfs.size_or_zero(path) != 0) load_properties(vfs, ctx, table.categories, path, u, yard_buffer, features);
        {
            // Model: objects3d\<objectname>.3DO; height becomes the Y extent.
            const std::string model_path = virtual_path("objects3d", truncate(u.object_name, 0x20), "3DO");
            std::vector<std::uint8_t> data;
            if (vfs.read(model_path, data)) {
                const auto m = load_3do(data, true);
                if (!m.error.empty()) table.notes.push_back("model " + model_path + ": " + m.error);
                u.model_min_y = 0;
                u.model_max_y = m.root ? model_height(m.root.get()) : 0;
                u.model_size_y = u.model_max_y - u.model_min_y;
                u.model_pieces = m.root ? model_piece_count(m.root.get()) : 0;
            } else {
                table.notes.push_back("missing model (the game aborts): " + model_path);
            }
        }
        const std::string base = strip_extension(u.unit_name);
        const bool gui0 = vfs.size_or_zero(virtual_path("guis", base + "0", "GUI")) != 0;
        u.type_flags = gui0 ? (u.type_flags | unit_flags::kHasGui0) : (u.type_flags & ~unit_flags::kHasGui0);
        std::uint8_t pages = 1;
        bool any = false;
        while (vfs.size_or_zero(virtual_path("guis", base + std::to_string(pages), "GUI")) != 0) {
            ++pages;
            any = true;
        }
        u.build_menu_page_count = any ? pages : ((u.type_flags & unit_flags::kHasGui0) ? 1 : 0);
    }

    // Build options from gamedata\sidedata.tdf [CANBUILD] -> [<unitname>] canbuild1..N.
    // The 30-entry scratch list is shared by all builders and never cleared.
    std::array<std::uint16_t, 30> scratch{};
    auto sidedata = load_tdf(vfs, "gamedata\\sidedata.TDF");
    auto find_id = [&](const std::string& name) -> std::uint16_t {
        std::size_t lo = 1, hi = kept.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (stricmp_c(kept[mid].unit_name, name) < 0) lo = mid + 1;
            else hi = mid;
        }
        if (lo != kept.size() && stricmp_c(name, kept[lo].unit_name) == 0) return kept[lo].index;
        return 0;
    };
    for (std::size_t i = 1; i < kept.size(); ++i) {
        UnitDefRecord& u = kept[i];
        u.build_options_count = 0;
        u.has_build_options = false;
        if (!(u.type_flags & (1u << 6))) continue;
        int count = 0;
        const TdfSection* cb = sidedata ? sidedata->child("CANBUILD") : nullptr;
        const TdfSection* us = cb ? cb->child(u.unit_name) : nullptr;
        if (us) {
            for (int k = 1;; ++k) {
                bool found = false;
                const std::string name = us->get_string("canbuild" + std::to_string(k), 0x20, "", &found);
                if (!found) break;
                const std::uint16_t id = find_id(name);
                if (id != 0) {
                    if (count < 30) scratch[static_cast<std::size_t>(count)] = id;  // beyond 30 the game overruns its heap block
                    ++count;
                }
            }
            u.build_options_count = count;
        }
        u.has_build_options = true;
        u.build_options = scratch;
    }

    // Download menus (download\*.TDF), applied after the units are loaded.
    // The game allocates 0xBD bytes per file: a slot count followed by room for 5
    // slots of 0x25 bytes {u16 unit index, u8 menu, u8 button, char[32] unit name}.
    // Files with more sections write past their block into the next file's block;
    // that overlap is reproduced by working on the same flat byte buffer.
    {
        const auto menu_files = vfs.enumerate("download\\*.TDF");
        const std::size_t file_stride = 0xBD, slot_stride = 0x25;
        std::vector<std::uint8_t> buf(menu_files.size() * file_stride + 0x4000, 0);
        std::vector<bool> written(buf.size(), false);
        auto put = [&](std::size_t off, const void* src, std::size_t n) {
            if (off + n > buf.size()) {
                table.notes.push_back("download menu write beyond modelled buffer");
                return;
            }
            std::memcpy(&buf[off], src, n);
            for (std::size_t k = 0; k < n; ++k) written[off + k] = true;
        };
        auto rd16 = [&](std::size_t off, bool& clean) {
            clean = clean && off + 2 <= buf.size() && written[off] && written[off + 1];
            return off + 2 <= buf.size() ? static_cast<std::uint16_t>(buf[off] | (buf[off + 1] << 8)) : std::uint16_t{0};
        };
        auto rd32 = [&](std::size_t off) {
            std::uint32_t v = 0;
            if (off + 4 <= buf.size()) std::memcpy(&v, &buf[off], 4);
            return v;
        };
        for (std::size_t fi = 0; fi < menu_files.size(); ++fi) {
            auto root = load_tdf(vfs, virtual_path("download", menu_files[fi], "TDF"));
            if (!root) continue;
            const std::size_t base = fi * file_stride;
            std::uint32_t count = 0;
            std::size_t slot = 0;
            for (const auto& sec : root->children()) {
                ++count;
                put(base, &count, 4);
                bool found = false;
                const std::string menu = sec->get_string("UNITMENU", 0x20, "", &found);
                if (found) {
                    for (std::size_t ui = 0; ui < kept.size(); ++ui) {
                        if (stricmp_c(kept[ui].unit_name, menu) == 0) {
                            const std::size_t so = base + 4 + slot;
                            const auto idx = static_cast<std::uint16_t>(ui);
                            put(so, &idx, 2);
                            const auto m = static_cast<std::uint8_t>(sec->get_int("MENU", 0));
                            const auto b = static_cast<std::uint8_t>(sec->get_int("BUTTON", 0));
                            put(so + 2, &m, 1);
                            put(so + 3, &b, 1);
                            char name[32] = {};
                            const std::string un = sec->get_string("UNITNAME", 0x20, "");
                            std::memcpy(name, un.data(), un.size());
                            put(so + 4, name, 32);
                            break;
                        }
                    }
                }
                slot += slot_stride;
            }
        }
        bool clean = true;
        // Raise build menu page counts.
        for (std::size_t ui = 0; ui < kept.size(); ++ui) {
            for (std::size_t fi = 0; fi < menu_files.size(); ++fi) {
                const std::size_t base = fi * file_stride;
                const std::uint32_t n = rd32(base);
                for (std::uint32_t k = 0; k < n; ++k) {
                    const std::size_t so = base + 4 + k * slot_stride;
                    if (rd16(so, clean) == ui && so + 2 < buf.size() && kept[ui].build_menu_page_count < buf[so + 2])
                        kept[ui].build_menu_page_count = buf[so + 2];
                }
            }
        }
        // DOWNLOADABLE for units named in slot 0 of any file.
        for (auto& u : kept) {
            for (std::size_t fi = 0; fi < menu_files.size(); ++fi) {
                const char* nm = reinterpret_cast<const char*>(&buf[fi * file_stride + 4 + 4]);
                if (stricmp_c(std::string(nm, strnlen(nm, 32)), u.unit_name) == 0) u.type_flags |= 1u << 5;
            }
        }
        // Append download build options to builders (count < 31 allowed: one past the buffer).
        for (std::size_t ui = 0; ui < kept.size(); ++ui) {
            UnitDefRecord& u = kept[ui];
            if (!u.has_build_options) continue;
            for (std::size_t fi = 0; fi < menu_files.size(); ++fi) {
                const std::size_t base = fi * file_stride;
                const std::uint32_t n = rd32(base);
                for (std::uint32_t k = 0; k < n; ++k) {
                    const std::size_t so = base + 4 + k * slot_stride;
                    if (rd16(so, clean) != ui || u.build_options_count >= 31) continue;
                    const char* nm = reinterpret_cast<const char*>(&buf[so + 4]);
                    const std::uint16_t id = find_id(std::string(nm, strnlen(nm, 32)));
                    if (id == 0) continue;
                    if (u.build_options_count < 30) u.build_options[static_cast<std::size_t>(u.build_options_count)] = id;
                    else table.notes.push_back("download build option written past the 30-entry list: " + u.unit_name);
                    ++u.build_options_count;
                }
            }
        }
        // The entries as the build menus read them (only those whose slot was written: an entry
        // whose UNITMENU names no unit keeps the allocator's bytes, see the note).
        for (std::size_t fi = 0; fi < menu_files.size(); ++fi) {
            const std::size_t base = fi * file_stride;
            const std::uint32_t n = rd32(base);
            for (std::uint32_t k = 0; k < n; ++k) {
                const std::size_t so = base + 4 + k * slot_stride;
                bool slot_clean = true;
                const std::uint16_t ui = rd16(so, slot_clean);
                if (!slot_clean || so + 4 + 32 > buf.size()) continue;
                const char* nm = reinterpret_cast<const char*>(&buf[so + 4]);
                table.download_menus.push_back({ui, buf[so + 2], buf[so + 3], std::string(nm, strnlen(nm, 32))});
            }
        }
        if (!clean) table.notes.push_back("download menus read uninitialised slot memory (game: heap garbage)");
    }

    table.units = std::move(kept);
    return table;
}

} // namespace ta
