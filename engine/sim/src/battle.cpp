#include "sim/battle.h"

namespace sim {

Battle::Battle(BattleOutside& outside, const HeightMap& heights, BattleRng& rng)
    : players(kPlayerRecords),
      types(1),
      outside_(outside),
      heights_(heights),
      rng_(rng),
      combat_{units, ticks, players, combat_env_, *this},
      weapons_{units, ticks, players, weapon_env_, *this},
      projectile_world_{projectiles, combat_, heights_, map, features, projectile_env_, *this, rng_},
      orders_{ticks, players, rng_, *this, 0},
      footprint_{map, buckets, units, ticks, players, *this, {}},
      feature_world_{feature_state, map, features, heights_, anims, feature_env_, *this, rng_, crt_rand},
      effects_world_{effects, map, anims, effect_art, *this, rng_},
      motion_{units, ticks, players, motion_types_, heights_, motion_hooks_, {}, {}},
      missions_{orders_, units, combat_, *this, mission_hooks_, mission_types_, alliances, mission_env_},
      targeting_{units, ticks, players, alliances, targeting_types_, mission_types_, map, 0, missions_, targeting_env_,
                 targeting_hooks_, targeting_state_},
      route_{map, units, ticks, players, route_types_, move_classes, feature_blocks, explored, route_search, motion_, route_hooks_, {}, {}},
      economy_{units, ticks, players, player_economy, economy_types_, economy_env_, economy_hooks_},
      sight_world_{sight, sight_tables, explored, units, ticks, players, alliances, sight_types_, buckets, &targeting_state_, sight_env_, 0, 0},
      construction_{map, features, construction_types_, heights_, construction_hooks_, construction_env_},
      ai_{units, ticks, players, player_economy, alliances, ai_types_, targeting_state_, map, features, rng_, ai_env_, ai_hooks_, ai_state, {}},
      unit_missions_{targeting_types_, motion_types_, unit_mission_types_, map, features, heights_, explored, unit_mission_hooks_,
                     unit_mission_env_} {
    missions_.construction = &construction_;
    missions_.unit_missions = &unit_missions_;
    unit_missions_.economy = &player_economy;
    unit_missions_.tick_types = &tick_types_;
    alliances.resize(kPlayerRecords);
    player_economy.resize(kPlayerRecords);
    feature_state.reset();
    effects.reset();
    types[0].info.max_damage = 0;  // the blank type of empty slots
    types[0].tick.damage_modifier = 0;
}

Battle::~Battle() {
    for (MissionOrder* o : free_orders_) delete o;
    for (Unit& u : units)
        for (MissionOrder* list : {u.orders, u.secondary_orders})
            while (list) {
                MissionOrder* next = list->next;
                delete list;
                list = next;
            }
}

void Battle::sync_settings() {
    // Units point at their type data by index; re-point them (the type table may have grown).
    for (std::size_t i = 0; i < units.size() && i < ticks.size(); ++i) {
        const std::uint16_t k = ticks[i].type_index;
        if (k == 0 || k >= types.size()) continue;
        units[i].type = &types[k].info;
        ticks[i].tick_type = &types[k].tick;
    }
    const BattleSettings& s = settings;
    unit_env_.sea_level = s.sea_level;
    unit_env_.water_damage_enabled = s.water_damage_enabled;
    unit_env_.water_damage = s.water_damage;
    unit_env_.broadcast_unit_states = s.net_game;
    unit_env_.addresses = s.addresses;
    combat_env_.local_player = s.local_player;
    combat_env_.local_view = s.local_view;
    combat_env_.game_mode = s.game_mode;
    combat_env_.ai_difficulty = s.ai_difficulty;
    combat_env_.fog_of_war = s.fog_of_war;
    combat_env_.empty_type = &types[0].info;
    combat_env_.empty_tick_type = &types[0].tick;
    weapon_env_.networked = s.net_game;
    weapon_env_.sea_level = s.sea_level;
    weapon_env_.gravity = s.gravity;
    weapon_env_.heightmap = &heights_;
    weapon_env_.addresses = s.addresses;
    projectile_env_.game_tick = unit_env_.game_tick;
    projectile_env_.wind = s.wind_drift;
    projectile_env_.gravity = s.gravity;
    projectile_env_.sea_level = s.sea_level;
    projectile_env_.no_sea_level_trigger = s.no_sea_level_trigger;
    projectile_env_.options = s.damage_options;
    projectile_env_.net_game = s.net_game;
    orders_.game_tick = unit_env_.game_tick;
    feature_env_.game_tick = unit_env_.game_tick;
    feature_env_.sea_level = s.sea_level;
    feature_env_.gravity = s.gravity;
    feature_env_.wind = s.wind_drift;
    feature_env_.destructible = (s.damage_options & 8) != 0;
    feature_env_.network_role = s.feature_network_role;
    effect_art.gravity = s.gravity;
    effect_art.sea_level = s.sea_level;
    effect_art.no_sea_level_trigger = s.no_sea_level_trigger;
    effect_art.lava_world = s.lava_world;
    // Motion and mission type data by type index.
    motion_types_.resize(types.size());
    mission_types_.resize(types.size());
    for (std::size_t k = 0; k < types.size(); ++k) {
        motion_types_[k] = types[k].motion;
        mission_types_[k] = types[k].mission;
    }
    motion_.env.game_tick = unit_env_.game_tick;
    motion_.env.sea_level = s.sea_level;
    motion_.env.gravity = s.gravity;
    motion_.env.addresses = s.addresses;
    mission_env_.sea_level = s.sea_level;
    mission_env_.gravity = s.gravity;
    mission_env_.addresses = s.addresses;
    targeting_types_.resize(types.size());
    for (std::size_t k = 0; k < types.size(); ++k) targeting_types_[k] = types[k].targeting;
    targeting_env_.game_tick = unit_env_.game_tick;
    targeting_env_.sea_level = s.sea_level;
    targeting_env_.local_player = s.local_player;
    targeting_env_.unit_limit = s.unit_limit;
    targeting_env_.target_everything = s.target_everything;
    targeting_env_.command_mode = s.command_mode;
    targeting_.feature_count = static_cast<std::uint32_t>(features.size());
    route_types_.resize(types.size());
    for (std::size_t k = 0; k < types.size(); ++k) {
        route_types_[k] = types[k].route;
        route_types_[k].footprint_x = types[k].create.footprint_x;
        route_types_[k].footprint_z = types[k].create.footprint_z;
        route_types_[k].movement = types[k].create.movement;
    }
    construction_types_.resize(types.size());
    for (std::size_t k = 0; k < types.size(); ++k) construction_types_[k] = types[k].construction;
    construction_env_.ai_difficulty = s.ai_difficulty;
    construction_env_.sea_level = s.sea_level;
    construction_env_.build_menu_focus = s.build_menu_focus;
    unit_mission_types_.resize(types.size());
    tick_types_.resize(types.size());
    for (std::size_t k = 0; k < types.size(); ++k) {
        unit_mission_types_[k] = types[k].unit_missions;
        tick_types_[k] = types[k].tick;
    }
    unit_mission_env_.map_width = map.width * 16 - 32;
    unit_mission_env_.map_height = map.height * 16 - 128;
    unit_mission_env_.sea_level = s.sea_level;
    unit_mission_env_.gravity = s.map_gravity;  // the bomb run reads the map info's raw value
    unit_mission_env_.air_repair_patrol_site_address = s.air_repair_patrol_site_address;
    route_.env.game_tick = unit_env_.game_tick;
    route_.env.sea_level = s.sea_level;
    route_.env.player_count = s.player_count;
    route_.env.units_per_player = s.units_per_player;
    economy_types_.resize(types.size());
    for (std::size_t k = 0; k < types.size(); ++k) economy_types_[k] = types[k].economy;
    economy_env_.game_tick = unit_env_.game_tick;
    economy_env_.ai_difficulty = s.ai_difficulty;
    economy_env_.wind_fraction = wind.fraction;
    economy_env_.tidal_strength = s.tidal_strength;
    sync_sight();
}

void Battle::sync_sight() {
    const BattleSettings& s = settings;
    sight_types_.resize(types.size());
    for (std::size_t k = 0; k < types.size(); ++k) sight_types_[k] = types[k].sight;
    sight.flags = static_cast<std::uint16_t>((sight.flags & sight_flags::kViewClean) | (s.explore ? sight_flags::kReveal : 0) |
                                             (s.fog_of_war ? sight_flags::kFog : 0) | (s.true_sight ? sight_flags::kTrueSight : 0));
    sight_env_.game_tick = unit_env_.game_tick;
    sight_env_.sea_level = s.sea_level;
    sight_env_.view_player = s.local_view;
    sight_env_.player_count = s.player_count;
    sight_env_.options = s.player_sight;
}

void Battle::init_sight() {
    sync_settings();
    init_sight_map(sight, explored, heights_, settings.sea_level);
    init_player_sight(sight, players);
    sight_ready_ = true;
}

void Battle::rebuild_sight(bool clear_explored) {
    sync_settings();
    sim::rebuild_sight(sight_world_, clear_explored);
}

void Battle::expire_lingering_sight() {
    if (!sight_ready_) return;
    sync_settings();
    sim::expire_lingering_sight(sight_world_);
}

bool Battle::unit_seen_by(int viewer, const Unit& unit) {
    sync_settings();
    return sim::unit_visible(sight_world_, viewer, unit);
}

bool Battle::point_seen_by(int viewer, const std::array<std::int32_t, 3>& pos) {
    sync_settings();
    return point_in_sight(sight_world_, viewer, pos);
}

void Battle::update_sight(Unit& u) {
    if (!sight_ready_) {
        outside_.update_sight(u);
        return;
    }
    sync_sight();
    update_unit_sight(sight_world_, u);
}

void Battle::stamp_line_of_sight(Unit& u) {
    if (!sight_ready_) {
        outside_.stamp_line_of_sight(u);
        return;
    }
    sync_sight();
    stamp_new_unit(sight_world_, u);
}

void Battle::reveal_dead_unit(Unit& u) {
    // The kill handler, for a unit of the viewed player (0x482910): sight lingers 60 steps.
    if (!sight_ready_) {
        outside_.reveal_dead_unit(u);
        return;
    }
    sync_sight();
    linger_dead_unit(sight_world_, u);
}

void Battle::forget_fog(Unit& u) {
    // The kill handler with fog (0x482090): the unit's sight goes.
    if (!sight_ready_) {
        outside_.forget_fog(u);
        return;
    }
    sync_sight();
    remove_unit_sight(sight_world_, u);
}

bool Battle::TargetingHooks::unit_visible(int p, Unit& u) {
    if (!b.sight_ready_) return b.outside_.unit_visible(p, u);
    b.sync_sight();
    return sim::unit_visible(b.sight_world_, p, u);
}

bool Battle::TargetingHooks::position_explored(int p, const std::array<std::int32_t, 3>& pos) {
    if (!b.sight_ready_) return b.outside_.position_explored(p, pos);
    b.sync_sight();
    return sim::position_explored(b.sight_world_, p, pos);
}

void Battle::start_players() {
    sync_settings();
    create_player_targeting(targeting_);
    // The computer player's part of the player set-up (0x464700), for every record with a
    // controller type: groups, controller and roles, the table (eight random draws).
    ai_types_.clear();
    sync_ai();
    // Records the battle start already set up (setup_player) keep theirs.
    for (int slot = 0; slot < 10; ++slot)
        if (players[static_cast<std::size_t>(slot)].controller != 0 && !ai_state.controllers[static_cast<std::size_t>(slot)].exists)
            ai_create_player(ai_, slot);
}

void Battle::setup_player(int player) {
    init_player_economy(player);
    sync_ai();
    ai_create_player(ai_, player);  // skips a watched record itself
    outside_.ai_player_setup(player);
}

void Battle::battle_ready() {
    collect_ai_metal_sites();
    outside_.ai_battle_ready();
}

void Battle::sync_ai() {
    if (ai_types_.size() != types.size()) {
        ai_types_.resize(types.size());
        for (std::size_t k = 0; k < types.size(); ++k) ai_types_[k] = types[k].ai;
    }
    ai_env_.game_tick = unit_env_.game_tick;
    ai_env_.difficulty = settings.ai_difficulty;
    ai_env_.map_width = map.width;
    ai_env_.map_height = map.height;
    ai_env_.surface_metal = settings.surface_metal;
    ai_env_.provider = settings.provider;
    ai_env_.units_per_player = settings.units_per_player;
    ai_env_.wind_fraction = wind.fraction;
    ai_env_.wind_max_speed = wind.max_speed;
    ai_env_.wind_denominator = wind.denominator;
    ai_env_.tidal_strength = settings.tidal_strength;
    ai_env_.stale_site_height = settings.ai_site_height_word;
}

Ai& Battle::ai() {
    sync_settings();
    sync_ai();
    return ai_;
}

void Battle::collect_ai_metal_sites() { ai_collect_metal_sites(ai()); }

void Battle::AiHooks::build_menu(Unit& u, std::uint16_t type, bool weapon_menu) {
    // The build menu command (0x419B00, +1): the stockpile button's mission, else the
    // factory's (or mobile builder's) build queue of the type.
    if (weapon_menu) {
        b.sync_settings();
        adjust_build_count(b.missions_, u, mission::kBuildWeapon, 0, 1);
    } else {
        b.order_build_count(u, type, 1);
    }
}

bool Battle::AiHooks::point_in_sight(int player, const std::array<std::int32_t, 3>& pos) {
    if (!b.sight_ready_) return b.outside_.position_explored(player, pos);
    b.sync_sight();
    return sim::point_in_sight(b.sight_world_, player, pos);
}

void Battle::prune_retired_goals() {
    // Kept while the route search or a unit's motion goal still points at one.
    auto& r = motion_.retired_goals;
    if (r.empty()) return;
    for (std::size_t i = 0; i < r.size();) {
        bool used = r[i].get() == route_search.order_goal;
        for (std::size_t k = 1; k < ticks.size() && !used; ++k)
            used = ticks[k].motion.goal && ticks[k].motion.goal->attached == r[i].get();
        if (!used) r.erase(r.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

void Battle::players_stage() {
    sync_settings();
    sim::run_route_search(route_);  // the route search runs first
    prune_retired_goals();
    for (int slot = 0; slot < 10; ++slot) {
        const PlayerSlot& p = players[static_cast<std::size_t>(slot)];
        if (!p.active || p.controller < 1 || p.controller > 3 || p.state == 10) continue;
        if (targeting_state_.controllers[static_cast<std::size_t>(slot)].exists) {
            // The player's controller: an AI player's own work, then the weapon-target pass.
            const bool ai = p.controller == 2;
            if (ai) sim::ai_controller_step(this->ai(), slot);
            weapon_target_pass(targeting_, slot, ai);
        }
        refresh_unit_table(targeting_, slot);
        // The player's sight updates. With the sight system they share one sync (before the
        // first): a sight update changes nothing the sync copies (types, settings, the step).
        bool synced = false;
        for (int id = p.first_unit; id <= p.last_unit && id < static_cast<int>(units.size()); ++id)
            if (id > 0 && (units[static_cast<std::size_t>(id)].flags & unit_flags::kAlive)) {
                if (!sight_ready_) {
                    update_sight(units[static_cast<std::size_t>(id)]);
                    continue;
                }
                if (!synced) sync_sight();
                synced = true;
                update_unit_sight(sight_world_, units[static_cast<std::size_t>(id)]);
            }
        outside_.player_stage_rest(slot);
        // The player's 30-step turn: victory checks, the economy settlement, sensors.
        PlayerEconomy& account = player_economy[static_cast<std::size_t>(slot)];
        if (account.next_update_tick > unit_env_.game_tick) continue;
        account.next_update_tick += 30;
        if (slot == settings.local_player) victory_turn(slot);
        outside_.player_victory_turn(slot);
        if (settlement_allowed(players[static_cast<std::size_t>(slot)], end_game)) {
            sync_settings();
            settle_player(economy_, slot);
        }
        // The viewed player's sensor pass (0x467440) belongs to this turn, after the settlement.
        if (sight_ready_ && slot == settings.local_view) {
            sync_sight();
            update_sensors(sight_world_);
        }
        outside_.player_sensor_turn(slot);
    }
    victory_stage_end();
    outside_.players_stage_end();
}

void Battle::init_player_economy(int player) {
    sync_settings();
    sim::init_player_economy(economy_, player);
}

void Battle::step_wind() {
    sim::step_wind(wind, unit_env_.game_tick, rng_, crt_rand);
    // What the other systems read: the drift of shots and fires, and the generators' scripts.
    settings.wind_drift = wind.velocity;
    unit_env_.wind_changed = wind.changed;
    unit_env_.wind_heading = wind.heading;
    unit_env_.wind_speed = wind.speed;
}

void Battle::auto_share_resources() {
    sync_settings();
    const int local = settings.local_player;
    auto_share(
        economy_, local, alliances,
        [&](int kind, int to, float amount) { outside_.resources_shared(kind, players[static_cast<std::size_t>(local)].state, to, amount); },
        [&](int to) { outside_.explored_map_shared(local, to); });
}

std::int32_t Battle::TargetingHooks::intercept_target(Unit& u, int slot, std::array<std::int32_t, 3>& pos) {
    const std::int32_t i = find_intercept_target(b.projectile_world_, u, slot);
    if (i >= 0) pos = b.projectiles.slots[static_cast<std::size_t>(i)].pos;
    return i;
}

std::string Battle::first_fault() {
    std::string out;
    auto check = [&out](const char* name, const std::string& f) {
        if (out.empty() && !f.empty()) out = std::string(name) + ": " + f;
    };
    check("route search", route_.fault);
    check("missions", missions_.fault);
    check("motion", motion_.fault);
    check("computer player", ai().fault);
    check("footprints", footprint_.fault);
    check("features", feature_state.fault);
    check("effects", effects.fault);
    check("projectiles", projectiles.fault);
    check("targeting", targeting_.fault);
    check("nano", nano.fault);
    check("effect lists", fx_lists.fault);
    return out;
}

void Battle::init_routes() {
    sync_settings();
    const std::size_t blocks = static_cast<std::size_t>(map.width >> 1) * static_cast<std::size_t>(map.height >> 1);
    if (explored.size() != blocks) explored.assign(blocks, 0xFFFF);
    build_passability(route_);
    init_route_search(route_);
}

void Battle::run_route_search() {
    sync_settings();
    sim::run_route_search(route_);
    prune_retired_goals();
}

void Battle::refresh_passability(std::int16_t x, std::int16_t z, std::int16_t w, std::int16_t h) {
    sync_settings();
    sim::update_passability(route_, x, z, w, h);
}

void Battle::step_units_and_projectiles() {
    sync_settings();
    run_unit_pass(units, ticks, players, unit_env_, *this, camera_countdown_);
    update_projectiles(projectile_world_);
}

void Battle::step_effects() {
    sync_settings();
    sim::step_effects(effects_world_);
}

void Battle::step_features() {
    sync_settings();
    sim::step_features(feature_world_);
}

void Battle::step_nano() {
    sync_settings();
    step_nano_streams(nano, crt_rand, unit_env_.game_tick);
}

void Battle::order_build_count(Unit& unit, std::uint16_t type, std::int32_t delta) {
    sync_settings();
    const std::uint8_t kind = ticks[unit.id].has_motion ? mission::kMobileBuild : mission::kBuildingBuild;
    adjust_build_count(missions_, unit, kind, type, delta);
}

bool Battle::order_mobile_build(Unit& builder, std::uint16_t type, const std::array<std::int32_t, 3>& pos, bool queued) {
    return order(builder, mission::kMobileBuild, queued, nullptr, &pos, type, 0);
}

bool Battle::order(Unit& unit, std::uint8_t kind, bool queued, Unit* target, const std::array<std::int32_t, 3>* pos,
                   std::int32_t param0, std::int32_t param1) {
    sync_settings();
    return enqueue_order(orders_, unit, kind, queued, target, pos ? pos->data() : nullptr, param0, param1);
}

void Battle::transport_attach(Unit& cargo, Unit* transport, std::uint8_t piece, std::uint8_t layer) {
    // The sender checks the same conditions as the receiver, then sends to the first
    // playing player's machine and applies the packet itself (0x48AAC0).
    sync_settings();
    if (!(cargo.flags & unit_flags::kAlive) || (cargo.flags & 0x20000000u) || cargo.cargo_head) return;
    if (transport && (!(transport->flags & unit_flags::kAlive) || transport == &cargo || transport->carrier)) return;
    std::uint8_t pk[7] = {0x0A, static_cast<std::uint8_t>(cargo.id), static_cast<std::uint8_t>(cargo.id >> 8),
                          static_cast<std::uint8_t>(transport ? transport->id : 0), static_cast<std::uint8_t>(transport ? transport->id >> 8 : 0),
                          piece, layer};
    std::int32_t net = -1;
    for (int s = 0; s < 10; ++s)
        if (players[static_cast<std::size_t>(s)].active && (players[static_cast<std::size_t>(s)].controller == 1 || players[static_cast<std::size_t>(s)].controller == 2)) {
            net = players[static_cast<std::size_t>(s)].net_id;
            break;
        }
    outside_.network_send(net, pk, 7);
    apply_transport_attach(missions_, construction_hooks_, cargo, transport, piece, layer);
}

void Battle::reset_features_and_effects() {
    sync_settings();
    feature_state.reset();
    effects.reset();
    fx_lists.reset();
    init_feature_type_cursors(feature_world_);
}

void Battle::spawn_wreck(Unit& u, std::int32_t level, bool burn) {
    // The kill packet's handler (0x486360): the unit is still in its cells.
    WreckSource src;
    if (u.type) {
        src.corpse = u.type->corpse;
        src.feature_unit = u.type->is_feature;
    }
    src.owner = static_cast<std::uint8_t>(u.id < ticks.size() ? ticks[u.id].owner : 10);
    sim::spawn_wreck(feature_world_, u, src, level, burn);
}

Unit* Battle::unit_by_id(std::uint16_t id) { return id < units.size() ? &units[id] : nullptr; }

void Battle::claim_yard_cells(Unit& u) {
    // Yard opening (0x47DAC0) after the cells were found free and the yard flag changed.
    claim_wanted_cells(footprint_, u);
    sim::update_passability(route_, u.cell_x, u.cell_z, u.footprint_x, u.footprint_z);
}

void Battle::weapons(Unit& unit, std::uint32_t& stale_word) { process_weapon_slots(weapons_, unit, stale_word); }

void Battle::damage(Unit* attacker, Unit& target, std::int32_t amount, std::int32_t kind, std::int32_t heading) {
    apply_damage(combat_, attacker, target, amount, kind, heading);
}

void Battle::repair_heal(Unit& worker, Unit& target, float rate) { sim::repair_heal(combat_, worker, target, rate); }

void Battle::motion(Unit& unit, std::uint32_t& stale_word) {
    run_motion(motion_, unit);
    stale_word = motion_stale_word(unit_env_);  // a saved register of the unit pass
}

void Battle::motion_cells_claimed(Unit& unit) {
    // The claim stamps the motion object's claim step.
    if (ticks[unit.id].has_motion) ticks[unit.id].motion.claim_tick = unit_env_.game_tick;
}

void Battle::primary_orders(Unit& unit, std::uint32_t& stale_word) {
    missions_.caller = MissionCaller::Primary;
    run_primary_orders(orders_, unit);
    // The original's routine leaves a saved register there: the visited slot's player record.
    stale_word = unit_env_.addresses.player(unit_env_.pass_slot);
}

void Battle::secondary_orders(Unit& unit, std::uint32_t& stale_word) {
    missions_.caller = MissionCaller::Secondary;
    run_secondary_orders(orders_, unit, stale_word);
}

void Battle::settle_height(Unit& unit, std::uint32_t& stale_word) {
    const UnitTickState& t = ticks[unit.id];
    settle_on_ground(unit, t.has_motion, types[t.type_index].settle, unit_env_.sea_level, unit_env_.game_tick, heights_, *this,
                     stale_word);
}

void Battle::killed(Unit& unit, std::uint8_t kind, std::uint32_t& stale_word) { kill_unit(combat_, unit, kind, stale_word); }

void Battle::add_paralysis(Unit& victim, std::uint16_t amount) { sim::add_paralysis(orders_, victim, amount); }

void Battle::clear_orders(Unit& unit) { sim::clear_orders(orders_, unit, true); }

void Battle::remove_projectiles(Unit& unit) { remove_owner_bursts(projectile_world_, unit); }

void Battle::detonate(Unit& unit, bool self_destruct) { detonate_unit_death(projectile_world_, unit, self_destruct); }

std::int32_t Battle::fire(Unit& unit, int slot, Unit* target, WorldPos& target_pos) {
    return fire_weapon(projectile_world_, unit, slot, target, target_pos);
}

MissionOrder* Battle::allocate_order() {
    // Orders carry the original's heap address (it can reach the stale stack
    // word); heap addresses vary between runs, so a plain sequence stands in.
    // Freed orders are reused last-freed-first with their address, as a heap hands back
    // the block it just took: goals and the route search keep plain pointers to orders
    // that were destroyed, and what reaches them is then the order now living there.
    if (!free_orders_.empty()) {
        MissionOrder* o = free_orders_.back();
        free_orders_.pop_back();
        const std::uint32_t address = o->address;
        *o = MissionOrder{};
        o->address = address;
        return o;
    }
    auto* o = new MissionOrder;
    o->address = next_order_address_;
    next_order_address_ += 0x58;
    return o;
}

void Battle::free_order(MissionOrder* order) {
    if (order->goal) motion_.retired_goals.push_back(order->goal);  // goals may still point here (prune_retired_goals)
    order->goal.reset();
    free_orders_.push_back(order);
}

void Battle::step() {
    // Game_RunSimulationStep 0x495490, one pending step (docs/RE_MEMORY.md "Battle step").
    advance_tick();                  // 1
    if (settings.net_game) outside_.network_input();  // 2
    step_units_and_projectiles();    // 3, 4
    step_effects();                  // 5
    players_stage();                 // 6
    step_features();                 // 7
    // 8: texture animation cursors (presentation), wind, meteors, camera (its shake decay).
    step_wind();
    step_meteor_shower();
    step_camera_shake(shake, crt_rand);
    {                                // 9: the effect sweep, lists 0-9 in order (6: nano streams)
        FxContext c = fx_context();
        step_effect_lists(c, 0, 6);
        step_nano();
        c.nano_streams = static_cast<std::int32_t>(nano.streams.size());
        step_effect_lists(c, 7, EffectLists::kLists);
    }
    // The minimap blink counter (presentation) runs here.
    if (settings.net_game && settings.net_frame_timing) {  // 10
        auto_share_resources();
        outside_.network_flush();
    }
}

void Battle::step_meteor_shower() {
    std::int32_t pos[3], vel[3];
    if (!step_meteors(meteors, unit_env_.game_tick, map.width, map.height, crt_rand, pos, vel)) return;
    sync_settings();
    if (meteors.weapon) spawn_free_projectile(projectile_world_, *meteors.weapon, Vec3i{pos[0], pos[1], pos[2]}, Vec3i{vel[0], vel[1], vel[2]}, true);
}

void Battle::end_frame() {
    // Drawing: every debris piece with smoke starts a puff, with fire a jittered flame
    // (0x421550, before the frame's other drawing); then lingering sight expires.
    if (settings.draw_side_effects)
        for (int k = 0; k < kMaxDebris; ++k) {
            if (effects.debris_block[static_cast<std::size_t>(k)] == -1) continue;
            const DebrisPiece& d = effects.debris[static_cast<std::size_t>(k)];
            FxContext c = fx_context();
            if (d.flags & 2u) fx_smoke(c, d.move, 9);
            if (d.flags & 1u) fx_flame_jittered(c, d.move, 9);
        }
    expire_lingering_sight();
}

void Battle::units_drawn(const std::vector<std::uint16_t>& ids) {
    // 0x45AC20 before it draws: nothing for a carried unit; else the unit's pose, then every
    // unit in its cargo list without the hidden flag (0x20000).
    for (const std::uint16_t id : ids) {
        if (id == 0 || id >= units.size()) continue;
        Unit& u = units[id];
        if (u.carrier) continue;
        u.pose.sync(u.pitch, u.heading, u.bank);
        for (Unit* c = u.cargo_head; c; c = c->cargo_next)
            if (!(c->flags & 0x20000u)) c->pose.sync(c->pitch, c->heading, c->bank);
    }
}

void Battle::emit_sfx(Unit& u, std::int32_t piece, std::int32_t type) {
    // A unit script's effect (0x480EB0): only for units the viewed player sees. The pose is
    // brought up to date; the effect starts at the piece's first point and points at its
    // second (types without 0x100), or at the piece's origin (0x100 types).
    outside_.emit_sfx(u, piece, type);
    if (!targeting_hooks_.unit_visible(settings.local_view, u)) return;
    u.pose.sync(u.pitch, u.heading, u.bank);
    if (piece < 0 || static_cast<std::size_t>(piece) >= u.pose.pieces.size()) {
        effects.fault = "effect from a piece outside the object state";
        return;
    }
    const PosePiece& pp = u.pose.pieces[static_cast<std::size_t>(piece)];
    std::array<std::int32_t, 3> at{}, to{};
    const auto add = [](std::int32_t a, std::int32_t b) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b)); };
    const auto sub = [](std::int32_t a, std::int32_t b) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b)); };
    if (!(static_cast<std::uint32_t>(type) & 0x100u)) {
        if (pp.points.size() < 6) {
            effects.fault = "effect from a piece with fewer than two points";
            return;
        }
        at = {add(u.pos[0], pp.points[0]), add(u.pos[1], pp.points[1]), sub(u.pos[2], pp.points[2])};
        to = {add(u.pos[0], pp.points[3]), add(u.pos[1], pp.points[4]), sub(u.pos[2], pp.points[5])};
    } else {
        at = {add(u.pos[0], pp.xform[0]), add(u.pos[1], pp.xform[1]), sub(u.pos[2], pp.xform[2])};
    }
    FxContext c = fx_context();
    switch (type) {
    case 0: fx_flame(c, at, to, 1, 6, 7); break;
    case 1: fx_flame(c, at, to, 1, 7, 7); break;
    case 2: fx_wake(c, at, to, 0x10, true, 2); break;
    case 3: fx_wake(c, at, to, 8, true, 2); break;
    case 4: fx_wake(c, to, at, 0x10, true, 2); break;
    case 5: fx_wake(c, to, at, 8, true, 2); break;
    case 0x101: fx_smoke(c, at, 9, false); break;
    case 0x102: fx_smoke(c, at, 9, true); break;
    case 0x103: {
        const std::array<std::int32_t, 3> sea = {at[0], static_cast<std::int32_t>(static_cast<std::uint32_t>(settings.sea_level) << 16), at[2]};
        fx_wake(c, at, sea, 8, false, 7);
        break;
    }
    default: break;
    }
}

