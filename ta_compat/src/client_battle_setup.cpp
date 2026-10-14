// Skirmish set-up for the playable client. ADAPTER: until the game-flow agent's
// battle set-up lands, this follows the battle view tool's set-up (map, features,
// economy, sight, routes, every unit type) and places each player's commander at
// its start position. The battle's outside forwards presentation events to the
// client; AI players get the AI agent's controller here once it exists.

#include <algorithm>
#include <cstdio>

#include "sim/battle_quiet.h"
#include "sim/economy.h"
#include "sim/missions.h"
#include "ta/battle_economy.h"
#include "ta/client_world.h"
#include "ta/cob.h"
#include "ta/model3do.h"
#include "ta/sight_tables.h"

// AI adapter: with the AI agent's computer player present, AI players get their type data,
// the map's metal sites and the profile (ai\*.txt); otherwise they stand still.
#if __has_include("ta/ai_profile.h")
#define CLIENT_HAVE_AI 1
#include "ta/ai_profile.h"
#else
#define CLIENT_HAVE_AI 0
#endif

namespace ta {

struct ClientWorld::Outside : sim::QuietBattleOutside {
    ClientWorld* w = nullptr;
    // Behaviours not built yet wait 30 steps at a time instead of looping.
    std::int32_t run_mission(sim::Unit&, sim::MissionOrder& order, std::uint32_t&) override {
        order.wait |= sim::order_wait::kTimer;
        order.wake_tick = w->battle_->game_tick() + 30;
        return sim::mission_result::kContinue;
    }
    // AI adapter: the AI agent's per-step controller for computer players goes here.
    void ai_controller_step(int) override {}
    // Presentation.
    void play_sound(std::uint16_t s, const sim::Vec3i& p) override {
        if (w->client_) w->client_->on_sound(s, p);
    }
    void speech(sim::Unit& u, std::uint32_t id, sim::SpeechText) override {
        if (w->client_) w->client_->on_speech(u, id);
    }
    void under_attack_notice(sim::Unit& u) override {
        if (w->client_) w->client_->on_under_attack(u);
    }
    void selection_changed() override {
        if (w->client_) w->client_->selection_changed();
    }
    void orders_panel_changed() override {
        if (w->client_) w->client_->selection_changed();
    }
    void feature_fire_sound(std::int16_t x, std::int16_t z) override {
        if (w->client_) {
            const int s = w->sample("treeburn");
            if (s >= 0) w->client_->on_sound(static_cast<std::uint16_t>(s), {x << 20, 0, z << 20});
        }
    }
};

ClientWorld::ClientWorld() = default;
ClientWorld::~ClientWorld() = default;

std::array<int, 2> ClientWorld::start_position(int player) const {
    if (player < 0 || static_cast<std::size_t>(player) >= setup_.players.size()) return {0, 0};
    int k = setup_.players[static_cast<std::size_t>(player)].start_pos;
    if (k < 0) k = player;
    if (static_cast<std::size_t>(k) < start_positions_.size()) return start_positions_[static_cast<std::size_t>(k)];
    return {hm_.width() * 8, hm_.height() * 8};
}

bool ClientWorld::setup_battle(std::string& error) {
    // The map.
    if (!vfs_.read("maps\\" + setup_.map + ".tnt", tnt_)) {
        error = "map not found: " + setup_.map;
        return false;
    }
    if (!tnt_load_tiles(tnt_, tiles_) || !tnt_load_heights(tnt_, hm_, &tnt_info_)) {
        error = "bad map file";
        return false;
    }
    std::vector<std::uint8_t> ota;
    vfs_.read("maps\\" + setup_.map + ".ota", ota);
    if (!ota.empty()) {
        // Start positions: [GlobalHeader] > [Schema 0] > [specials] > specialwhat=StartPosN, XPos, ZPos.
        const TdfParseResult r = tdf_parse(std::string_view(reinterpret_cast<const char*>(ota.data()), ota.size()));
        const TdfSection* gh = r.root ? r.root->child("GlobalHeader") : nullptr;
        const TdfSection* sch = gh ? gh->child("Schema 0") : nullptr;
        const TdfSection* sp = sch ? sch->child("specials") : nullptr;
        if (sp)
            for (const auto& c : sp->children()) {
                const std::string what = c->get_string("specialwhat", 64, "");
                if (what.size() > 8 && stricmp_c(what.substr(0, 8), "StartPos") == 0) {
                    const int n = std::atoi(what.c_str() + 8);
                    if (n >= 1 && n <= 10) {
                        if (start_positions_.size() < static_cast<std::size_t>(n)) start_positions_.resize(static_cast<std::size_t>(n), {0, 0});
                        start_positions_[static_cast<std::size_t>(n - 1)] = {c->get_int("XPos", 0), c->get_int("ZPos", 0)};
                    }
                }
            }
    }

    // Definitions.
    weapons_ = load_weapon_defs(vfs_, WeaponDefOptions{});
    std::array<std::string, 256> weapon_names;
    for (std::size_t i = 0; i < 256; ++i) weapon_names[i] = weapons_.slots[i].name;
    fdefs_.load_library(vfs_, weapon_names);
    std::vector<std::string> tnt_names;
    tnt_feature_names(tnt_, tnt_names);
    fdefs_.load_map_features(tnt_names);
    load_unit_context(vfs_, ctx_);
    defs_ = load_unit_defs(vfs_, ctx_, &fdefs_);
    fdefs_.resolve_chains();
    textures_.load(vfs_);

    // The battle.
    weapon_types_ = to_weapon_types(weapons_);
    rng_.seed(setup_.seed);
    outside_ = std::make_unique<Outside>();
    outside_->w = this;
    battle_ = std::make_unique<sim::Battle>(*outside_, hm_, rng_);
    sim::Battle& b = *battle_;
    anim_import_ = std::make_unique<AnimImporter>(vfs_, b.anims);
    bind_weapon_explosions(weapons_, weapon_types_, *anim_import_);
    b.features = to_feature_types(fdefs_, weapon_types_, *anim_import_, &feature_models_);
    load_effect_art(*anim_import_, b.effect_art);
    b.settings.sea_level = static_cast<std::uint8_t>(tnt_info_.sea_level);
    b.settings.gravity = 112;
    b.settings.damage_options = 8;  // weapons damage features
    const int np = static_cast<int>(setup_.players.size());
    const int per = 250;
    b.init_unit_table(static_cast<std::size_t>(1 + per * np));
    for (int p = 0; p < np; ++p) {
        sim::PlayerSlot& s = b.players[static_cast<std::size_t>(p)];
        s.active = true;
        s.controller = static_cast<std::uint8_t>(setup_.players[static_cast<std::size_t>(p)].ai ? 2 : 1);
        s.net_id = p + 1;
        s.state = static_cast<std::uint8_t>(p);
        b.alliances[static_cast<std::size_t>(p)][static_cast<std::size_t>(p)] = 1;  // allied with itself only
        b.assign_unit_range(p, 1 + per * p, per + per * p);
        b.init_player_economy(p);
        sim::set_storage_bonus(s, b.player_economy[static_cast<std::size_t>(p)], static_cast<std::int32_t>(setup_.start_energy),
                               static_cast<std::int32_t>(setup_.start_metal));
        s.energy_stored = setup_.start_energy;
        s.metal_stored = setup_.start_metal;
    }
    b.settings.ai_difficulty = 2;
    b.map.width = hm_.width();
    b.map.height = hm_.height();
    b.map.cells.assign(static_cast<std::size_t>(b.map.width) * static_cast<std::size_t>(b.map.height), sim::MapCell{});
    for (std::int32_t z = 0; z < b.map.height; ++z)
        for (std::int32_t x = 0; x < b.map.width; ++x) b.map.cells[static_cast<std::size_t>(z * b.map.width + x)].height = hm_.at(x, z);
    sim::refresh_height_range(b.map, 0, 0, static_cast<std::int16_t>(b.map.width), static_cast<std::int16_t>(b.map.height));
    b.init_buckets();
    b.reset_features_and_effects();
    place_tnt_features(b, tnt_);
    {
        MapEconomy me;
        if (!ota.empty()) read_map_economy(ota, 0, me);
#if CLIENT_HAVE_AI
        b.settings.surface_metal = me.surface_metal < 0 ? 0 : me.surface_metal;
#endif
        init_map_economy(b, tnt_, me, fdefs_);
        b.step_wind();
    }
    b.settings.player_count = static_cast<std::uint16_t>(np);
    b.settings.units_per_player = static_cast<std::uint16_t>(per);
    b.settings.local_player = static_cast<std::uint8_t>(setup_.human);
    b.move_classes = to_move_classes(ctx_);
    b.settings.fog_of_war = setup_.line_of_sight;
    b.settings.explore = setup_.mapping;
    b.settings.true_sight = setup_.true_los;
    b.settings.local_view = static_cast<std::uint8_t>(setup_.human);
    {
        std::string err;
        if (!load_sight_tables(vfs_, b.sight_tables, err)) std::printf("sight tables: %s\n", err.c_str());
        b.init_sight();
    }
    b.init_routes();

    // Every unit type, at its definition index (the original's type index).
    types_.clear();
    types_.push_back(nullptr);  // index 0: the blank type
    int loaded = 0;
    for (std::size_t k = 1; k < defs_.units.size(); ++k) {
        const UnitDefRecord& d = defs_.units[k];
        auto t = std::make_unique<ClientUnitType>();
        std::string err;
        b.types.emplace_back();
        if (load_unit_assets(vfs_, d, t->assets, err)) {
            auto m = load_3do(t->assets.model_file, true);
            if (m.root) {
                t->root = std::move(m.root);
                t->model = build_render_model(t->root.get(), textures_);
                bind_pose_primitives(t->assets.pose_model, t->model);
                to_battle_unit_type(d, t->assets, weapon_types_, b.types.back());
                to_targeting_type(d, defs_.categories, weapon_types_, b.types.back().targeting);
#if CLIENT_HAVE_AI
                to_ai_type(d, weapon_types_, b.types.back().ai);
#endif
                t->ok = true;
                ++loaded;
            }
        }
        types_.push_back(std::move(t));
    }
    for (std::size_t k = 1; k < types_.size(); ++k)
        if (types_[k] && types_[k]->ok) model_of_pose_[&types_[k]->assets.pose_model] = &types_[k]->model;
    std::printf("map %s: %dx%d cells, %zu start positions; unit types: %d of %zu loaded\n", setup_.map.c_str(), hm_.width(),
                hm_.height(), start_positions_.size(), loaded, defs_.units.size() - 1);

    // Commanders at the start positions.
    std::vector<std::uint8_t> sd;
    vfs_.read("gamedata\\sidedata.tdf", sd);
    const TdfParseResult sr = tdf_parse(std::string_view(reinterpret_cast<const char*>(sd.data()), sd.size()));
    for (int p = 0; p < np; ++p) {
        const int side = setup_.players[static_cast<std::size_t>(p)].side;
        const TdfSection* s = sr.root ? sr.root->child("SIDE" + std::to_string(side)) : nullptr;
        const std::string com = s ? s->get_string("commander", 32, "") : std::string();
        b.players[static_cast<std::size_t>(p)].commander_name = com;
        std::uint16_t type = 0;
        for (std::size_t k = 1; k < defs_.units.size(); ++k)
            if (stricmp_c(defs_.units[k].unit_name, com) == 0 && types_[k] && types_[k]->ok) type = static_cast<std::uint16_t>(k);
        if (!type) {
            error = "no commander type for side " + std::to_string(side);
            return false;
        }
        const auto at = start_position(p);
        const std::int32_t gy = std::max(hm_.sample(static_cast<std::int16_t>(at[0]), static_cast<std::int16_t>(at[1])), 0);
        if (!b.create_unit(p, type, at[0] << 16, gy << 16, at[1] << 16, true, 1)) {
            error = "could not create the commander of player " + std::to_string(p);
            return false;
        }
    }
    b.start_players();
#if CLIENT_HAVE_AI
    {
        AiProfileReport report;
        if (!load_ai_profile(vfs_, ai_profile_path(ota, 0), defs_, b.ai(), report)) std::printf("no computer player profile\n");
        b.collect_ai_metal_sites();
    }
#endif
    b.rebuild_sight(true);
    return true;
}

bool ClientWorld::load(const std::string& game_dir, const SkirmishSetup& setup, std::string& error) {
    setup_ = setup;
    vfs_.mount_game_dir(game_dir);
    if (!load_palette(vfs_, palette_) || !load_blend_table(vfs_, blend_)) {
        error = "cannot load the palette / blend table";
        return false;
    }
    if (!setup_battle(error)) return false;
    if (!build_content(error)) return false;
    return true;
}

} // namespace ta
