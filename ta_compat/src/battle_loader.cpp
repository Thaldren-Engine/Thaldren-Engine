#include "ta/battle_loader.h"

#include <algorithm>

#include "ta/battle_economy.h"
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

// Start positions of a schema: [GlobalHeader] [Schema n] [specials] [specialK] with
// specialwhat=StartPosN, XPos, ZPos (world units).
int read_start_positions(const std::vector<std::uint8_t>& ota, int schema, std::array<std::array<std::int32_t, 2>, 10>& out) {
    const TdfParseResult parsed = tdf_parse(std::string_view(reinterpret_cast<const char*>(ota.data()), ota.size()));
    if (!parsed.root) return 0;
    const TdfSection* h = parsed.root->child("GlobalHeader");
    const TdfSection* s = h ? h->child("Schema " + std::to_string(schema)) : nullptr;
    const TdfSection* sp = s ? s->child("specials") : nullptr;
    if (!sp) return 0;
    int count = 0;
    for (const auto& c : sp->children()) {
        const std::string what = c->get_string("specialwhat", 64, "");
        if (what.size() < 9 || stricmp_c(what.substr(0, 8), "StartPos") != 0) continue;
        const int n = msvc_atoi(what.substr(8));
        if (n < 1 || n > 10) continue;
        out[static_cast<std::size_t>(n - 1)] = {c->get_int("XPos", 0), c->get_int("ZPos", 0)};
        count = std::max(count, n);
    }
    return count;
}

}  // namespace

std::unique_ptr<LoadedBattle> load_battle(Vfs& vfs, const BattleRequest& req, sim::BattleOutside& outside, std::string& error) {
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
    load_effect_art(*lb->anims, b.effect_art);
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
    MapEconomy me;
    if (!lb->ota.empty()) read_map_economy(lb->ota, req.schema, me);
    init_map_economy(b, lb->tnt, me, lb->feature_defs);
    b.step_wind();
    b.move_classes = to_move_classes(lb->unit_context);
    std::string err;
    load_sight_tables(vfs, b.sight_tables, err);

    // Players.
    sim::BattleStart start = req.start;
    if (!lb->ota.empty()) start.start_position_count = read_start_positions(lb->ota, req.schema, start.start_positions);
    const std::unique_ptr<TdfSection> sides = parse_file(vfs, "gamedata\\sidedata.tdf");
    for (std::size_t s = 0; s < 10; ++s) {
        sim::StartPlayer& p = start.players[s];
        if (p.kind == sim::PlayerKind::Empty || p.commander_type) continue;
        const std::string com = side_commander(sides.get(), req.sides[s]);
        p.commander_type = lb->type_index(com);
        b.players[s].commander_name = com;
    }
    if (!sim::start_battle(b, start, error)) return nullptr;
    return lb;
}

}  // namespace ta
