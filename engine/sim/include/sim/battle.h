#pragma once
// A battle: all simulation state of one game and the per-step routines wired
// together in the original's order (see docs/RE_MEMORY.md, "Battle step").
//
// The systems built so far call each other directly here; everything else —
// systems not built yet (mission behaviours, transport, line of sight, AI),
// presentation, network and user interface — goes to one BattleOutside interface.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "sim/ai.h"
#include "sim/anim.h"
#include "sim/bitstream.h"
#include "sim/combat.h"
#include "sim/economy.h"
#include "sim/construction.h"
#include "sim/effects.h"
#include "sim/effect_lists.h"
#include "sim/features.h"
#include "sim/fire.h"
#include "sim/footprint.h"
#include "sim/ground_settle.h"
#include "sim/heightmap.h"
#include "sim/missions.h"
#include "sim/mission_goals.h"
#include "sim/motion.h"
#include "sim/net_units.h"
#include "sim/nano_spray.h"
#include "sim/orders.h"
#include "sim/projectile.h"
#include "sim/rng.h"
#include "sim/targeting.h"
#include "sim/route_search.h"
#include "sim/sight.h"
#include "sim/sky.h"
#include "sim/unit.h"
#include "sim/unit_create.h"
#include "sim/unit_missions.h"
#include "sim/unit_tick.h"
#include "sim/victory.h"
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
    TargetingTypeInfo targeting;
    RouteType route;  // movement class and placement limits (footprint and movement come from `create`)
    EconomyType economy;  // production, upkeep and storage (sim/economy.h)
    SightType sight;  // sight, radar, sonar, jamming, cloak distance (sim/sight.h)
    ConstructionTypeInfo construction;  // building, reclaiming and placement (sim/construction.h)
    AiTypeInfo ai;  // what the computer player reads of the type (sim/ai.h)
    UnitMissionTypeInfo unit_missions;  // aircraft and transport orders, kamikaze (sim/unit_missions.h)
};

// Settings and per-step inputs shared by the systems.
struct BattleSettings {
    std::uint8_t sea_level = 0;
    // Gravity in world units (16.16) per step per step (GS+0x14263; the map load turns the map's
    // value per second squared into it, see RE_MEMORY "Motion").
    std::int32_t gravity = 0;
    // The map info's own "gravity" value as written (map info +0xD3C: 0 when the key is
    // missing, -1 without a map info). Only the bomb run's drop lead reads it (AirStrike).
    std::int32_t map_gravity = -1;
    bool net_game = false;            // network game: state broadcasts and packets
    std::uint8_t local_player = 0;
    std::uint8_t local_view = 0;
    std::int32_t game_mode = 0;       // nonzero: a commander's death ends its player
    std::int32_t ai_difficulty = 0;
    bool fog_of_war = false;          // line of sight option: live sight counts (sim/sight.h)
    bool explore = false;             // mapping option: the map starts unexplored and units explore it
    bool true_sight = false;          // line of sight type: rays over the terrain (else circles)
    std::array<SightPlayerOptions, 11> player_sight{};  // per player: shares radar, watching
    bool water_damage_enabled = false;
    std::int32_t water_damage = 0;
    std::uint16_t damage_options = 0;  // bit 3 weapons damage features, bit 7 double damage, bit 8 half damage
    bool no_sea_level_trigger = false;
    Vec3i wind_drift{};               // per-step drift of ballistic and dropped shots (from the wind stage)
    std::int32_t script_tick_rate = 30;  // steps per second unit scripts are built with
    std::uint16_t unit_limit = 250;      // units per player (the weapon-target pass visits limit/30 + 1 per step)
    bool target_everything = false;      // game option: units without "shoot me" are targets for everyone
    std::int32_t command_mode = 0;       // game option: 1 = the alternative default-command rules
    std::uint16_t player_count = 0;      // players in the game (the route search shares its budget by it)
    std::uint16_t units_per_player = 0;  // size of a player's part of the unit table
    bool lava_world = false;          // lava instead of water splashes
    std::uint8_t feature_network_role = 0;  // feature damage: 0 alone, 1 network host, 2 other machine
    float tidal_strength = 0.0f;      // the map's tidal strength (tidal generators)
    std::uint16_t build_menu_focus = 0;     // unit whose build menu this machine shows (user interface)
    std::int32_t network_provider = 0;      // the map info's provider type (3: reclaims are announced)
    // Inputs the original takes from stale stack memory (sim/unit_missions.h): the y of the
    // reclaim sites repair patrols find, and the address the air repair patrol leaves in its
    // event argument.
    std::int32_t reclaim_site_y = 0;
    std::uint32_t air_repair_patrol_site_address = 0;
    std::int32_t provider = 2;              // the map info's kind of game: 1 campaign, 2 skirmish, 3 network
    bool cd_mismatch = false;               // the copy-protection check failed (forces a defeat later)
    bool shake_disabled = false;            // the player turned camera shaking off (no rand() draws)
    bool net_frame_timing = true;           // network games: the end-of-step sharing and flush run (0x506DBC)
    // Drawing a frame starts particle emitters for flaming and smoking debris pieces in the
    // original (0x421550), which draws rand(); on: Battle::end_frame() does the same.
    bool draw_side_effects = false;
    OriginalAddresses addresses;
    // The computer player (sim/ai.h): the map's surface metal (the OTA schema's SurfaceMetal)
    // and the stack word the constructors' build sites take their height from (an input).
    std::int32_t surface_metal = 0;
    std::int32_t ai_site_height_word = 0;
};

