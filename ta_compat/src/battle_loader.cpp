#include "ta/battle_loader.h"

#include <algorithm>

#include "ta/ai_profile.h"
#include "ta/battle_economy.h"
#include "ta/battle_features.h"
#include "ta/sight_tables.h"
#include "ta/tdf.h"
#include "ta/tnt.h"

namespace ta {

std::uint16_t LoadedBattle::type_index(const std::string& unit_name) const {
    for (std::size_t k = 1; k < type_defs.size(); ++k)
        if (type_defs[k] && stricmp_c(type_defs[k]->unit_name, unit_name) == 0) return static_cast<std::uint16_t>(k);
    return 0;
}

namespace {

std::unique_ptr<TdfSection> parse_file(Vfs& vfs, const std::string& path) {
    std::vector<std::uint8_t> bytes;
    if (!vfs.read(path, bytes)) return nullptr;
    TdfParseResult r = tdf_parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    return std::move(r.root);
}

// The commander unit name of a side, from gamedata\sidedata.tdf ([SIDEn] name / commander).
std::string side_commander(const TdfSection* sides, const std::string& side) {
    if (!sides) return {};
    for (const auto& s : sides->children()) {
        bool found = false;
        const std::string name = s->get_string("name", 32, "", &found);
        if (found && stricmp_c(name, side) == 0) return s->get_string("commander", 32, "");
    }
    return {};
}

// The schema the original picks (0x436860) for a skirmish or network game: among
// "Schema N" sections (N = 0, 1, ... until one is missing) of type "Network 1".."Network 4"
// (types in that order), the one whose StartPos count equals the player count (the highest
// filled slot + 1), else the largest count seen; later candidates replace earlier ones under
// the same rule. Returns the schema number (-1: none).
int select_schema(const TdfSection& header, int players) {
    static const char* const kTypes[4] = {"Network 1", "Network 2", "Network 3", "Network 4"};
    int chosen = -1, best = 0;
    for (const char* type : kTypes)
        for (int n = 0;; ++n) {
            const TdfSection* s = header.child("Schema " + std::to_string(n));
            if (!s) break;
            if (stricmp_c(s->get_string("Type", 32, ""), type) != 0) continue;
            int count = 0;
            if (const TdfSection* sp = s->child("specials"))
                for (const auto& c : sp->children()) {
                    bool found = false;
                    const std::string what = c->get_string("specialwhat", 16, "", &found);
                    if (found && what.size() >= 8 && stricmp_c(what.substr(0, 8), "StartPos") == 0) ++count;
                }
            if (count != 0 && (count == players || players == 0 || (best < count && best != players))) {
                chosen = n;
                best = count;
            }
        }
    return chosen;
}

// Start positions of a schema (0x437320's table): StartPosN gives id N-1; a StartPos
// without digits takes the next of a running count (1, 2, ...) minus one.
void read_start_rules(const TdfSection& schema, std::vector<sim::StartRule>& out) {
    const TdfSection* sp = schema.child("specials");
    if (!sp) return;
    std::int32_t running = 0;
    for (const auto& c : sp->children()) {
        bool found = false;
        const std::string what = c->get_string("specialwhat", 256, "", &found);
        if (!found || what.size() < 8 || stricmp_c(what.substr(0, 8), "StartPos") != 0) continue;
        sim::StartRule r;
        r.x = static_cast<std::int16_t>(c->get_int("XPos", 0));
        r.z = static_cast<std::int16_t>(c->get_int("ZPos", 0));
        std::int32_t id;
        if (what.size() > 8 && what[8] >= '0' && what[8] <= '9') id = msvc_atoi(what.substr(8));
        else id = ++running;
        r.id = id > 0 ? id - 1 : id;
        out.push_back(r);
    }
}

// The meteor configuration: the schema's MeteorWeapon/Radius/Density/Duration/Interval
// (enabled), or gamedata\meteor.tdf [Default] (disabled) when there is no weapon name; a
// schema with a zero value also takes the default file's values (still enabled).
void read_meteors(Vfs& vfs, const TdfSection* schema, sim::MeteorShower& m, std::string& weapon) {
    auto from = [&](const TdfSection& s) {
        weapon = s.get_string("MeteorWeapon", 32, "");
        const std::int32_t radius = s.get_int("MeteorRadius", 0);
        const float density = static_cast<float>(s.get_float("MeteorDensity", 0.0));
        const float duration = static_cast<float>(s.get_float("MeteorDuration", 0.0));
        const float interval = static_cast<float>(s.get_float("MeteorInterval", 0.0));
        m.configure(radius, density, duration, interval);
        return radius != 0 && density != 0.0f && duration != 0.0f && interval != 0.0f;
    };
    auto from_default = [&] {
        const std::unique_ptr<TdfSection> d = parse_file(vfs, "gamedata\\meteor.tdf");
        if (const TdfSection* sec = d ? d->child("Default") : nullptr) from(*sec);
    };
    const std::string name = schema ? schema->get_string("MeteorWeapon", 32, "") : std::string();
    if (name.empty()) {
        m.enabled = false;
        from_default();
        return;
    }
    if (!from(*schema)) from_default();
    m.enabled = true;
}

}  // namespace

std::unique_ptr<LoadedBattle> load_battle(Vfs& vfs, const BattleRequest& req, sim::BattleOutside& outside, std::string& error,
                                          const std::function<void(sim::Battle&)>& before_start) {
    auto lb = std::make_unique<LoadedBattle>();
    if (!vfs.read("maps\\" + req.map + ".tnt", lb->tnt)) {
        error = "map not found: " + req.map;
        return nullptr;
    }
    vfs.read("maps\\" + req.map + ".ota", lb->ota);
    TntInfo info;
    if (!tnt_load_heights(lb->tnt, lb->heights, &info)) {
        error = "bad map file";
        return nullptr;
    }
    lb->weapon_defs = load_weapon_defs(vfs, WeaponDefOptions{});
    std::array<std::string, 256> weapon_names;
    for (std::size_t i = 0; i < 256; ++i) weapon_names[i] = lb->weapon_defs.slots[i].name;
    lb->feature_defs.load_library(vfs, weapon_names);
    std::vector<std::string> tnt_names;
    tnt_feature_names(lb->tnt, tnt_names);
    lb->feature_defs.load_map_features(tnt_names);
    load_unit_context(vfs, lb->unit_context);
    lb->unit_defs = load_unit_defs(vfs, lb->unit_context, &lb->feature_defs);
    lb->feature_defs.resolve_chains();

    lb->weapons = to_weapon_types(lb->weapon_defs);
    lb->battle = std::make_unique<sim::Battle>(outside, lb->heights, lb->rng);
    sim::Battle& b = *lb->battle;
    lb->anims = std::make_unique<AnimImporter>(vfs, b.anims);
    bind_weapon_explosions(lb->weapon_defs, lb->weapons, *lb->anims);
    b.features = to_feature_types(lb->feature_defs, lb->weapons, *lb->anims, &lb->feature_models);
    bind_feature_blocks(b);
    load_effect_art(*lb->anims, b.effect_art);
    // Particle art the effect lists count frames of (fx.gaf "smoke 1", "smoke 2", "flamestream").
    auto frames = [&](const char* seq) -> std::int32_t {
        const sim::AnimSequence* a = b.anims.find(lb->anims->sequence("shared", "fx", seq, false));
        return a ? static_cast<std::int32_t>(a->frames.size()) : 0;
    };
    b.fx_lists.smoke1_frames = frames("smoke 1");
    b.fx_lists.smoke2_frames = frames("smoke 2");
    b.fx_lists.flame_frames = frames("flamestream");
    b.settings.sea_level = static_cast<std::uint8_t>(info.sea_level);
    b.settings.gravity = 112;
    b.settings.damage_options = 8;

    // Unit types: battle type index = definition index ([0] is the blank "None").
    const std::size_t ntypes = lb->unit_defs.units.size();
    lb->type_assets.resize(ntypes);
    lb->type_defs.assign(ntypes, nullptr);
    b.types.resize(ntypes);
    for (std::size_t k = 1; k < ntypes; ++k) {
        const UnitDefRecord& d = lb->unit_defs.units[k];
        lb->type_defs[k] = &d;
        auto assets = std::make_unique<UnitTypeAssets>();
        std::string err;
        if (!load_unit_assets(vfs, d, *assets, err)) continue;
        to_battle_unit_type(d, *assets, lb->weapons, b.types[k]);
        to_targeting_type(d, lb->unit_defs.categories, lb->weapons, b.types[k].targeting);
        lb->type_assets[k] = std::move(assets);
    }
    for (std::size_t k = 1; k < ntypes; ++k) to_ai_type(lb->unit_defs.units[k], lb->weapons, b.types[k].ai);
    {
        std::vector<std::string> type_names(ntypes);
        for (std::size_t k = 1; k < ntypes; ++k) type_names[k] = lb->unit_defs.units[k].unit_name;
        bind_resurrect_types(b.features, lb->feature_defs, type_names);
    }

    // The map grid, features, economy.
    b.map.width = lb->heights.width();
    b.map.height = lb->heights.height();
    b.map.cells.assign(static_cast<std::size_t>(b.map.width) * static_cast<std::size_t>(b.map.height), sim::MapCell{});
    for (std::int32_t z = 0; z < b.map.height; ++z)
        for (std::int32_t x = 0; x < b.map.width; ++x) b.map.cells[static_cast<std::size_t>(z * b.map.width + x)].height = lb->heights.at(x, z);
    sim::refresh_height_range(b.map, 0, 0, static_cast<std::int16_t>(b.map.width), static_cast<std::int16_t>(b.map.height));
    b.init_buckets();
    b.reset_features_and_effects();
    place_tnt_features(b, lb->tnt);
    // The map info's schema: chosen as the original does for skirmish and network games
    // (request.schema < 0), else the requested one.
    std::unique_ptr<TdfSection> ota_root;
    if (!lb->ota.empty()) ota_root = tdf_parse(std::string_view(reinterpret_cast<const char*>(lb->ota.data()), lb->ota.size())).root;
    const TdfSection* header = ota_root ? ota_root->child("GlobalHeader") : nullptr;
    int schema = req.schema;
    if (schema < 0 && header) {
        int players = 0;
        for (int s = 0; s < 10; ++s)
            if (req.start.players[static_cast<std::size_t>(s)].kind != sim::PlayerKind::Empty) players = s + 1;
        schema = select_schema(*header, players);
    }
    const TdfSection* schema_sec = header && schema >= 0 ? header->child("Schema " + std::to_string(schema)) : nullptr;
    MapEconomy me;
    if (!lb->ota.empty()) read_map_economy(lb->ota, schema < 0 ? 0 : schema, me);
    init_map_economy(b, lb->tnt, me, lb->feature_defs);
    std::string meteor_weapon;
    read_meteors(vfs, schema_sec, b.meteors, meteor_weapon);
    // The meteor weapon by name; a missing one or one without the meteor flag: weapon slot 0.
    b.meteors.weapon = &lb->weapons[0];
    for (std::size_t w = 0; w < 256 && !meteor_weapon.empty(); ++w)
        if (stricmp_c(lb->weapon_defs.slots[w].name, meteor_weapon) == 0) {
            if (lb->weapon_defs.slots[w].flags & 0x20u) b.meteors.weapon = &lb->weapons[w];
            break;
        }
    b.move_classes = to_move_classes(lb->unit_context);
    std::string err;
    load_sight_tables(vfs, b.sight_tables, err);

    // Players.
    sim::BattleStart start = req.start;
    if (schema_sec && start.start_rules.empty()) read_start_rules(*schema_sec, start.start_rules);
    const std::unique_ptr<TdfSection> sides = parse_file(vfs, "gamedata\\sidedata.tdf");
    for (std::size_t s = 0; s < 10; ++s) {
        sim::StartPlayer& p = start.players[s];
        if (p.kind == sim::PlayerKind::Empty || p.commander_type) continue;
        const std::string com = side_commander(sides.get(), req.sides[s]);
        p.commander_type = lb->type_index(com);
        if (p.commander_name.empty()) p.commander_name = com;
    }
    const int profile_schema = schema < 0 ? 0 : schema;
    start.after_player_setup = [&vfs, &lb, profile_schema](sim::Battle& battle) {
        AiProfileReport report;
        load_ai_profile(vfs, ai_profile_path(lb->ota, profile_schema), lb->unit_defs, battle.ai(), report);
    };
    if (before_start) before_start(b);
    if (!sim::start_battle(b, start, error)) return nullptr;
    return lb;
}

}  // namespace ta
