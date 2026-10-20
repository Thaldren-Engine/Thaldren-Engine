#pragma once
// The computer player (see docs/RE_MEMORY.md, "Computer player").
//
// A computer player sorts its units into numbered groups (the same ten groups a
// human player uses for control groups) and gives each group a role that acts on
// a timer:
//   1 factories and other unarmed structures: idle factories start a unit chosen
//     by weighted chance; metal makers are switched by the energy balance;
//   2/6 strike groups (land / sea): gather at the base until big enough, then go
//     for the nearest enemy unit; units far from the group's centre are handed to
//     the reserve, reserve units near it join;
//   3/7 reserves (land / sea): follow their strike group;
//   4 constructors: pick something to build, look for a site near the base centre
//     (metal spots for extractors) and patrol around the base;
//   5 armed structures: nothing;
//   8 aircraft: scout around the base, or fly to the map edges once numerous;
//   9 a "hunter" that walks a probe over the map looking for enemy strength (no
//     unit is ever put in its group, but its timer still draws random numbers).
// What to build comes from per-type scores (cost, production, weapons) scaled by a
// profile of weights and limits per difficulty, and the player's current economy.
//
// The per-player table that also holds the enemy lists (sim/targeting.h) carries
// the computer player's counts and scores; its refresh every 30 steps updates them.
//
// Orders go through the general order routine (sim/missions.h enqueue_order) after
// the per-unit mission resolution (sim/targeting.h resolve_mission), and factory
// queues through the build menu command; AiSystems carries them so the module can be
// driven by the battle or by a test.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/economy.h"
#include "sim/projectile.h"
#include "sim/rng.h"
#include "sim/targeting.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"
#include "sim/weapon_slot.h"