// Everything outside the systems built so far. Each method names the original
// routine (or system) it stands for in battle.cpp.
class BattleOutside {
public:
    virtual ~BattleOutside() = default;
    // Motion: the highest terrain of a unit's spatial bucket and sight.
    virtual std::uint8_t bucket_height(const Unit& unit) = 0;
    virtual void update_sight(Unit& unit) = 0;
    // Placement of a structure by its yard map (0x47D2E0; the rest of the placement check is built).
    virtual bool structure_fits(std::uint16_t type, std::int16_t cell_x, std::int16_t cell_z) = 0;
    // Mission behaviours: construction decay.
    virtual void unfinished_decay(Unit& unit, std::int32_t ticks) = 0;
    // Targeting (sim/targeting.h): line of sight (0x465AC0), the explored state of a map spot for
    // this machine's view, a feature type's flag byte, and the AI's share of the unit table refresh.
    virtual bool unit_visible(int player, Unit& unit) = 0;
    virtual bool position_explored(int player, const std::array<std::int32_t, 3>& pos) = 0;
    virtual std::uint8_t feature_flags(std::uint16_t feature) = 0;
    virtual void ai_own_unit(int table, Unit& unit) = 0;
    virtual void ai_rescore(int table) = 0;
    // Players' stage (0x464F80; its route search turn at the start is built in): an AI player's
    // own controller work (0x408C40 before its weapon-target pass); after each player's sight
    // updates the view player's minimap composite (0x466DC0, every step); the end-of-stage checks.
    virtual void ai_controller_step(int player) = 0;
    virtual void player_stage_rest(int player) = 0;
    virtual void players_stage_end() = 0;
    // Each player's 30-step turn in the players' stage, around its economy settlement (built
    // in, sim/economy.h): before it the local player's victory and defeat checks (they may
    // start the end of the game: Battle::end_game); after it the view player's radar, sonar
    // and cloak detection (0x467440, built in once init_sight() ran), fog minimap and network
    // view state.
    virtual void player_victory_turn(int) {}
    virtual void player_sensor_turn(int) {}
    // Network game: resources and the explored map shared automatically (packet 0x16;
    // kind 1 energy, 2 metal, from Battle::auto_share_resources).
    virtual void resources_shared(int /*kind*/, int /*from*/, int /*to*/, float /*amount*/) {}
    virtual void explored_map_shared(int /*from*/, int /*to*/) {}
    // Unit creation and footprints (sim/unit_create.h, sim/footprint.h): the parts owned by
    // other systems. Group list membership (0x480250, group 0 for a new unit), line of sight (0x482AC0) and
    // the victory conditions' unit-created notification (0x490580).
    virtual void join_group(Unit& unit, std::int32_t group) = 0;
    virtual void stamp_line_of_sight(Unit& unit) = 0;
    virtual void victory_unit_created(Unit& unit) = 0;
    // Not built yet: mission behaviours, attack retargeting, transport.
    virtual std::int32_t run_mission(Unit& unit, MissionOrder& order, std::uint32_t& event) = 0;
    virtual void attach_unit(Unit& unit, std::int32_t passenger, std::int32_t piece, std::int32_t mode) = 0;
    virtual void drop_unit(Unit& unit, std::int32_t passenger) = 0;
    virtual void detach_from_carrier(Unit& unit) = 0;
    virtual void release_unit(Unit& unit) = 0;
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
    virtual void under_attack_notice(Unit& unit) = 0;  // 0x47F850(unit, 2): spoken unless the unit is on screen
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
    virtual void camera_shake(std::int32_t x, std::int32_t y, std::int32_t duration) = 0;
    // Features and effects (sim/features.h, sim/effects.h): network packet 0x0F (6 bytes;
    // `to_host`: a non-host machine asking the host, else announced to all) and presentation.
    virtual void feature_packet(const std::uint8_t*, bool) {}
    virtual void feature_smoke(const Vec3i&) {}           // smoke of a burning feature (0x472810)
    virtual void feature_steam(const Vec3i&) {}           // geothermal vent (0x472C50)
    virtual void feature_fire_sound(std::int16_t, std::int16_t) {}  // "treeburn" at a cell
    virtual void wreck_fire(const Vec3i&) {}              // a burning wreck (0x472630)
    virtual void explosion_smoke(const Vec3i&) {}         // smoke after an explosion above the sea (0x472630)
    // Construction (sim/construction.h): victory bookkeeping of a unit changing hands, the unit
    // script call notice with arguments (0x456290), and user interface refreshes.
    virtual void victory_unit_transferred(Unit&) {}
    virtual void broadcast_script_call_args(Unit&, std::int16_t, std::int32_t, std::int32_t) {}
    virtual void builder_menu(Unit&) {}
    virtual void build_menu_refresh(Unit&) {}
    virtual void orders_panel_changed() {}
    virtual void selection_changed() {}
    // Aircraft, transports and teleporters (sim/unit_missions.h): the network notice of a unit
    // script function started by name with arguments (0x456200; -1 when the script has none),
    // and the trail of dots of a teleported unit (presentation; its particles draw the C
    // runtime rand() in the effect sweep, which is not modelled).
    virtual void broadcast_script_call_named(Unit&, std::int16_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                                             std::int32_t) {}
    virtual void teleport_trail(const std::array<std::int32_t, 3>&, const std::array<std::int32_t, 3>&) {}
    // Battle::step(): stage 2 (a network game's incoming packets, 0x453D40) and stage 10 (its
    // outgoing flush); the per-frame work after a frame's steps besides lingering sight.
    virtual void network_input() {}
    virtual void network_flush() {}
    // Battle start (sim/battle_start.h): a player record with a controller is set up
    // (0x464700, in slot order): after its economy and sight counts, a record that is not a
    // watched one (active with controller 3) gets its computer-player state and unit table
    // (the original draws the battle generator 8 times there, 0x40A150). Then, after the
    // commanders, the computer players' metal-site survey (0x4649D0).
    virtual void ai_player_setup(int /*player*/) {}
    virtual void ai_battle_ready() {}
    // Stale stack words the original reads (inputs; the defaults are the settings' values):
    // the computer player's constructor site height (per role call) and the reclaim sites' y.
    virtual std::int32_t ai_site_height_word(int /*player*/, std::int32_t fallback) { return fallback; }
    virtual std::int32_t reclaim_site_y(std::int32_t fallback) { return fallback; }
    // Missions (sim/mission_goals.h): a goal was met for the first time (the original plays
    // its "goal met" sound; presentation only).
    virtual void mission_goal_met() {}
};

