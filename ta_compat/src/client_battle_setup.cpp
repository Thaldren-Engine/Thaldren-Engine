// Battle set-up for the playable client: the battle comes from ta::load_battle (the
// original's battle loading and start, ta/battle_loader.h); this file adds the client's
// drawing data (unit models) and connects the battle's presentation events to the client.

#include <algorithm>
#include <cstdio>

#include "client/front_end.h"
#include "sim/battle_quiet.h"
#include "sim/missions.h"
#include "ta/client_world.h"
#include "ta/model3do.h"
#include "ta/net_setup.h"
#include "ta/tdf.h"

namespace ta {

struct ClientWorld::Outside : sim::QuietBattleOutside {
    ClientWorld* w = nullptr;
    // Behaviours the battle does not run itself wait 30 steps at a time instead of looping.
    std::int32_t run_mission(sim::Unit&, sim::MissionOrder& order, std::uint32_t&) override {
        order.wait |= sim::order_wait::kTimer;
        order.wake_tick = w->battle().game_tick() + 30;
        return sim::mission_result::kContinue;
    }
    // Presentation.
    void play_sound(std::uint16_t s, const sim::Vec3i& p) override {
        if (w->client_) w->client_->on_sound(s, p);
    }
    void speech(sim::Unit& u, std::uint32_t id, sim::SpeechText) override {
        if (w->client_) w->client_->on_speech(u, id);
    }
    void forget_speech(sim::Unit& u) override {
        if (w->client_) w->client_->forget_speech(u);
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
    // A mission goal met the first time: "Victory Condition" (0x47F1A0 from the goal objects).
    void mission_goal_met() override {
        if (w->client_) w->client_->interface_sound(w->content().ui_sounds.victory);
    }
    // The minimap's work inside the step (players' stage).
    void player_stage_rest(int p) override {
        if (w->client_) w->client_->minimap_player_stage(p);
    }
    void player_sensor_turn(int p) override {
        if (w->client_) w->client_->minimap_sensor_turn(p);
    }
    void players_stage_end() override {
        if (w->client_) w->client_->minimap_step_end();
    }
    // Combat involving the local player (the music's battle/peace switch).
    void stats_activity(std::int32_t kind) override {
        if (w->on_activity) w->on_activity(kind);
    }
};

ClientWorld::ClientWorld() = default;
ClientWorld::~ClientWorld() = default;

std::array<int, 2> ClientWorld::start_position(int player) const {
    if (lb_ && lb_->battle && player >= 0 && player < 10) {
        const sim::Battle& b = *lb_->battle;
        const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(player)];
        for (int id = p.first_unit; id > 0 && id <= p.last_unit && static_cast<std::size_t>(id) < b.units.size(); ++id)
            if (b.ticks[static_cast<std::size_t>(id)].type_index)
                return {b.units[static_cast<std::size_t>(id)].pos[0] >> 16, b.units[static_cast<std::size_t>(id)].pos[2] >> 16};
    }
    return {hm_.width() * 8, hm_.height() * 8};
}

BattleRequest to_battle_request(Vfs& vfs, const SkirmishSetup& setup) {
    BattleRequest req;
    req.map = setup.map;
    std::vector<std::uint8_t> sd;
    vfs.read("gamedata\\sidedata.tdf", sd);
    const TdfParseResult sr = tdf_parse(std::string_view(reinterpret_cast<const char*>(sd.data()), sd.size()));
    for (std::size_t p = 0; p < setup.players.size() && p < 10; ++p) {
        const SkirmishPlayer& sp = setup.players[p];
        const TdfSection* s = sr.root ? sr.root->child("SIDE" + std::to_string(sp.side)) : nullptr;
        req.sides[p] = s ? s->get_string("name", 32, "") : std::string(sp.side ? "CORE" : "ARM");
        sim::StartPlayer& st = req.start.players[p];
        st.kind = sp.ai ? sim::PlayerKind::Computer : sim::PlayerKind::Local;
        st.net_id = static_cast<std::int32_t>(p) + 1;
        st.colour = static_cast<std::uint8_t>(p);
        st.start_position = sp.start_pos;
        st.energy = static_cast<std::int32_t>(setup.start_energy);
        st.metal = static_cast<std::int32_t>(setup.start_metal);
    }
    req.start.fixed_locations = true;
    req.start.start_energy = setup.start_energy;
    req.start.start_metal = setup.start_metal;
    req.start.fog_of_war = setup.line_of_sight;
    req.start.explore = setup.mapping;
    req.start.true_sight = setup.true_los;
    req.start.local_player = static_cast<std::uint8_t>(setup.human);
    req.start.battle_rng_seed = setup.seed;
    req.start.loading_rand_seed = setup.seed;
    req.start.crt_rand_state = setup.seed;
    return req;
}

bool ClientWorld::setup_battle(std::string& error, const std::function<void(float, const std::string&)>& progress) {
    // The loading screen's rows in the original's order (Textures, Terrain, Units, Animation,
    // 3D Data, Explosions): each row is reported when the client's own work for it is done, the
    // units row model by model with the original's values (k * 100 / count for definition k of
    // count, the blank one included), so the screen always shows a state the original's could.
    auto row = [&](int r, int pct) {
        if (progress) progress(static_cast<float>(pct) / 100.0f, client::load_row_stage(r));
    };
    textures_.load(vfs_);
    row(0, 100);
    outside_ = std::make_unique<Outside>();
    outside_->w = this;
    sim::BattleOutside& out = net_wrap ? net_wrap(*outside_) : static_cast<sim::BattleOutside&>(*outside_);
    lb_ = load_battle(vfs_, request_, out, error, net_attach ? std::function<void(sim::Battle&)>(net_attach) : std::function<void(sim::Battle&)>());
    if (!lb_) return false;
    if (net_attach) {
        // Network games: the weapon table by index, and the network side again now that the
        // player records exist.
        set_network_weapons(*lb_->battle, lb_->weapons);
        net_attach(*lb_->battle);
    }
    // Copies the client draws from (static data).
    defs_ = lb_->unit_defs;
    weapons_ = lb_->weapon_defs;
    feature_models_ = lb_->feature_models;
    tnt_ = lb_->tnt;
    if (!tnt_load_tiles(tnt_, tiles_) || !tnt_load_heights(tnt_, hm_, &tnt_info_)) {
        error = "bad map file";
        return false;
    }
    row(1, 100);

    // Every unit type's model, at its definition index (the battle type index).
    sim::Battle& b = *lb_->battle;
    types_.clear();
    types_.push_back(nullptr);  // index 0: the blank type
    int loaded = 0;
    const std::size_t ndefs = lb_->type_assets.size();
    for (std::size_t k = 1; k < ndefs; ++k) {
        row(2, static_cast<int>(k * 100 / ndefs));
        auto t = std::make_unique<ClientUnitType>();
        if (lb_->type_assets[k]) {
            t->assets = lb_->type_assets[k].get();
            auto m = load_3do(t->assets->model_file, true);
            if (m.root) {
                t->root = std::move(m.root);
                t->model = build_render_model(t->root.get(), textures_);
                bind_pose_primitives(t->assets->pose_model, t->model);
                t->ok = true;
                ++loaded;
            }
        }
        types_.push_back(std::move(t));
    }
    row(2, 100);
    // (The features, movement classes and explosion tables were made inside load_battle.)
    row(3, 100);
    row(4, 100);
    row(5, 100);
    for (std::size_t k = 1; k < types_.size(); ++k)
        if (types_[k] && types_[k]->ok) model_of_pose_[&types_[k]->assets->pose_model] = &types_[k]->model;
    std::printf("map %s: %dx%d cells; unit types: %d of %zu loaded; players:", request_.map.c_str(), hm_.width(), hm_.height(), loaded,
                types_.size() - 1);
    for (int p = 0; p < 10; ++p)
        if (b.players[static_cast<std::size_t>(p)].active) std::printf(" %d(%s, controller %d)", p, request_.sides[static_cast<std::size_t>(p)].c_str(),
                                                                   b.players[static_cast<std::size_t>(p)].controller);
    std::printf("\n");
    return true;
}

bool ClientWorld::load(const std::string& game_dir, const SkirmishSetup& setup, std::string& error) {
    Vfs v;
    v.mount_game_dir(game_dir);
    v.scan();  // every battle start follows a rescan in the original (see ClientWorld::load below)
    BattleRequest req = to_battle_request(v, setup);
    setup_ = setup;
    return load(game_dir, req, setup.human, error);
}

bool ClientWorld::load(const std::string& game_dir, const BattleRequest& request, int human, std::string& error,
                       const std::function<void(float, const std::string&)>& progress) {
    request_ = request;
    human_ = human;
    setup_.map = request.map;
    setup_.human = human;
    // The start-up scan, then the rescan the original runs on every path into a battle
    // (skirmish Start, MULTI, New Campaign / briefing Start, load, restart): on the Steam
    // install that adds totala3.hpi, totala4.hpi and worlds.hpi.
    vfs_.mount_game_dir(game_dir);
    vfs_.scan();
    if (!load_palette(vfs_, palette_) || !load_blend_table(vfs_, blend_)) {
        error = "cannot load the palette / blend table";
        return false;
    }
    // The map line: the map's name, none for missions.
    if (progress) progress(0.0f, "map:" + (request.start.provider == 1 ? std::string() : request.map));
    if (!setup_battle(error, progress)) return false;
    if (progress) progress(1.0f, "Loading interface");
    if (!build_content(error)) return false;
    return true;
}

} // namespace ta