void Battle::attach_unit(Unit& u, std::int32_t passenger, std::int32_t piece, std::int32_t mode) {
    sync_settings();
    script_attach_unit(missions_, u, passenger, piece, mode);
}

void Battle::drop_unit(Unit& u, std::int32_t passenger) {
    sync_settings();
    script_drop_unit(missions_, u, passenger);
}

std::shared_ptr<OrderGoal> Battle::UnitMissionHooks::make_goal(Unit& u, MissionOrder& o, const GoalRequest& g) {
    std::shared_ptr<OrderGoal> goal;
    switch (g.kind) {
    case GoalRequest::Kind::Point: goal = make_point_goal(b.motion_, o, g.pos); break;
    case GoalRequest::Kind::Follow: goal = make_follow_goal(b.motion_, o, g.target); break;
    case GoalRequest::Kind::Dock: goal = make_dock_goal(b.motion_, o, g.target, g.piece); break;
    case GoalRequest::Kind::Face: goal = make_face_goal(b.motion_, o, g.target, g.pos); break;
    case GoalRequest::Kind::Manoeuvre: goal = make_manoeuvre_goal(b.motion_, o, g.pos, g.vel); break;
    }
    for (int k = 0; k < g.set_count; ++k) adjust_goal(u, *goal, g.sets[static_cast<std::size_t>(k)].first, g.sets[static_cast<std::size_t>(k)].second);
    return goal;
}

