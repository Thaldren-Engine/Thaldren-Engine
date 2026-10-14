#pragma once
// A battle: all simulation state of one game and the per-step routines wired
// together in the original's order (see docs/RE_MEMORY.md, "Battle step").
//
// The systems built so far call each other directly here; everything else —
// systems not built yet (motion, mission behaviours, transport, economy,
// features, line of sight), presentation, network and user interface — goes to
// one BattleOutside interface.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "sim/combat.h"
#include "sim/fire.h"
#include "sim/footprint.h"
#include "sim/ground_settle.h"
#include "sim/heightmap.h"
#include "sim/missions.h"
#include "sim/motion.h"
#include "sim/orders.h"
#include "sim/projectile.h"
#include "sim/rng.h"
#include "sim/unit.h"
#include "sim/unit_create.h"
#include "sim/unit_tick.h"
#include "sim/weapons.h"

namespace sim {

// One unit type as the battle uses it (index = the type index units carry).
struct BattleUnitType {
    UnitTypeInfo info;
    UnitTickTypeInfo tick;
    GroundSettleType settle;
    UnitCreateType create;
    MotionType motion;
    MissionTypeInfo mission;
};

// Settings and per-step inputs shared by the systems.
struct BattleSettings {
    std::uint8_t sea_level = 0;
    std::int32_t gravity = 0;
    bool net_game = false;            // network game: state broadcasts and packets
    std::uint8_t local_player = 0;
    std::uint8_t local_view = 0;
    std::int32_t game_mode = 0;       // nonzero: a commander's death ends its player
    std::int32_t ai_difficulty = 0;
    bool fog_of_war = false;
    bool water_damage_enabled = false;
    std::int32_t water_damage = 0;
    std::uint16_t damage_options = 0;  // bit 7 double damage, bit 8 half damage
    bool no_sea_level_trigger = false;
    Vec3i wind_drift{};               // per-step drift of ballistic and dropped shots (from the wind stage)
    std::int32_t script_tick_rate = 30;  // steps per second unit scripts are built with
    OriginalAddresses addresses;
};

// Everything outside the systems built so far. Each method names the original
// routine (or system) it stands for in battle.cpp.
class BattleOutside {
public:
    virtual ~BattleOutside() = default;
    // Not built yet: motion.
    // Motion: the placement check of a move into new cells (0x47DB70), the highest
    // terrain of a unit's spatial bucket, the route search (pathfinder) and sight.
    virtual bool can_place(Unit& unit, std::int16_t cell_x, std::int16_t cell_z, std::uint32_t layer) = 0;
    virtual std::uint8_t bucket_height(const Unit& unit) = 0;
    virtual void route_search_forget(MotionGoal& goal) = 0;
    virtual void update_sight(Unit& unit) = 0;
    // Mission behaviours: targeting, attack orders, mission resolution, construction decay.
    virtual void enemies_in_radius(int player, const std::array<std::int32_t, 3>& pos, std::int32_t radius,
                                   std::vector<Unit*>& out) = 0;
    virtual Unit* fire_at_will_target(Unit& unit) = 0;
    virtual bool issue_attack_order(Unit& unit, Unit& target, bool forced) = 0;
    virtual std::uint8_t resolve_mission(OrderIntent intent, Unit& unit, Unit* target, const std::array<std::int32_t, 3>* pos) = 0;
    virtual void unfinished_decay(Unit& unit, std::int32_t ticks) = 0;
    virtual bool use_resources_if_not_stalled(Unit& unit, float energy, float metal) = 0;
    // Unit creation and footprints (sim/unit_create.h, sim/footprint.h): the parts owned by
    // other systems. Motion object of a new mobile unit (0x43DC00; the battle has already set
    // has_motion), its footprint step when it claims cells, the movement classes' passability.
    virtual void update_passability(std::int16_t x, std::int16_t z, std::int16_t w, std::int16_t h) = 0;
    virtual void passability_after_release(Unit& unit) = 0;
    // Group list membership (0x480250, group 0 for a new unit), line of sight (0x482AC0) and
    // the victory conditions' unit-created notification (0x490580).
    virtual void join_group(Unit& unit, std::int32_t group) = 0;
    virtual void stamp_line_of_sight(Unit& unit) = 0;
    virtual void victory_unit_created(Unit& unit) = 0;
    // Not built yet: mission behaviours, attack retargeting, transport, economy, features.
    virtual std::int32_t run_mission(Unit& unit, MissionOrder& order, std::uint32_t& event) = 0;
    virtual void retarget(Unit* attacker, Unit& victim, std::uint16_t amount) = 0;
    virtual void attach_unit(Unit& unit, std::int32_t passenger, std::int32_t piece, std::int32_t mode) = 0;
    virtual void drop_unit(Unit& unit, std::int32_t passenger) = 0;
    virtual void detach_from_carrier(Unit& unit) = 0;
    virtual void release_unit(Unit& unit) = 0;
    virtual bool pay_energy(Unit& worker, float amount) = 0;
    virtual void damage_feature(MapCell& cell, std::int32_t x, std::int32_t z, const WeaponType& weapon) = 0;
    virtual Vec3i feature_anim_pos(std::uint16_t slot) = 0;
    virtual void spawn_wreck(Unit& unit, std::int32_t level, bool burn) = 0;
    virtual void explode_piece(Unit& unit, std::int32_t piece, std::int32_t type) = 0;  // debris (draws the battle RNG)
    virtual void emit_sfx(Unit& unit, std::int32_t piece, std::int32_t type) = 0;
    virtual void reveal_dead_unit(Unit& unit) = 0;
    virtual void forget_fog(Unit& unit) = 0;
    virtual void victory_unit_destroyed(Unit& unit) = 0;
    virtual void player_out_of_units(int player) = 0;
    virtual void commander_died(int player) = 0;
    // The real-time clock in steps (read by the hover bob; not deterministic in the original).
    virtual std::uint32_t clock_ticks() = 0;
    // Network.
    virtual void network_send(std::int32_t net_id, const std::uint8_t* packet, std::int32_t size) = 0;
    virtual void broadcast_player_units(int player) = 0;
    virtual void broadcast_script_call(Unit& unit, std::int16_t function) = 0;
    // Presentation and user interface (no effect on the simulation).
    virtual void on_activate_flags_changed(Unit& unit, std::uint8_t gained, std::uint8_t lost) = 0;
    virtual void speech(Unit& unit, std::uint32_t id, SpeechText text) = 0;
    virtual void display_changed(Unit& unit) = 0;
    virtual void build_counts_changed(Unit& unit) = 0;
    virtual void nano_beam(const std::array<std::int32_t, 3>& from, const std::array<std::int32_t, 6>& box) = 0;
    virtual void forget_speech(Unit& unit) = 0;
    virtual void leave_group(Unit& unit) = 0;
    virtual void stats_activity(std::int32_t kind) = 0;
    virtual void kill_counted(std::uint8_t attacker_slot, Unit& victim) = 0;
    virtual void stockpile_used(Unit& unit) = 0;
    virtual void camera_follow_step() = 0;
    virtual void play_sound(std::uint16_t sound, const Vec3i& pos) = 0;
    virtual void spawn_smoke(const Vec3i& pos) = 0;
    virtual void spawn_muzzle_smoke(const Vec3i& pos) = 0;
    virtual void spawn_explosion(const Vec3i& pos, const WeaponType& weapon, bool water_art, bool in_water) = 0;
    virtual void camera_shake(std::int32_t x, std::int32_t y, std::int32_t duration) = 0;
};

class Battle final : UnitWorld,
                     FootprintSystems,
                     UnitTickSystems,
                     CombatSystems,
                     WeaponSystems,
                     ProjectileSystems,
                     OrderSystems,
                     GroundSettleSystems {
public:
    static constexpr int kPlayerRecords = 11;  // ten slots and the "no owner" record

    Battle(BattleOutside& outside, const HeightMap& heights, BattleRng& rng);
    ~Battle() override;
    Battle(const Battle&) = delete;
    Battle& operator=(const Battle&) = delete;

    // State (index = unit id / player slot / type index; entry 0 of the unit table is unused).
    std::vector<Unit> units;
    std::vector<UnitTickState> ticks;
    std::vector<PlayerSlot> players;
    std::vector<BattleUnitType> types;  // [0] = the blank type of empty table slots
    // Alliance bytes of each player record, indexed by the other player's state index.
    std::vector<std::array<std::uint8_t, 16>> alliances;
    std::vector<FeatureTypeInfo> features;
    ProjectileTable projectiles;
    MapGrid map;
    UnitBuckets buckets;  // sized by init_buckets() once the map is set
    BattleSettings settings;

    // Setup: `count` unit table entries (ids 0..count-1; entry 0 unused), and a player's
    // inclusive range of them (the entries' owner is fixed by it).
    void init_unit_table(std::size_t count);
    void assign_unit_range(int player, int first, int last);
    void init_buckets() { buckets.resize_for_map(map.width, map.height); }

    // Creates a unit of type `type` for `player` at (x, y, z) (16.16) in the first free
    // entry of the player's range, or exactly entry `wanted_id` when nonzero (the original's
    // Unit_CreateFromDefId 0x485F50). `layer` gives the low two unit flags (1 = ground).
    // Unfinished units start with no hit points and the whole build fraction left.
    // Returns nullptr when the type cannot be made, the player's limit for it is reached or
    // there is no free entry.
    Unit* create_unit(int player, std::uint16_t type, std::int32_t x, std::int32_t y, std::int32_t z, bool fully_built,
                      std::uint32_t layer, std::uint16_t wanted_id = 0);

    // Per-step inputs the original reads from its game state.
    UnitTickEnvironment& unit_env() { return unit_env_; }

    std::uint32_t game_tick() const { return unit_env_.game_tick; }

    // The unit pass and then the projectile update of one step (the original's
    // stages 2 and 3; the caller advances the tick first and runs the other stages).
    void step_units_and_projectiles();
    void advance_tick() { ++unit_env_.game_tick; }

    // Contexts for direct calls (tests, other stages).
    Combat& combat() { return combat_; }
    Weapons& weapon_context() { return weapons_; }
    ProjectileWorld& projectile_world() { return projectile_world_; }
    OrderContext& order_context() { return orders_; }
    FootprintWorld& footprint_world() { return footprint_; }
    UnitWorld& unit_world() { return *this; }  // for unit_get_value / unit_set_value / unit_set_activate

private:
    void sync_settings();
    void init_unit_fields(Unit& u, UnitTickState& t, const BattleUnitType& type, std::int32_t x, std::int32_t y,
                          std::int32_t z, bool fully_built);
    void build_script_and_pose(Unit& u, const UnitCreateType& type);

    // UnitWorld
    Unit* unit_by_id(std::uint16_t id) override;
    const HeightMap& heightmap() override { return heights_; }
    bool yard_cells_free(Unit& u, std::int32_t v) override { return sim::yard_cells_free(footprint_, u, v); }
    void claim_yard_cells(Unit& u) override;
    void on_activate_flags_changed(Unit& u, std::uint8_t g, std::uint8_t l) override { outside_.on_activate_flags_changed(u, g, l); }
    std::uint32_t random_mod(std::uint32_t n) override { return rng_.next_mod(n); }
    void emit_sfx(Unit& u, std::int32_t p, std::int32_t t) override { outside_.emit_sfx(u, p, t); }
    void explode_piece(Unit& u, std::int32_t p, std::int32_t t) override { outside_.explode_piece(u, p, t); }
    void attach_unit(Unit& u, std::int32_t a, std::int32_t p, std::int32_t m) override { outside_.attach_unit(u, a, p, m); }
    void drop_unit(Unit& u, std::int32_t a) override { outside_.drop_unit(u, a); }

    // UnitTickSystems
    void weapons(Unit& unit, std::uint32_t& stale_word) override;
    void damage(Unit* attacker, Unit& target, std::int32_t amount, std::int32_t kind, std::int32_t heading) override;
    void repair_heal(Unit& worker, Unit& target, float rate) override;
    void primary_orders(Unit& unit, std::uint32_t& stale_word) override;
    void secondary_orders(Unit& unit, std::uint32_t& stale_word) override;
    void motion(Unit& unit, std::uint32_t& stale_word) override;
    void settle_height(Unit& unit, std::uint32_t& stale_word) override;
    void killed(Unit& unit, std::uint8_t kind, std::uint32_t& stale_word) override;
    void broadcast_player_units(int player, std::uint32_t&) override { outside_.broadcast_player_units(player); }
    void camera_follow_step(std::uint32_t&) override { outside_.camera_follow_step(); }

    // CombatSystems
    void retarget(Unit* a, Unit& v, std::uint16_t amount) override { outside_.retarget(a, v, amount); }
    void add_paralysis(Unit& victim, std::uint16_t amount) override;
    bool pay_energy(Unit& w, float amount) override { return outside_.pay_energy(w, amount); }
    void stats_activity(std::int32_t kind) override { outside_.stats_activity(kind); }
    void network_send(std::int32_t net, const std::uint8_t* p, std::int32_t size) override { outside_.network_send(net, p, size); }
    void reveal_dead_unit(Unit& u) override { outside_.reveal_dead_unit(u); }
    void victory_unit_destroyed(Unit& u) override { outside_.victory_unit_destroyed(u); }
    void clear_orders(Unit& unit) override;
    void forget_speech(Unit& u) override { outside_.forget_speech(u); }
    void leave_group(Unit& u) override { outside_.leave_group(u); }
    void remove_projectiles(Unit& unit) override;
    void detach_from_carrier(Unit& u) override { outside_.detach_from_carrier(u); }
    void clear_footprint(Unit& u) override { sim::clear_footprint(footprint_, u); }
    void forget_fog(Unit& u) override { outside_.forget_fog(u); }
    void kill_counted(std::uint8_t slot, Unit& v) override { outside_.kill_counted(slot, v); }
    void detonate(Unit& unit, bool self_destruct) override;
    void spawn_wreck(Unit& u, std::int32_t level, bool burn) override { outside_.spawn_wreck(u, level, burn); }
    void release_unit(Unit& u) override { outside_.release_unit(u); }
    void release_motion(Unit& u) override { destroy_motion(motion_, u); }
    void player_out_of_units(int p) override { outside_.player_out_of_units(p); }
    void commander_died(int p) override { outside_.commander_died(p); }

    // WeaponSystems
    std::int32_t fire(Unit& unit, int slot, Unit* target, WorldPos& target_pos) override;
    void stockpile_used(Unit& u) override { outside_.stockpile_used(u); }

    // ProjectileSystems
    const WeaponType* slot_weapon(const Unit& u, int slot) override { return u.weapons[static_cast<std::size_t>(slot)].type; }
    std::uint16_t slot_fire_heading(const Unit& u, int slot) override { return u.weapons[static_cast<std::size_t>(slot)].aim_heading; }
    std::int32_t motion_speed(const Unit& u) override { return sim::motion_speed(ticks[u.id]); }
    void play_sound(std::uint16_t s, const Vec3i& p) override { outside_.play_sound(s, p); }
    void spawn_smoke(const Vec3i& p) override { outside_.spawn_smoke(p); }
    void spawn_muzzle_smoke(const Vec3i& p) override { outside_.spawn_muzzle_smoke(p); }
    void spawn_explosion(const Vec3i& p, const WeaponType& w, bool wa, bool iw) override { outside_.spawn_explosion(p, w, wa, iw); }
    void camera_shake(std::int32_t x, std::int32_t y, std::int32_t d) override { outside_.camera_shake(x, y, d); }
    void damage_feature(MapCell& c, std::int32_t x, std::int32_t z, const WeaponType& w) override { outside_.damage_feature(c, x, z, w); }
    Vec3i feature_anim_pos(std::uint16_t slot) override { return outside_.feature_anim_pos(slot); }

    // OrderSystems
    std::int32_t run_mission(Unit& u, MissionOrder& o, std::uint32_t& event) override { return run_mission_behaviour(missions_, u, o, event); }
    void clear_weapon_target(Unit& unit, int slot) override { sim::clear_weapon_target(unit, slot); }
    void release_weapon_targets(Unit& unit) override { sim::release_weapon_targets(unit, 3); }
    void release_attached(Unit& u, MissionOrder& o) override { release_order_goal(motion_, u, o); }
    void broadcast_script_call(Unit& u, std::int16_t f) override { outside_.broadcast_script_call(u, f); }
    MissionOrder* allocate_order() override;
    void free_order(MissionOrder* order) override;

    // FootprintSystems
    void update_passability(std::int16_t x, std::int16_t z, std::int16_t w, std::int16_t h) override {
        outside_.update_passability(x, z, w, h);
    }
    void passability_after_release(Unit& u) override { outside_.passability_after_release(u); }
    void motion_cells_claimed(Unit& u) override;

    // GroundSettleSystems
    std::int32_t motion_speed(Unit& u) override { return sim::motion_speed(ticks[u.id]); }
    std::uint32_t motion_start_tick(Unit& u) override { return motion_move_tick(ticks[u.id]); }

    // Motion and mission hooks (separate objects: some names clash with the combat hooks).
    struct MotionHooks final : MotionSystems {
        Battle& b;
        explicit MotionHooks(Battle& battle) : b(battle) {}
        bool can_place(Unit& u, std::int16_t x, std::int16_t z, std::uint32_t layer) override { return b.outside_.can_place(u, x, z, layer); }
        void clear_footprint(Unit& u) override { release_cells(b.footprint_, u); }  // 0x47D0E0
        void claim_footprint(Unit& u) override { sim::claim_footprint(b.footprint_, u); }
        void update_sight(Unit& u) override { b.outside_.update_sight(u); }
        bool off_map(const Unit& u) override { return u.bucket == kOffMapBucket; }
        std::uint8_t bucket_height(const Unit& u) override { return b.outside_.bucket_height(u); }
        void route_search_forget(MotionGoal& g) override { b.outside_.route_search_forget(g); }
        void set_activated(Unit& u, bool on) override { unit_set_activate(u, b, unit_flags::kActivated, on); }
        std::uint32_t allocate_object(std::uint32_t) override { return 0; }
        void free_object(std::uint32_t) override {}
    };
    struct MissionHooks final : MissionSystems {
        Battle& b;
        explicit MissionHooks(Battle& battle) : b(battle) {}
        std::int32_t run_other(Unit& u, MissionOrder& o, std::uint32_t& event) override { return b.outside_.run_mission(u, o, event); }
        void speech(Unit& u, std::uint32_t id, SpeechText t) override { b.outside_.speech(u, id, t); }
        void enemies_in_radius(int p, const std::array<std::int32_t, 3>& pos, std::int32_t r, std::vector<Unit*>& out) override {
            b.outside_.enemies_in_radius(p, pos, r, out);
        }
        Unit* fire_at_will_target(Unit& u) override { return b.outside_.fire_at_will_target(u); }
        bool issue_attack_order(Unit& u, Unit& t, bool forced) override { return b.outside_.issue_attack_order(u, t, forced); }
        std::uint8_t resolve_mission(OrderIntent i, Unit& u, Unit* t, const std::array<std::int32_t, 3>* pos) override {
            return b.outside_.resolve_mission(i, u, t, pos);
        }
        void unfinished_decay(Unit& u, std::int32_t ticks) override { b.outside_.unfinished_decay(u, ticks); }
        void display_changed(Unit& u) override { b.outside_.display_changed(u); }
        bool use_resources_if_not_stalled(Unit& u, float e, float m) override { return b.outside_.use_resources_if_not_stalled(u, e, m); }
        void build_counts_changed(Unit& u) override { b.outside_.build_counts_changed(u); }
        void nano_beam(const std::array<std::int32_t, 3>& from, const std::array<std::int32_t, 6>& box) override { b.outside_.nano_beam(from, box); }
        void drop_goal_object(Unit& u, MissionOrder& o) override { release_order_goal(b.motion_, u, o); }
    };
    std::uint32_t clock_ticks() override { return outside_.clock_ticks(); }

    BattleOutside& outside_;
    const HeightMap& heights_;
    BattleRng& rng_;
    UnitTickEnvironment unit_env_;
    CombatEnvironment combat_env_;
    WeaponEnvironment weapon_env_;
    ProjectileEnvironment projectile_env_;
    Combat combat_;
    Weapons weapons_;
    ProjectileWorld projectile_world_;
    OrderContext orders_;
    FootprintWorld footprint_;
    MotionHooks motion_hooks_{*this};
    MissionHooks mission_hooks_{*this};
    std::vector<MotionType> motion_types_;        // by type index (copied from types each step)
    std::vector<MissionTypeInfo> mission_types_;  // by type index
    MotionContext motion_;
    MissionEnvironment mission_env_;
    Missions missions_;
    std::vector<std::unique_ptr<UnitScriptHost>> script_hosts_;  // by unit id
    std::int16_t camera_countdown_ = 0;
    std::uint32_t next_order_address_ = 0x01000000;
};

} // namespace sim
