#pragma once
// Movement classes, their passability grids and the route search
// (see docs/RE_MEMORY.md, "Route search").
//
// Every movement class keeps a grid with a 2-bit grade per map cell for a
// footprint of the class's size standing with its corner on that cell:
// 0 blocked, 1 passable but costly (rough slope or the edge of an obstacle),
// 3 free. Units that have not claimed cells for a while count as obstacles;
// units that moved recently do not (they will be gone by the time the route
// is driven). The grids are built when the map is loaded and patched where
// footprints, features and unit stamps change.
//
// The route search serves one ground unit at a time. Each step it hands out
// a work budget among the playing players; a player's turn picks its next
// unit whose motion goal wants a route, then expands a best-first search over
// the cells (a few dozen per step), and finally hands the unit a route of
// corner waypoints (or none).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/motion.h"
#include "sim/projectile.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"

namespace sim {

constexpr int kMoveClasses = 32;

// One movement class (the original's 32-entry table of 0x20 bytes).
struct MoveClass {
    bool named = false;  // slot in use (the original: a name pointer)
    std::int16_t footprint_x = 0, footprint_z = 0;
    std::int16_t max_water_depth = 10000, min_water_depth = -10000;
    std::uint8_t max_slope = 0xFF, bad_slope = 0xFF, max_water_slope = 0xFF, bad_water_slope = 0xFF;
    std::int32_t width = 0, height = 0;  // grid size (the map's, once built)
    // 2 bits per cell; word (z >> 4) * width + x holds rows z & ~15 .. +15 of column x.
    std::vector<std::uint32_t> grid;
    // Units whose footprint step is older than this block the class's cells.
    // Program-wide in the original (it survives from one battle to the next).
    std::uint32_t lock_tick = 0;