class Battle final : UnitWorld,
                     FeatureSystems,
                     EffectsSystems,
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
    AnimLibrary anims;                // animation sequences of features and effects
    FeatureState feature_state;       // feature animation slots (sim/features.h)
    EffectsState effects;             // world effects, shards, debris (sim/effects.h)
    NanoSprayState nano;              // nano lathe particle streams (sim/nano_spray.h)
    EffectLists fx_lists;             // the other particle emitters of the effect sweep (sim/effect_lists.h)
    EffectsEnvironment effect_art;    // effect sequences (the rest is synced from the settings)
    Lcg15 crt_rand;                   // the C runtime rand() stream of the simulation thread
    ProjectileTable projectiles;
    MapGrid map;
    UnitBuckets buckets;  // sized by init_buckets() once the map is set
    BattleSettings settings;
    // Movement classes and their passability grids, the route search (sim/route_search.h).
    std::array<MoveClass, kMoveClasses> move_classes{};
    std::vector<std::uint8_t> feature_blocks;  // by feature type: nonzero when it blocks movement
    std::vector<std::uint16_t> explored;       // per 2 x 2 cells: bit p = player p has seen it
    RouteSearch route_search;
    // Resources (sim/economy.h): each player record's economy besides its stored amounts
    // (PlayerSlot::energy_stored / metal_stored), the wind, and the victory state the
    // settlement checks.
    std::vector<PlayerEconomy> player_economy;
    WindState wind;
    EndGameState end_game;
    VictoryState victory;  // outcome per player and the end of the game (sim/victory.h)
    MeteorShower meteors;  // meteor showers (sim/sky.h), configured by the loader
    CameraShake shake;     // the camera shake, whose decay draws rand() (sim/sky.h)
    MissionGoals mission_goals;  // a mission's win and loss goals (sim/mission_goals.h; provider 1)
    void outside_cue_goal_met() { outside_.mission_goal_met(); }

    // Setup: `count` unit table entries (ids 0..count-1; entry 0 unused), and a player's
    // inclusive range of them (the entries' owner is fixed by it).
    void init_unit_table(std::size_t count);
    void assign_unit_range(int player, int first, int last);
    // After the map's heights and height ranges (and the sea level) are set.
    void init_buckets() {
        buckets.resize_for_map(map.width, map.height);
        build_bucket_heights(buckets, map, settings.sea_level);
    }
    // Once the map (heights, features) and the unit ranges are set, before units are made:
    // builds the movement classes' grids and sets up the route search (the original does
    // this while loading the battle). An `explored` map of the wrong size becomes all seen.
    void init_routes();

    // Creates a unit of type `type` for `player` at (x, y, z) (16.16) in the first free
    // entry of the player's range, or exactly entry `wanted_id` when nonzero (the original's
    // Unit_CreateFromDefId 0x485F50). `layer` gives the low two unit flags (1 = ground).
    // Unfinished units start with no hit points and the whole build fraction left.
    // Returns nullptr when the type cannot be made, the player's limit for it is reached or
    // there is no free entry.
    Unit* create_unit(int player, std::uint16_t type, std::int32_t x, std::int32_t y, std::int32_t z, bool fully_built,
                      std::uint32_t layer, std::uint16_t wanted_id = 0);

    // Battle start: the players' weapon-target controllers and unit tables (after the player
    // slots are set up).
    void start_players();
    // The computer player (sim/ai.h). start_players() also makes every player's controller,
    // roles and table (the original's player set-up, with eight random draws per table);
    // the profile is applied through ai() afterwards and the map's metal spots are collected
    // once the map features are placed (the original does it after the first players' stage).
    AiState ai_state;
    Ai& ai();
    void collect_ai_metal_sites();
    // The players' stage (stage 6 of a step): for every playing player its weapon-target pass,
    // its unit table refresh (every 30 steps) and its units' sight; the rest is outside.
    void players_stage();
    Targeting& targeting() { return targeting_; }
    // Economy setup for a player at battle start (after its slot's state is set): everything
    // zero, its first 30-step turn at the current step (0x464700's economy part).
    void init_player_economy(int player);
    // Battle start: the whole player set-up of one record (economy, then the computer-player
    // controller and table: eight battle generator draws; none for a watched record), and the
    // end of the set-up (the computer players' metal-site survey).
    void setup_player(int player);
    void battle_ready();
    // The economy context (construction, reclaim, transfers and tests use it with sim/economy.h).
    Economy& economy() {
        sync_settings();
        return economy_;
    }
    // Stage 8 (after the features; the animation cursors between them are presentation):
    // the wind may change (0x490C40). The new drift reaches the projectiles and fires through
    // settings.wind_drift, wind generators through the next settlement, and their scripts
    // through the next unit pass.
    void step_wind();
    // Network games, at the end of a step: the local player's automatic sharing (0x457D30).
    void auto_share_resources();

    // Line of sight (sim/sight.h). Until init_sight() runs, sight goes to BattleOutside
    // (update_sight, unit_visible, position_explored, stamp_line_of_sight, reveal_dead_unit,
    // forget_fog) as before. Order: map heights, sea level and player slots set ->
    // init_sight() -> init_routes() -> units created -> rebuild_sight(true) (the battle start).
    SightTables sight_tables;  // masks and rings (filled by the importer)
    SightState sight;          // sight counts, explored map size, height band, lingering sight
    void init_sight();
    void rebuild_sight(bool clear_explored);
    // Once per drawn frame, after that frame's steps (even none): lingering sight of dead
    // units expires (the original runs it per frame, so it depends on the frame rate).
    void expire_lingering_sight();
    // Whether player record `viewer` sees the unit (line of sight test), and the block test at
    // a point (shots are drawn where the viewed player sees).
    bool unit_seen_by(int viewer, const Unit& unit);
    bool point_seen_by(int viewer, const std::array<std::int32_t, 3>& pos);
    std::uint32_t sight_faults() const { return sight_world_.faults; }

    // Per-step inputs the original reads from its game state.
    UnitTickEnvironment& unit_env() { return unit_env_; }

    std::uint32_t game_tick() const { return unit_env_.game_tick; }
    BattleRng& battle_rng() { return rng_; }                 // the battle random generator
    const HeightMap& height_map() const { return heights_; }  // the terrain heights

    // The unit pass and then the projectile update of one step (the original's
    // stages 2 and 3; the caller advances the tick first and runs the other stages).
    void step_units_and_projectiles();
    // Stage 4, right after the projectiles: debris pieces and world effects.
    void step_effects();
    // Stage 6, after the players' stage: feature animations, reproduction, fires, falling wrecks.
    void step_features();
    // Stage 9: the effect sweep's nano streams (their emissions draw the C runtime rand()).
    void step_nano();
    // Command input (sim/construction.h): a factory's (or mobile builder's) build queue of `type`
    // grows or shrinks by `delta`; a mobile builder is ordered to build `type` at `pos`.
    void order_build_count(Unit& unit, std::uint16_t type, std::int32_t delta);
    bool order_mobile_build(Unit& builder, std::uint16_t type, const std::array<std::int32_t, 3>& pos, bool queued);
    // Any order (the original's general order routine, see enqueue_order in sim/missions.h).
    bool order(Unit& unit, std::uint8_t kind, bool queued, Unit* target, const std::array<std::int32_t, 3>* pos,
               std::int32_t param0 = 0, std::int32_t param1 = 0);
    // Transport attach / detach as sent and applied (packet 0x0A, 0x48AAC0).
    void transport_attach(Unit& cargo, Unit* transport, std::uint8_t piece, std::uint8_t layer);
    Construction& construction() { return construction_; }
    Missions& missions() { return missions_; }
    // Map setup: all feature slots free, no effects; then the type-wide feature animations.
    // Call once the feature types and animations are set (before placing map features).
    void reset_features_and_effects();
    void advance_tick() { ++unit_env_.game_tick; }
    // One whole step in the original's order (Game_RunSimulationStep 0x495490, see
    // docs/RE_MEMORY.md "Battle step"): tick, network input, units and projectiles, effects,
    // players, features, wind, meteors, the effect sweep, network flush. Commands
    // (sim/commands.h) are applied between steps. After the frame's steps (even none) the
    // frame loop calls end_frame().
    void step();
    void end_frame();
    // Stage 8's meteors (sim/sky.h): a scheduled shower, a meteor shot when one drops.
    void step_meteor_shower();
    // Victory (sim/victory.h, engine/sim/src/victory.cpp): the local player's checks in its turn
    // of the players' stage, the network end check after the stage's loop, and a commander's
    // death destroying its player's units (0x486F10).
    void victory_turn(int slot);
    void victory_stage_end();
    // `stale_word`: the kill routine's wreck word for the units that go through it (owners that
    // do not play): what the caller's stack holds there (a network player leaving, 0x452CC0:
    // an input); each such kill leaves its final wreck level in it, the detonation leaves it.
    void kill_team(int slot, std::uint32_t& stale_word);
    // Commander death: the player plays, so its units only take the damage path (no kill
    // routine call, the word is never read).
    void kill_team(int slot);
    void deathmatch_respawn(int slot);
    // The route search's step: the first thing the players stage (0x464F80) does, after
    // the effects stage and before the players' own updates.
    void run_route_search();
    // Cells of a footprint changed (a feature placed or removed): every movement class
    // recomputes them (0x440A40).
    void refresh_passability(std::int16_t x, std::int16_t z, std::int16_t w, std::int16_t h);

    // Contexts for direct calls (tests, other stages).
    Combat& combat() { return combat_; }
    Weapons& weapon_context() { return weapons_; }
    ProjectileWorld& projectile_world() { return projectile_world_; }
    OrderContext& order_context() { return orders_; }
    FootprintWorld& footprint_world() { return footprint_; }
    RouteContext& route_context() { return route_; }
    // The first fault any system recorded (where the original would crash, hang or read
    // garbage), prefixed with the system's name; empty when there is none.
    std::string first_fault();
    UnitWorld& unit_world() { return *this; }  // for unit_get_value / unit_set_value / unit_set_activate
    FeatureWorld& feature_world() {
        sync_settings();
        return feature_world_;
    }
    EffectsWorld& effects_world() {
        sync_settings();
        return effects_world_;
    }

    // Network play (sim/net_units.h, engine/sim/src/net_units.cpp): received packets that
    // change units, and the unit state packets. `slot` is the sending player's slot.
    Unit* apply_unit_packet(int slot, const std::uint8_t* packet);                       // 0x09 (0x4861D0)
    std::vector<std::uint8_t> unit_state_packet(int slot);                              // 0x2C (0x48B710)
    std::uint32_t apply_unit_state_packet(int slot, const std::uint8_t* packet, std::size_t size);  // 0x2C (0x48B920): its step
    void apply_transport_packet(const std::uint8_t* packet);                            // 0x0A (0x48AB70)
    void apply_fire_packet(const std::uint8_t* packet);                                 // 0x0D (0x49D270)
    void apply_script_packet(const std::uint8_t* packet);                               // 0x10
    void apply_activate_packet(const std::uint8_t* packet);                             // 0x11
    void apply_finished_packet(const std::uint8_t* packet);                             // 0x12 (0x41B8D0)
    void apply_ownership_packet(int new_owner, const std::uint8_t* packet);             // 0x14 (0x488570)
    void share_explored_map(int from, int to);                                          // 0x16 kind 3
    NetUnitInputs net_inputs;
    NetUnitState& net_state() { return *net_; }