void Battle::UnitMissionHooks::adjust_goal(Unit&, OrderGoal& goal, GoalRequest::Set what, std::int32_t value) {
    if (auto* p = dynamic_cast<PointGoal*>(&goal)) {
        switch (what) {
        case GoalRequest::Set::Altitude: p->set_altitude(b.motion_, value); break;
        case GoalRequest::Set::Radius: p->set_radius(static_cast<std::int16_t>(value)); break;
        case GoalRequest::Set::Heading: p->set_heading(static_cast<std::int16_t>(value)); break;
        default: break;
        }
    } else if (auto* mg = dynamic_cast<ManoeuvreGoal*>(&goal)) {
        if (what == GoalRequest::Set::TargetHeading) mg->set_target_heading(static_cast<std::uint16_t>(value));
    }
}

void Battle::UnitMissionHooks::air_bases_in_radius(int state, const std::array<std::int32_t, 3>& pos, std::int32_t r,
                                                   std::vector<Unit*>& out) {
    const bool ok = state >= 0 && static_cast<std::size_t>(state) < b.targeting_state_.tables.size() &&
                    sim::air_bases_in_radius(b.targeting_state_.tables[static_cast<std::size_t>(state)], b.ticks, pos, r, out);
    if (!ok && b.missions_.fault.empty()) b.missions_.fault = "air base search on a player without a unit table";
}

void Battle::UnitMissionHooks::broadcast_script_call(Unit& u, const char* name, std::int32_t argc, std::int32_t a, std::int32_t c1,
                                                     std::int32_t c2, std::int32_t c3) {
    const int fn = u.script ? u.script->program().find_function(name) : -1;
    b.outside_.broadcast_script_call_named(u, static_cast<std::int16_t>(fn), argc, a, c1, c2, c3);
}

void Battle::UnitMissionHooks::wreck_attitude(const MapCell& origin, Unit& u) {
    const std::size_t slot = origin.feature_aux;
    if (slot >= b.feature_state.slots.size()) {
        if (b.missions_.fault.empty()) b.missions_.fault = "wreck attitude from a feature slot past the table";
        return;
    }
    const FeatureSlot& f = b.feature_state.slots[slot];
    u.pitch = f.get<std::int16_t>(FeatureSlot::kAngles);
    u.heading = f.get<std::uint16_t>(FeatureSlot::kAngles + 2);
    u.bank = f.get<std::int16_t>(FeatureSlot::kAngles + 4);
}

} // namespace sim
