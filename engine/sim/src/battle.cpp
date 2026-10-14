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
      motion_{units, ticks, players, motion_types_, heights_, motion_hooks_, {}, {}},
      missions_{orders_, units, combat_, *this, mission_hooks_, mission_types_, alliances, mission_env_} {
    alliances.resize(kPlayerRecords);
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
}

void Battle::step_units_and_projectiles() {
    sync_settings();
    run_unit_pass(units, ticks, players, unit_env_, *this, camera_countdown_);
    update_projectiles(projectile_world_);
}

Unit* Battle::unit_by_id(std::uint16_t id) { return id < units.size() ? &units[id] : nullptr; }

void Battle::claim_yard_cells(Unit& u) {
    // Yard opening (0x47DAC0) after the cells were found free and the yard flag changed.
    claim_wanted_cells(footprint_, u);
    outside_.update_passability(u.cell_x, u.cell_z, u.footprint_x, u.footprint_z);
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