private:
    void sync_settings();
    void prune_retired_goals();
    FxContext fx_context() {
        return FxContext{fx_lists, crt_rand, unit_env_.game_tick, static_cast<std::int32_t>(nano.streams.size()), settings.sea_level, &heights_,
                         wind.velocity[0], wind.velocity[2], settings.gravity};
    }  // order goals the route search no longer reads (MotionContext::retired_goals)
    // The shared effect-object pool (0x51E610: 1000 objects for the effect lists and the nano
    // streams) has an object left and is not blocked: a nano stream spawn needs one.
    bool effect_pool_free() const {
        return !fx_lists.blocked && fx_lists.live() + static_cast<int>(nano.streams.size()) < EffectLists::kPool;
    }
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
    void emit_sfx(Unit& u, std::int32_t p, std::int32_t t) override;
    void explode_piece(Unit& u, std::int32_t p, std::int32_t t) override {
        sim::explode_piece(effects_world_, u, p, static_cast<std::uint32_t>(t));
    }
    void attach_unit(Unit& u, std::int32_t a, std::int32_t p, std::int32_t m) override;
    void drop_unit(Unit& u, std::int32_t a) override;

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
    void retarget(Unit* a, Unit& v, std::uint16_t) override { retarget_attack(b_targeting(), a, v); }
    void add_paralysis(Unit& victim, std::uint16_t amount) override;
    bool pay_energy(Unit& w, float amount) override { return add_energy_use_if_not_stalled(ticks[w.id], amount); }  // 0x401180
    void stats_activity(std::int32_t kind) override { outside_.stats_activity(kind); }
    void network_send(std::int32_t net, const std::uint8_t* p, std::int32_t size) override { outside_.network_send(net, p, size); }
    void reveal_dead_unit(Unit& u) override;
    void victory_unit_destroyed(Unit& u) override {
        mission_unit_destroyed(*this, u);  // 0x4904C0
        outside_.victory_unit_destroyed(u);
    }
    void clear_orders(Unit& unit) override;
    void forget_speech(Unit& u) override { outside_.forget_speech(u); }
    void leave_group(Unit& u) override {
        sim::set_unit_group(ai_, u, -1);  // 0x480250(unit, -1)
        outside_.leave_group(u);
    }
    void remove_projectiles(Unit& unit) override;
    void detach_from_carrier(Unit& u) override { transport_attach(u, nullptr, 0xFF, 1); }
    void clear_footprint(Unit& u) override { sim::clear_footprint(footprint_, u); }
    void forget_fog(Unit& u) override;
    void kill_counted(std::uint8_t slot, Unit& v) override { outside_.kill_counted(slot, v); }
    void detonate(Unit& unit, bool self_destruct) override;
    void spawn_wreck(Unit& u, std::int32_t level, bool burn) override;
    void release_unit(Unit& u) override {
        release_referrers(u);  // 0x489740
        outside_.release_unit(u);
    }
    void release_motion(Unit& u) override { destroy_motion(motion_, u); }
    void player_out_of_units(int p) override {
        // The message that a player is out of units picks its wording with one C runtime
        // rand() (skirmish 0x47BD70, "obliterated"; network game 0x450380, "has left", while
        // this machine's player is present — always, here). The message itself is presentation.
        if (settings.provider == 2 || settings.provider == 3) crt_rand.next();
        outside_.player_out_of_units(p);
    }
    void commander_died(int p) override {
        outside_.commander_died(p);  // the user interface part (0x491D70)
        kill_team(p);
    }

    // WeaponSystems
    std::int32_t fire(Unit& unit, int slot, Unit* target, WorldPos& target_pos) override;
    void stockpile_used(Unit& u) override { outside_.stockpile_used(u); }

    // ProjectileSystems
    const WeaponType* slot_weapon(const Unit& u, int slot) override { return u.weapons[static_cast<std::size_t>(slot)].type; }
    std::uint16_t slot_fire_heading(const Unit& u, int slot) override { return u.weapons[static_cast<std::size_t>(slot)].aim_heading; }
    std::int32_t motion_speed(const Unit& u) override { return sim::motion_speed(ticks[u.id]); }
    void play_sound(std::uint16_t s, const Vec3i& p) override { outside_.play_sound(s, p); }
    void spawn_smoke(const Vec3i& p) override {
        FxContext c = fx_context();
        fx_smoke(c, p, 9);
        outside_.spawn_smoke(p);
    }
    void spawn_muzzle_smoke(const Vec3i& p) override {
        FxContext c = fx_context();
        fx_muzzle_smoke(c, p, 9);
        outside_.spawn_muzzle_smoke(p);
    }
    void spawn_explosion(const Vec3i& p, const WeaponType& w, bool wa, bool iw) override { spawn_impact(effects_world_, p, w, wa, iw); }
    void camera_shake(std::int32_t x, std::int32_t y, std::int32_t d) override {
        add_camera_shake(shake, x, y, d, settings.shake_disabled);
        outside_.camera_shake(x, y, d);
    }
    void damage_feature(MapCell& c, std::int32_t x, std::int32_t z, const WeaponType& w) override {
        sim::damage_feature(feature_world_, c, x, z, w);
    }
    Vec3i feature_anim_pos(std::uint16_t slot) override { return feature_slot_pos(feature_state, slot); }

    // FeatureSystems (update_passability is shared with the footprints)
    std::uint32_t create_object(std::uint32_t) override { return next_object_ += 0x40; }
    void free_object(std::uint32_t) override {}
    void area_damage(const WeaponType& w, const Vec3i& at) override { area_damage_at(projectile_world_, w, at); }
    void feature_packet(const std::uint8_t* p, bool to_host) override { outside_.feature_packet(p, to_host); }
    void feature_smoke(const Vec3i& p) override {
        FxContext c = fx_context();
        fx_smoke(c, p, 5);
        outside_.feature_smoke(p);
    }
    void feature_steam(const Vec3i& p) override {
        FxContext c = fx_context();
        fx_steam(c, p, 4);
        outside_.feature_steam(p);
    }
    void feature_fire_sound(std::int16_t x, std::int16_t z) override { outside_.feature_fire_sound(x, z); }
    void wreck_fire(const Vec3i& p) override {
        FxContext c = fx_context();
        fx_smoke_emitter(c, p, 15, 900, 9);
        outside_.wreck_fire(p);
    }

    // EffectsSystems
    bool unit_velocity(const Unit& u, Vec3i& v) override {
        if (u.id >= ticks.size() || !ticks[u.id].has_motion) return false;
        v = ticks[u.id].velocity;
        return true;
    }
    int unit_owner(const Unit& u) override { return u.id < ticks.size() ? ticks[u.id].owner : 0; }
    void explosion_smoke(const Vec3i& p) override {
        FxContext c = fx_context();
        fx_smoke_emitter(c, p, 7, 15, 9);
        outside_.explosion_smoke(p);
    }

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
        sim::update_passability(route_, x, z, w, h);
    }
    void passability_after_release(Unit& u) override { sim::passability_after_release(route_, u); }
    void motion_cells_claimed(Unit& u) override;

    // GroundSettleSystems
    std::int32_t motion_speed(Unit& u) override { return sim::motion_speed(ticks[u.id]); }
    std::uint32_t motion_start_tick(Unit& u) override { return motion_move_tick(ticks[u.id]); }

    // Motion and mission hooks (separate objects: some names clash with the combat hooks).
    struct MotionHooks final : MotionSystems {
        Battle& b;
        explicit MotionHooks(Battle& battle) : b(battle) {}
        bool can_place(Unit& u, std::int16_t x, std::int16_t z, std::uint32_t layer) override {  // 0x47DB70
            return can_place_footprint(b.route_, b.ticks[u.id].type_index, u.id, x, z, layer);
        }
        void clear_footprint(Unit& u) override { release_cells(b.footprint_, u); }  // 0x47D0E0
        void claim_footprint(Unit& u) override { sim::claim_footprint(b.footprint_, u); }
        void update_sight(Unit& u) override { b.update_sight(u); }
        bool off_map(const Unit& u) override { return u.bucket == kOffMapBucket; }
        std::uint8_t bucket_height(const Unit& u) override {  // the unit's bucket record, second byte
            if (u.bucket == kOffMapBucket) return 0;
            if (u.bucket < 0 || static_cast<std::size_t>(u.bucket) >= b.buckets.top.size()) {
                if (b.motion_.fault.empty()) b.motion_.fault = "cruise height of a unit without a spatial bucket (the original reads a null record)";
                return 0;
            }
            return b.buckets.top[static_cast<std::size_t>(u.bucket)];
        }
        void route_search_forget(MotionGoal& g) override { sim::route_search_forget(b.route_search, g); }
        void set_activated(Unit& u, bool on) override { unit_set_activate(u, b, unit_flags::kActivated, on); }
        // Goal objects need a nonzero handle: an order with handle 0 counts as having no goal
        // and would never detach it from the motion object (stand-in addresses, as for orders).
        std::uint32_t allocate_object(std::uint32_t) override { return b.next_goal_address_ += 0x40; }
        void free_object(std::uint32_t) override {}
    };
    struct MissionHooks final : MissionSystems {
        Battle& b;
        explicit MissionHooks(Battle& battle) : b(battle) {}
        std::int32_t run_other(Unit& u, MissionOrder& o, std::uint32_t& event) override { return b.outside_.run_mission(u, o, event); }
        void speech(Unit& u, std::uint32_t id, SpeechText t) override { b.outside_.speech(u, id, t); }
        void enemies_in_radius(int p, const std::array<std::int32_t, 3>& pos, std::int32_t r, std::vector<Unit*>& out) override {
            sim::enemies_in_radius(b.targeting_, p, pos, r, out);
        }
        Unit* fire_at_will_target(Unit& u) override { return sim::fire_at_will_target(b.targeting_, u); }
        bool issue_attack_order(Unit& u, Unit& t, bool forced) override { return sim::issue_attack_order(b.targeting_, u, t, forced); }
        std::uint8_t resolve_mission(OrderIntent i, Unit& u, Unit* t, const std::array<std::int32_t, 3>* pos) override {
            return sim::resolve_mission(b.targeting_, static_cast<std::uint8_t>(i), u, t, pos);
        }
        void unfinished_decay(Unit& u, std::int32_t ticks) override { sim::unfinished_decay(b.missions_, u, ticks); }
        void display_changed(Unit& u) override { b.outside_.display_changed(u); }
        bool use_resources_if_not_stalled(Unit& u, float e, float m) override { return add_use_if_not_stalled(b.ticks[u.id], e, m); }
        void build_counts_changed(Unit& u) override { b.outside_.build_counts_changed(u); }
        void nano_beam(const std::array<std::int32_t, 3>& from, const std::array<std::int32_t, 6>& box) override {
            if (b.effect_pool_free()) spawn_nano_stream(b.nano, b.crt_rand, b.unit_env_.game_tick, point_box(from), box);
            b.outside_.nano_beam(from, box);
        }
        void drop_goal_object(Unit& u, MissionOrder& o) override { release_order_goal(b.motion_, u, o); }
        void attach_circle_goal(Unit&, MissionOrder& o, const std::array<std::int32_t, 3>& pos, std::int32_t radius) override {
            sim::attach_circle_goal(b.motion_, o, pos, radius);
        }
    };
    struct RouteHooks final : RouteSystems {
        Battle& b;
        explicit RouteHooks(Battle& battle) : b(battle) {}
        bool structure_fits(std::uint16_t type, std::int16_t x, std::int16_t z) override { return sim::structure_fits(b.construction_, type, x, z); }
    };
    struct TargetingHooks final : TargetingSystems {
        Battle& b;
        explicit TargetingHooks(Battle& battle) : b(battle) {}
        bool unit_visible(int p, Unit& u) override;
        bool position_explored(int p, const std::array<std::int32_t, 3>& pos) override;
        // A feature type's flag byte (its low byte: 0x40 blocking, 0x80 reclaimable).
        std::uint8_t feature_flags(std::uint16_t f) override {
            return f < b.features.size() ? static_cast<std::uint8_t>(b.features[f].flags) : b.outside_.feature_flags(f);
        }
        std::int32_t intercept_target(Unit& u, int slot, std::array<std::int32_t, 3>& pos) override;
        // (the refresh's start brings the AI context up to date for the rest of it)
        void ai_table_begin(int table) override { sim::ai_table_begin(b.ai(), table); }
        void ai_own_unit(int table, Unit& u) override { sim::ai_table_own_unit(b.ai_, table, u); }
        void ai_table_end(int table) override { sim::ai_table_end(b.ai_, table); }
        void ai_rescore(int table) override { sim::ai_rescore(b.ai_, table); }
        void under_attack(Unit& u) override { b.outside_.under_attack_notice(u); }
    };
    struct EconomyHooks final : EconomySystems {
        Battle& b;
        explicit EconomyHooks(Battle& battle) : b(battle) {}
        void set_cloaked(Unit& u, bool on) override { unit_set_activate(u, b, unit_flags::kCloaked, on); }  // 0x48B090
    };
    Targeting& b_targeting() { return targeting_; }
    void update_sight(Unit& u);
    void stamp_line_of_sight(Unit& u);
    void sync_sight();
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
    FeatureEnvironment feature_env_;
    FeatureWorld feature_world_;
    EffectsWorld effects_world_;
    std::uint32_t next_object_ = 0x02000000;  // stand-in object state handles of 3D features
    std::uint32_t next_goal_address_ = 0x03000000;  // stand-in addresses of motion goal objects
    MotionHooks motion_hooks_{*this};
    MissionHooks mission_hooks_{*this};
    std::vector<MotionType> motion_types_;        // by type index (copied from types each step)
    std::vector<MissionTypeInfo> mission_types_;  // by type index
    MotionContext motion_;
    MissionEnvironment mission_env_;
    Missions missions_;
    std::vector<TargetingTypeInfo> targeting_types_;  // by type index
    TargetingEnvironment targeting_env_;
    TargetingState targeting_state_;
    TargetingHooks targeting_hooks_{*this};
    Targeting targeting_;
    RouteHooks route_hooks_{*this};
    std::vector<RouteType> route_types_;  // by type index
    RouteContext route_;
    EconomyHooks economy_hooks_{*this};
    std::vector<EconomyType> economy_types_;  // by type index
    EconomyEnvironment economy_env_;
    Economy economy_;
    std::vector<std::unique_ptr<UnitScriptHost>> script_hosts_;  // by unit id
    std::int16_t camera_countdown_ = 0;
    std::uint32_t next_order_address_ = 0x01000000;
    std::vector<MissionOrder*> free_orders_;  // destroyed orders, reused last first (allocate_order)
    std::vector<SightType> sight_types_;  // by type index
    SightEnvironment sight_env_;
    SightWorld sight_world_;
    bool sight_ready_ = false;
    // Construction (sim/construction.h).
    struct ConstructionHooks final : ConstructionSystems, TransportLinks {
        Battle& b;
        explicit ConstructionHooks(Battle& battle) : b(battle) {}
        Unit* create_unit(int player, std::uint16_t type, const std::array<std::int32_t, 3>& pos, bool built, std::uint32_t layer) override {
            return b.create_unit(player, type, pos[0], pos[1], pos[2], built, layer);
        }
        bool can_place(std::uint16_t type, std::uint16_t exclude, std::int16_t x, std::int16_t z, std::uint32_t mode) override {
            return can_place_footprint(b.route_, type, exclude, x, z, mode);
        }
        void transport_attach(Unit& cargo, Unit* t, std::uint8_t piece, std::uint8_t layer) override { b.transport_attach(cargo, t, piece, layer); }
        void attach_edge_goal(Unit&, MissionOrder& o, std::uint32_t cell, std::uint32_t size) override { sim::attach_edge_goal(b.motion_, o, cell, size); }
        void attach_ring_goal(Unit&, MissionOrder& o, const std::array<std::int32_t, 3>& pos, std::int32_t outer, std::int32_t inner) override {
            sim::attach_ring_goal(b.motion_, o, pos, outer, inner);
        }
        void nano_beam_reverse(const std::array<std::int32_t, 6>& box, const std::array<std::int32_t, 3>& to) override {
            if (b.effect_pool_free()) spawn_nano_stream(b.nano, b.crt_rand, b.unit_env_.game_tick, box, point_box(to));
        }
        void kill_feature(std::int32_t x, std::int32_t z, bool reclaimed) override { sim::kill_feature(b.feature_world_, x, z, reclaimed); }
        void victory_unit_transferred(Unit& u) override {
            mission_unit_transferred(b, u);  // 0x490520
            b.outside_.victory_unit_transferred(u);
        }
        void network_send(std::int32_t net, const std::uint8_t* p, std::int32_t size) override { b.outside_.network_send(net, p, size); }
        void broadcast_script_call_args(Unit& u, std::int16_t f, std::int32_t n, std::int32_t a) override { b.outside_.broadcast_script_call_args(u, f, n, a); }
        void builder_menu(Unit& u) override { b.outside_.builder_menu(u); }
        void build_menu_refresh(Unit& u) override { b.outside_.build_menu_refresh(u); }
        void orders_panel_changed() override { b.outside_.orders_panel_changed(); }
        std::int32_t network_provider() override { return b.settings.network_provider; }
        // TransportLinks
        void bucket_unlink(Unit& u) override { sim::bucket_unlink(b.footprint_, u); }
        void bucket_prepend(Unit& u) override { sim::bucket_prepend(b.footprint_, u); }
        std::uint32_t type_flags(const Unit& u) override {
            const UnitTickState& t = b.ticks[u.id];
            return t.tick_type ? t.tick_type->type_flags : 0;
        }
        void selection_changed() override { b.outside_.selection_changed(); }
    };
    ConstructionHooks construction_hooks_{*this};
    std::vector<ConstructionTypeInfo> construction_types_;  // by type index
    ConstructionEnvironment construction_env_;
    Construction construction_;
    // The computer player (sim/ai.h): orders, factory queues, activation, placement, sight
    // and weapon range for it.
    struct AiHooks final : AiSystems {
        Battle& b;
        explicit AiHooks(Battle& battle) : b(battle) {}
        std::uint8_t resolve(std::uint8_t intent, Unit& u, Unit* t, const std::array<std::int32_t, 3>* pos) override {
            return sim::resolve_mission(b.targeting_, intent, u, t, pos);
        }
        void enqueue(std::uint8_t kind, bool queued, Unit& u, Unit* t, const std::array<std::int32_t, 3>* pos, std::int32_t p0,
                     std::int32_t p1) override {
            b.order(u, kind, queued, t, pos, p0, p1);
        }
        void build_menu(Unit& u, std::uint16_t type, bool weapon_menu) override;
        void set_activated(Unit& u, bool on) override { unit_set_activate(u, b, unit_flags::kActivated, on); }
        bool can_place(std::uint16_t type, std::int16_t x, std::int16_t z) override { return can_place_footprint(b.route_, type, 0, x, z, 1); }
        bool structure_fits(std::uint16_t type, std::int16_t x, std::int16_t z) override { return sim::structure_fits(b.construction_, type, x, z); }
        std::int32_t last_place_metal() override { return b.construction_env_.last_place_metal; }
        bool point_in_sight(int player, const std::array<std::int32_t, 3>& pos) override;
        bool weapon_in_range(Unit& u, const std::array<std::int32_t, 3>& from, const std::array<std::int32_t, 3>& to, int slot) override {
            return sim::weapon_in_range(b.weapons_, u, slot, from, to);
        }
        std::int32_t stale_site_height(int player, std::int32_t fallback) override { return b.outside_.ai_site_height_word(player, fallback); }
    };
    void sync_ai();
    AiHooks ai_hooks_{*this};
    std::vector<AiTypeInfo> ai_types_;  // by type index (taken at start_players and when the type table grows)
    AiEnvironment ai_env_;
    Ai ai_;
    // Aircraft, transports and the other movement orders (sim/unit_missions.h).
    struct UnitMissionHooks final : UnitMissionSystems {
        Battle& b;
        explicit UnitMissionHooks(Battle& battle) : b(battle) {}
        void heap_alloc(std::uint32_t) override {}
        std::shared_ptr<OrderGoal> make_goal(Unit& u, MissionOrder& o, const GoalRequest& g) override;
        void attach_goal(Unit&, MissionOrder& o, std::shared_ptr<OrderGoal> goal) override { attach_order_goal(b.motion_, o, std::move(goal)); }
        void adjust_goal(Unit& u, OrderGoal& goal, GoalRequest::Set what, std::int32_t value) override;
        void set_layer(Unit& u, std::uint32_t layer) override { set_motion_layer(b.motion_, u, layer); }
        void units_in_radius(const std::array<std::int32_t, 3>& pos, std::int32_t r, std::vector<Unit*>& out) override {
            sim::units_in_radius(b.buckets, pos, r, out);
        }
        void air_bases_in_radius(int state, const std::array<std::int32_t, 3>& pos, std::int32_t r, std::vector<Unit*>& out) override;
        void broadcast_script_call(Unit& u, const char* name, std::int32_t argc, std::int32_t a, std::int32_t c1, std::int32_t c2,
                                   std::int32_t c3) override;
        std::int32_t reclaim_site_y() override { return b.outside_.reclaim_site_y(b.settings.reclaim_site_y); }
        void place_unit(Unit& u, const std::array<std::int32_t, 3>& pos, std::uint32_t layer) override { sim::place_unit(b.motion_, u, pos, layer); }
        void teleport_trail(Unit& u, const std::array<std::int32_t, 3>& to) override {
            // Flame dots from where the unit stands to where it goes (0x471FD0: 30 steps, effect
            // list 5); their particles draw the C runtime rand() in the effect sweep.
            FxContext c = b.fx_context();
            fx_dots(c, u.pos, to, 30, 5);
            b.outside_.teleport_trail(u.pos, to);
        }
        void remove_feature(std::int32_t cell) override { sim::remove_feature(b.feature_world_, cell, false); }
        void wreck_attitude(const MapCell& origin, Unit& u) override;
    };
    std::shared_ptr<NetUnitState> net_ = std::make_shared<NetUnitState>();
    void apply_full_state(BitReader& r, Unit& u);
    void write_full_state(BitWriter& w, const Unit& u) const;
    UnitMissionHooks unit_mission_hooks_{*this};
    std::vector<UnitMissionTypeInfo> unit_mission_types_;  // by type index
    std::vector<UnitTickTypeInfo> tick_types_;             // by type index (copies, for lookups by index)
    UnitMissionEnvironment unit_mission_env_;
    UnitMissions unit_missions_;
};

} // namespace sim