    std::uint32_t grade(std::int32_t x, std::int32_t z) const {
        return (grid[static_cast<std::size_t>((z >> 4) * width + x)] >> ((z & 15) * 2)) & 3u;
    }
    void set_grade(std::int32_t x, std::int32_t z, std::uint32_t g) {
        std::uint32_t& w = grid[static_cast<std::size_t>((z >> 4) * width + x)];
        const int s = (z & 15) * 2;
        w = (w & ~(3u << s)) | (g << s);
    }
};

// Unit type data placement and routes need.
struct RouteType {
    std::int8_t move_class = -1;  // index into the class table, -1 = none
    std::int16_t footprint_x = 0, footprint_z = 0;
    std::uint8_t movement = 0;    // 0: structure (yard map placement)
    std::uint8_t max_slope = 0, max_water_slope = 0;
    std::int16_t max_water_depth = 0, min_water_depth = 0;
};

// A node of the search's open set.
struct RouteNode {
    std::int32_t heap_index = 0;  // position in the open heap; next free node when free
    std::uint32_t cell = 0;       // x in the low, z in the high half
    std::int32_t g = 0, f = 0;    // cost so far, cost plus estimate (heap key)
    std::int16_t step = 0;        // terrain cost of entering the cell (0 or 30)
    std::int16_t depth = 0;       // straight steps so far (100 at the start)
};

// The search's state (the original's single 0xC9-byte search object) and the
// program-wide counters it shares between battles.
struct RouteSearch {
    std::vector<RouteNode> pool;      // [0, pool_count) used or on the free list
    std::vector<std::int32_t> heap;   // pool indexes; [0, open_count) valid
    std::int32_t free_head = -1, pool_count = 0, open_count = 0;
    // The popped node stays at the top of the heap until the next insertion
    // reuses it or the next pop frees it.
    std::int32_t hold = 0;
    std::int32_t width = 0, height = 0, cell_count = 0;
    std::vector<std::uint8_t> cell_flags;  // bits 0-1: 0 new, 1 open, 2 closed, 3 blocked; 4 goal; 8 probe line
    std::vector<std::uint8_t> cell_dir;    // direction the cell was entered (0 = -z, 2 = -x, 4 = +z, 6 = +x)
    std::vector<std::uint16_t> cell_node;  // pool index of an open cell
    std::vector<std::uint32_t> touched;    // one bit per 8 cells (cleared at the next search)
    std::uint32_t start_cell = 0, found_cell = 0;
    std::int32_t nearest_x = 0, nearest_z = 0;  // goal cell nearest to the start
    std::int32_t probe_best = 0;          // smallest estimate on the probe line
    std::int32_t turn_range = 0;          // directions tried each side of straight ahead
    std::int32_t budget_divisor = 0x535;  // work per step, shared by the players
    std::int32_t work = 0;                // work done in this round
    std::int32_t scale = 0;               // estimate scale of this search (16.16)
    std::int32_t base_budget = 0x18000;
    Unit* active = nullptr;               // the unit being served
    RouteGoal* goal = nullptr;            // its motion goal (also left from the last ask)
    OrderGoal* order_goal = nullptr;      // where it wants to go
    int move_class = -1;
    std::array<std::uint8_t, 8> turn_cost{}, dir_cost{};
    std::uint8_t player = 0;              // whose turn it is
    std::array<std::int32_t, 10> unit_cursor{};  // unit id each player's turn looked at last
    std::array<std::int32_t, 10> credit{};       // work each player may still spend
    // Program-wide in the original (not reset between battles).
    std::int32_t refresh_counter = 0;
    std::array<std::int32_t, 10> estimate_scale{};  // per player, from its recent load
    std::array<std::int32_t, 10> load{};            // turns each player took since the refresh
};

struct RouteEnvironment {
    std::uint32_t game_tick = 0;
    std::uint8_t sea_level = 0;
    std::uint16_t player_count = 0;      // players in the game (the budget is shared by it)
    std::uint16_t units_per_player = 0;  // size of a player's part of the unit table
};

// What placement reaches outside this module.
class RouteSystems {
public:
    virtual ~RouteSystems() = default;
    // Structures are placed by their yard map (0x47D2E0, not built here).
    virtual bool structure_fits(std::uint16_t type, std::int16_t cell_x, std::int16_t cell_z) = 0;
};

struct RouteContext {
    MapGrid& map;
    std::vector<Unit>& units;           // index = unit id
    std::vector<UnitTickState>& ticks;  // index = unit id
    std::vector<PlayerSlot>& players;
    const std::vector<RouteType>& types;  // index = type index
    std::array<MoveClass, kMoveClasses>& classes;
    // Per feature type: nonzero when it blocks movement (the feature type's flag 0x40);
    // the size is the feature type count.
    const std::vector<std::uint8_t>& feature_blocks;
    // Per 2 x 2 cells (row-major, (map width / 2) per row): bit p set when player p
    // has seen that part of the map. Unseen terrain counts as passable for the player.
    const std::vector<std::uint16_t>& explored;
    RouteSearch& search;
    MotionContext& motion;  // the motion goals the search serves
    RouteSystems& systems;
    RouteEnvironment env;
    // Set when the original would have crashed or read outside its tables.
    std::string fault;
};

// Builds every named class's grid for the map (0x440940; at map load).
void build_passability(RouteContext& c);
// Every named class recomputes the cells a footprint of (w, h) at (x, z) can
// affect (0x440A40; a footprint was claimed, a yard opened, a feature placed).
void update_passability(RouteContext& c, std::int16_t x, std::int16_t z, std::int16_t w, std::int16_t h);
// One class (0x440830).
void update_class_passability(RouteContext& c, MoveClass& m, std::int16_t x, std::int16_t z, std::int16_t w, std::int16_t h);
// After a unit released its cells (0x440A70): with a motion object its footprint
// step becomes the game tick and the classes that counted it as an obstacle
// recompute its footprint; without one, every named class does.
void passability_after_release(RouteContext& c, Unit& unit);

// Placement check of a move or a build (0x47DB70): whether a unit of type `type`
// may stand with its footprint's corner at (x, z) on layer `mode`, ignoring cells
// held by `exclude`. Off the map: only layer 2. Structures go by their yard map;
// layers other than 1 need nothing more.
bool can_place_footprint(RouteContext& c, std::uint16_t type, std::uint16_t exclude, std::int16_t x, std::int16_t z,
                         std::uint32_t mode);

// Sets up the search for the map (0x40E9E0; after the map, before any unit).
void init_route_search(RouteContext& c);
// One step of the route search (0x40EB70; the start of the players stage).
void run_route_search(RouteContext& c);
// A route goal is going away or changing its target: if the search serves it,
// the search drops the unit (0x40E9C0).
void route_search_forget(RouteSearch& s, const MotionGoal& goal);

} // namespace sim