namespace sim {

// Unit type data the computer player reads (index = type index).
struct AiTypeInfo {
    std::uint32_t type_flags = 0;  // the type flag word (builder 0x40, air base 0x200, flier 0x800, ...)
    std::uint32_t abilities = 0;   // the "can do" word (attack 0x10, capture 0x1000, ...)
    // The three weapon definitions (null: the definition's missing weapon; the original would crash).
    std::array<const WeaponType*, 3> weapons{};
    float build_cost_energy = 0.0f, build_cost_metal = 0.0f;
    float energy_make = 0.0f, energy_use = 0.0f, extracts_metal = 0.0f;
    float wind_generator = 0.0f, tidal_generator = 0.0f;
    std::uint8_t makes_metal = 0;
    bool structure = false;            // a structure's build code (weighs in the base centre)
    std::int16_t radar_distance = 0, sonar_distance = 0;
    std::int16_t min_water_depth = 0;  // >= 0: a water unit
    std::int16_t footprint_x = 0, footprint_z = 0;
    bool has_build_options = false;          // a build list exists (it may be empty)
    std::vector<std::uint16_t> build_options;  // in menu order
    std::string side;                        // compared byte for byte with the builder's
    bool weapon_menu_name = false;           // its name names a weapon-build button (the stockpile route)
};

// What the battle (or a test) does for the computer player.
class AiSystems {
public:
    virtual ~AiSystems() = default;
    // The mission kind an intent means for the unit (sim/targeting.h resolve_mission).
    virtual std::uint8_t resolve(std::uint8_t intent, Unit& unit, Unit* target, const std::array<std::int32_t, 3>* pos) = 0;
    // The general order routine (sim/missions.h enqueue_order).
    virtual void enqueue(std::uint8_t kind, bool queued, Unit& unit, Unit* target, const std::array<std::int32_t, 3>* pos,
                         std::int32_t param0, std::int32_t param1) = 0;
    // The build menu command for a factory: its queue of `type` grows by one (`weapon_menu`:
    // the stockpile button route instead).
    virtual void build_menu(Unit& unit, std::uint16_t type, bool weapon_menu) = 0;
    // Switches the unit's activation (its scripts run).
    virtual void set_activated(Unit& unit, bool on) = 0;
    // Placement checks: the general one for a site corner (mode 1, nothing excluded) and the
    // structure yard check (no view), and the metal the last structure check summed.
    virtual bool can_place(std::uint16_t type, std::int16_t cell_x, std::int16_t cell_z) = 0;
    virtual bool structure_fits(std::uint16_t type, std::int16_t cell_x, std::int16_t cell_z) = 0;
    virtual std::int32_t last_place_metal() = 0;
    // Whether `player` sees the point (sight counts with fog, else this machine's explored map).
    virtual bool point_in_sight(int player, const std::array<std::int32_t, 3>& pos) = 0;
    // Whether the unit's weapon `slot` reaches `target` from `from` (sim/weapons.h weapon_in_range).
    virtual bool weapon_in_range(Unit& unit, const std::array<std::int32_t, 3>& from, const std::array<std::int32_t, 3>& target,
                                 int slot) = 0;
    // The constructors' site height for `player`'s role (a stale stack word, an input; see
    // AiEnvironment::stale_site_height, which is the default).
    virtual std::int32_t stale_site_height(int /*player*/, std::int32_t fallback) { return fallback; }
};

struct AiEnvironment {
    std::uint32_t game_tick = 0;
    std::int32_t difficulty = 0;               // 0 easy, 1 medium, 2 hard (profile "plan" lines)
    std::int32_t map_width = 0, map_height = 0;  // cells
    // The view's scroll extent in world units (GS+0x1422B / +0x1422F: cells x 16 less 32 and 128
    // unless "+edge" changed the margins); constructors that capture limit their sites by it.
    std::int32_t scroll_width = 0, scroll_height = 0;
    std::int32_t surface_metal = 0;            // the map's surface metal value
    std::int32_t provider = 0;                 // the map info's kind of game (1: downloadable units are never built)
    std::uint16_t units_per_player = 0;
    float wind_fraction = 0.0f;                // the wind's current fraction of full output
    std::int32_t wind_max_speed = 0, wind_denominator = 5000;
    float tidal_strength = 0.0f;
    // The constructors' site height: the original never writes the height of the site it
    // found and leaves whatever its stack held there (often a heap address), so it is an
    // input. It reaches the build order's position and, for capture-able builders, the base
    // centre height their patrols use.
    std::int32_t stale_site_height = 0;
};

// The computer player's part of a player's unit table (index as TargetingState::tables).
struct AiMetalSite {
    std::int16_t cell_x = 0, cell_z = 0;
    float score = 0.0f;  // the feature's metal (a search candidate: the negated distance)
};
struct AiTable {
    bool exists = false;
    int player = 0;                          // the player record it was made for
    std::vector<std::int16_t> counts;        // finished own units per type (last refresh)
    std::vector<std::int8_t> threat;         // per type: weapons and cost, -100..100
    std::vector<std::array<std::int8_t, 3>> build_scores;  // per type: general, metal, energy
    std::vector<std::int8_t> anchor_weight;  // per type: weight in the base centre
    std::vector<std::uint8_t> weight;        // per type: profile weight, percent (1..100)
    std::vector<std::int32_t> weight_fixed;  // per type: set by a unit-name line (later lines skip it)
    std::vector<std::int32_t> limit;         // per type: most units built (-1 none)
    std::vector<std::int32_t> limit_fixed;
    std::int32_t builders = 0;               // own finished units with a build list
    std::vector<Unit*> air_bases;            // own finished, activated builder air bases
    std::array<std::int32_t, 3> anchor{};    // the base centre (16.16)
    std::int16_t centre_cell_x = 0, centre_cell_z = 0;  // the map's centre cell (never read)
    std::vector<AiMetalSite> metal_sites;    // metal features (cell, metal)
    std::int32_t search_radius = 0;          // constructors' site search radius (world units)
    // Random site grids (land, water): cell size, offset and border, drawn when the table is made.
    std::int16_t land_w = 0, land_h = 0, land_dx = 0, land_dz = 0;
    std::int32_t land_margin = 0;
    std::int16_t water_w = 0, water_h = 0, water_dx = 0, water_dz = 0;
    std::int32_t water_margin = 0;
    // Refresh accumulators (the base centre's weighted sums).
    float sum_x = 0.0f, sum_y = 0.0f, sum_z = 0.0f, sum_w = 0.0f;
};

// A role on a timer (one per group number 1..9).
enum class AiRole : std::uint8_t { None, Factories, Strike, Reserve, Constructors, Armed, Aircraft, Hunter };
struct AiBehaviour {
    AiRole role = AiRole::None;
    std::uint32_t next_tick = 0;  // runs when not after the game tick
    std::int32_t group = 0;       // the group it acts on
    // Strike groups.
    std::int32_t min_size = 0, max_size = 0, peer = 0, peer_distance = 0;
    bool attacking = false;
    // Reserves: the strike group they follow.
    std::int32_t follows = 0;
    // The hunter: best spot, probe, probe step, best threat.
    std::array<std::int32_t, 3> best{}, probe{}, step{};
    std::int32_t best_threat = 0;
};
struct AiController {
    bool exists = false;
    int player = 0;                     // player slot
    std::uint8_t side = 0;              // the player's state index: its table and resources
    std::int32_t regroup_countdown = 30;  // steps to the next group assignment
    std::array<AiBehaviour, 10> roles{};  // by group number (0 unused)
};

// The ten groups of every player record (a unit is in at most one: Unit::group, -1 none).
using UnitGroupLists = std::array<std::array<std::vector<Unit*>, 10>, 11>;

struct AiState {
    std::array<AiController, 10> controllers{};  // by player slot
    std::array<AiTable, 10> tables{};            // by table index (as TargetingState::tables)
    UnitGroupLists groups{};                     // by player record
    // The last "plan" line matched the difficulty. A process-wide value in the original (on at
    // program start, then whatever the last profile text left), so it starts on.
    bool profile_gate = true;
};

struct Ai {
    std::vector<Unit>& units;  // index = unit id
    std::vector<UnitTickState>& ticks;
    std::vector<PlayerSlot>& players;
    std::vector<PlayerEconomy>& economy;  // by player record
    const std::vector<std::array<std::uint8_t, 16>>& allied;
    const std::vector<AiTypeInfo>& types;  // by type index
    TargetingState& targeting;              // enemy lists and the controllers' hold tick
    const MapGrid& map;
    const std::vector<FeatureTypeInfo>& features;
    BattleRng& rng;
    AiEnvironment& env;
    AiSystems& systems;
    AiState& state;
    // Set when the original would crash or read memory the model cannot know.
    std::string fault;
};

// --- Groups (0x480250): moves the unit to group `group` of its owner (-1: none). It leaves
// its old group's list (searched in its current owner's lists) by swapping in the last entry.
void set_unit_group(Ai& ai, Unit& unit, std::int32_t group);

// --- Battle start.
// A player's set-up (part of 0x464700): its groups emptied; unless it is a watched player, a
// controller with its roles and a table (eight random draws for the site grids, then the
// type scores). Call per player in slot order.
void ai_create_player(Ai& ai, int player);
// The metal features of the map (0x4649D0), for every player with a controller.
void ai_collect_metal_sites(Ai& ai);

// --- Profile commands ("plan", "weight", "limit"; the text is read by the importer).
// "plan": whether the following lines apply (any of the words names the difficulty).
void ai_profile_plan(Ai& ai, const std::vector<std::string>& words);
// Whether lines apply regardless (the unit definitions' own weight lines).
void ai_profile_force(Ai& ai);
// "weight": types in `mask` get their weight percent scaled by `factor` (clamped 0..100) on
// every player with a controller; `fixed` (a unit name) keeps later lines from changing it.
void ai_profile_weight(Ai& ai, const CategoryMask& mask, float factor, bool fixed);
// "limit": types in `mask` get the limit on every active computer player.
void ai_profile_limit(Ai& ai, const CategoryMask& mask, std::int32_t limit, bool fixed);
// The weight and limit defaults of a table (before a profile is read again).
void ai_reset_profile(Ai& ai, int table);

// --- The players' stage.
// A computer player's own work, before its weapon-target pass (0x408C40): the group
// assignment every 30 steps and every role whose timer is due.
void ai_controller_step(Ai& ai, int player);
// Unit table refresh (sim/targeting.h refresh_unit_table): before the units, for each own
// finished unit, and after them (the base centre).
void ai_table_begin(Ai& ai, int table);
void ai_table_own_unit(Ai& ai, int table, Unit& unit);
void ai_table_end(Ai& ai, int table);
// The type scores (0x409730; after a refresh, one time in 30).
void ai_rescore(Ai& ai, int table);

// --- Parts (also called directly by tests).
void ai_assign_groups(Ai& ai, int player);       // 0x408830
void ai_run_role(Ai& ai, int player, int group);  // one role's timer action
// Turns a metal maker on or off by the energy balance (0x408670; one random draw).
void ai_switch_by_economy(Ai& ai, Unit& unit);
// The type a builder should make next (0 none), by weighted chance (0x40BDB0).
std::uint16_t ai_pick_build_type(Ai& ai, int side, Unit& builder);
// The score of building `type` for the side now (0x40BB00).
std::int32_t ai_build_score(Ai& ai, int side, std::uint16_t type);
// A site for `type` near `near` (0x40BFE0): true with the footprint-centred position in `out`
// (x and z only).
bool ai_find_site(Ai& ai, int side, const std::array<std::int32_t, 3>& near, std::uint16_t type,
                  std::array<std::int32_t, 3>& out);
// The nearest live, uncloaked enemy unit of the controller's player (0x4071F0).
Unit* ai_nearest_enemy(Ai& ai, int player, std::int32_t x, std::int32_t z);
// Orders to every unit of the player in group `group` (0x480460).
void ai_order_group(Ai& ai, int player, std::int32_t group, std::uint8_t intent, bool queued, Unit* target,
                    const std::array<std::int32_t, 3>* pos, std::int32_t param0, std::int32_t param1);
// Enemy threat seen by the side within `radius` world units of `pos` (0x40B1C0).
std::int32_t ai_threat_near(Ai& ai, int side, const std::array<std::int32_t, 3>& pos, std::int32_t radius);
// The table's builder air bases within a radius (0x40B530) is sim::air_bases_in_radius
// (sim/unit_missions.h; the aircraft behaviours use it).

}  // namespace sim
