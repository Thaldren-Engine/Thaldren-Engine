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
      construction_{map, features, construction_types_, heights_, construction_hooks_, construction_env_} {
    missions_.construction = &construction_;
    alliances.resize(kPlayerRecords);
    feature_state.reset();
    effects.reset();
    types[0].info.max_damage = 0;  // the blank type of empty slots
    types[0].tick.damage_modifier = 0;
}

Battle::~Battle() {
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
    route_.env.game_tick = unit_env_.game_tick;
    route_.env.sea_level = s.sea_level;
    route_.env.player_count = s.player_count;
    route_.env.units_per_player = s.units_per_player;
}

void Battle::start_players() {
    sync_settings();
    create_player_targeting(targeting_);
}

void Battle::players_stage() {
    sync_settings();
    sim::run_route_search(route_);  // the route search runs first
    for (int slot = 0; slot < 10; ++slot) {
        const PlayerSlot& p = players[static_cast<std::size_t>(slot)];
        if (!p.active || p.controller < 1 || p.controller > 3 || p.state == 10) continue;
        if (targeting_state_.controllers[static_cast<std::size_t>(slot)].exists) {
            // The player's controller: an AI player's own work, then the weapon-target pass.
            const bool ai = p.controller == 2;
            if (ai) outside_.ai_controller_step(slot);
            weapon_target_pass(targeting_, slot, ai);
        }
        refresh_unit_table(targeting_, slot);
        for (int id = p.first_unit; id <= p.last_unit && id < static_cast<int>(units.size()); ++id)
            if (id > 0 && (units[static_cast<std::size_t>(id)].flags & unit_flags::kAlive)) outside_.update_sight(units[static_cast<std::size_t>(id)]);
        outside_.player_stage_rest(slot);
    }
    outside_.players_stage_end();
}

std::int32_t Battle::TargetingHooks::intercept_target(Unit& u, int slot, std::array<std::int32_t, 3>& pos) {
    const std::int32_t i = find_intercept_target(b.projectile_world_, u, slot);
    if (i >= 0) pos = b.projectiles.slots[static_cast<std::size_t>(i)].pos;
    return i;
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
    auto* o = new MissionOrder;
    o->address = next_order_address_;
    next_order_address_ += 0x58;
    return o;
}

void Battle::free_order(MissionOrder* order) { delete order; }

} // namespace sim
