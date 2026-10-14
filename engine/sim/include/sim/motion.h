#pragma once
// Unit motion (see docs/RE_MEMORY.md, "Motion").
//
// Every mobile unit owns a motion object: its velocity, speed, turn of this
// step, a decaying "tilt" vector that leans aircraft into their turns, a few
// step stamps and a layer (ground or air). The motion object owns a motion
// goal, which turns what the unit's current order wants into something to
// steer at:
//   - ground units follow a route of waypoints (set by the route search);
//   - aircraft fly at a target point with a target heading.
// Mission behaviours give their order an order goal (a point to reach, an area
// to get into, a manoeuvre to fly); the order owns it and hands it to the
// motion goal. When the goal is reached or no route exists, the order hears
// it through its event bits.
//
// Each step of the unit pass runs, per mobile unit: the motion goal's step,
// ground or air steering (turn, accelerate, velocity), the move itself (with
// the occupancy grid deciding whether the unit may enter new cells), the
// move-rate script notifications and the water-occupancy script notification.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sim/heightmap.h"
#include "sim/orders.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"

namespace sim {

namespace motion_flags {
// Unit type flags.
constexpr std::uint32_t kTypeFlies = 0x800;        // air physics and an air goal
constexpr std::uint32_t kTypeHover = 0x1000;
constexpr std::uint32_t kTypeFloats = 0x80000;     // held at its waterline
constexpr std::uint32_t kTypeSeaAltitude = 0x400000;  // cruise altitude measured from the sea level
// Unit flags.
constexpr std::uint32_t kResettle = 0x10000;       // settle on the ground this step
constexpr std::uint32_t kLayerMask = 0x3;
constexpr std::uint32_t kMoveRateMask = 0xC;       // bits 2-3: move rate the script last heard
// MotionState::flags.
constexpr std::uint8_t kLayer = 0x3;               // 1 ground, 2 air
constexpr std::uint8_t kBlocked = 0x4;             // the last move into new cells was refused
}  // namespace motion_flags

// Events an order goal reports to its mission order (MissionOrder::events).
namespace goal_events {
constexpr std::uint32_t kReached = 0x20, kNoRoute = 0x40, kReplaced = 0x80;
constexpr std::uint32_t kAll = 0x3E0;  // cleared when an order gets a new goal
}  // namespace goal_events

// Unit type data motion needs.
struct MotionType {
    std::uint32_t type_flags = 0;
    std::int32_t max_velocity = 0, brake_rate = 0, acceleration = 0;  // 16.16 per step
    std::int32_t bank_scale = 0x10000, pitch_scale = 0;  // the first leans pitch, the second bank (sic)
    std::int32_t move_rate1 = 0, move_rate2 = 0;
    std::uint16_t turn_rate = 0;
    std::int16_t cruise_altitude = 0;
    std::uint8_t waterline = 0;
    std::int16_t model_height = 0;  // whole units (the integer half of the model's top)
    bool has_move_class = false;    // the route search only serves units with a movement class
};

struct MotionEnvironment {
    std::uint32_t game_tick = 0;
    std::uint8_t sea_level = 0;
    std::int32_t gravity = 0;
    OriginalAddresses addresses;
};

class MotionGoal;
class OrderGoal;

// What motion reaches outside the unit table.
class MotionSystems {
public:
    virtual ~MotionSystems() = default;
    // Occupancy grid. Whether the unit's footprint may stand with its corner at
    // map cell (x, z) on the layer; then, after a successful move into new
    // cells: clear its old footprint, claim the new one (also links it into its
    // spatial bucket) and refresh what it sees.
    virtual bool can_place(Unit& unit, std::int16_t cell_x, std::int16_t cell_z, std::uint32_t layer) = 0;
    virtual void clear_footprint(Unit& unit) = 0;
    // Also stamps MotionState::claim_tick with the game tick (units with motion).
    virtual void claim_footprint(Unit& unit) = 0;
    virtual void update_sight(Unit& unit) = 0;
    // Spatial buckets (8 x 8 cells): whether the unit is in the off-map bucket,
    // and the highest terrain in its bucket.
    virtual bool off_map(const Unit& unit) = 0;
    virtual std::uint8_t bucket_height(const Unit& unit) = 0;
    // The route search drops its work if it is serving this goal.
    virtual void route_search_forget(MotionGoal& goal) = 0;
    // Activation bit of the unit (with its scripts), as the activate value does.
    virtual void set_activated(Unit& unit, bool on) = 0;
    // The original's heap addresses of new goal objects (only compared by tests;
    // 0 is fine), and their release.
    virtual std::uint32_t allocate_object(std::uint32_t size) = 0;
    virtual void free_object(std::uint32_t address) = 0;
};

struct MotionContext {
    std::vector<Unit>& units;           // index = unit id
    std::vector<UnitTickState>& ticks;  // index = unit id
    std::vector<PlayerSlot>& players;
    const std::vector<MotionType>& types;  // index = type index
    const HeightMap& map;
    MotionSystems& systems;
    MotionEnvironment env;
    // Set when the original would have crashed (a division by zero).
    std::string fault;

    UnitTickState& tick(const Unit& u) { return ticks[u.id]; }
    const MotionType& type(const Unit& u) { return types[ticks[u.id].type_index]; }
    MotionState& motion(const Unit& u) { return ticks[u.id].motion; }
};

// ---------------------------------------------------------------------------
// Order goals (owned by a mission order, MissionOrder::goal).

class OrderGoal {
public:
    virtual ~OrderGoal() = default;
    virtual int kind() const = 0;   // 2 point, 3 manoeuvre, 4 circle, 5 ring, 6 edge
    virtual bool is_area() const { return true; }
    // Whether `unit` (the order's unit) is there.
    virtual bool reached(MotionContext& c, Unit& unit) = 0;
    virtual bool contains_cell(std::int32_t /*x*/, std::int32_t /*z*/) const { return false; }
    // Cells the route search aims at (packed: x in the low, z in the high half).
    virtual void goal_cells(std::vector<std::uint32_t>& out) const { out.clear(); }
    // How far (roughly) a cell is from the goal; 0 inside.
    virtual std::int32_t distance(std::int32_t /*x*/, std::int32_t /*z*/) const { return 0; }
    // Where to go; false if there is nothing to go to (then `out` is left alone,
    // or only partly written, as in the original).
    virtual bool position(MotionContext& c, std::array<std::int32_t, 3>& out) = 0;
    virtual bool heading(MotionContext& /*c*/, std::uint16_t& /*out*/) { return false; }
    // Stays attached to the motion goal once reached.
    virtual bool keep_when_reached() const { return false; }
    // Lets go of the units it links to (before it is deleted).
    virtual void release() {}

    void report(std::uint32_t events) const;

    MissionOrder* order = nullptr;
    Unit* owner = nullptr;          // the order's unit when the goal was made
    std::uint32_t address = 0;      // the original's heap address (tests)
};

// Kind 2: a point, optionally following another unit (one of its pieces), with
// an approach radius, altitude, heading, or a formation offset.
namespace point_goal {
constexpr std::uint16_t kFollow = 0x1, kOffset = 0x2, kMatchHeading = 0x4, kAltitude = 0x8, kRadius = 0x10,
                        kGround = 0x20, kHeading = 0x40, kFace = 0x80;
}
class PointGoal : public OrderGoal {
public:
    int kind() const override { return 2; }
    bool is_area() const override { return false; }
    bool reached(MotionContext& c, Unit& unit) override;
    bool position(MotionContext& c, std::array<std::int32_t, 3>& out) override;
    bool heading(MotionContext& c, std::uint16_t& out) override;
    bool keep_when_reached() const override;
    void release() override;
    // Setters the behaviours use.
    void set_altitude(MotionContext& c, std::int32_t altitude);
    void set_heading(std::int16_t h) { flags = static_cast<std::uint16_t>(flags | point_goal::kHeading); goal_heading = h; }
    void set_radius(std::int16_t r) { flags = static_cast<std::uint16_t>(flags | point_goal::kRadius); radius = r; }

    std::uint16_t flags = 0;
    std::int16_t radius = 0, altitude = 0, goal_heading = 0;
    std::int16_t piece = -1;        // piece of the followed unit (-1: its origin)
    UnitLink link;                  // the followed unit
    std::array<std::int32_t, 3> pos{};
    std::int32_t offset = 0;        // distance ahead of the followed unit (formation)
};

// Kind 3: an aircraft manoeuvre: a point moving with a velocity that turns
// toward a target heading.
class ManoeuvreGoal : public OrderGoal {
public:
    int kind() const override { return 3; }
    bool is_area() const override { return false; }
    bool reached(MotionContext& c, Unit& unit) override;
    bool position(MotionContext& c, std::array<std::int32_t, 3>& out) override;
    bool heading(MotionContext& c, std::uint16_t& out) override;
    void set_target_heading(std::uint16_t h) { flags = static_cast<std::uint16_t>(flags | 1); target_heading = h; }

    std::uint16_t flags = 0;  // bit 0: target heading set
    std::array<std::int32_t, 3> pos{}, vel{};
    std::int16_t unused22 = 0, unused26 = 0;
    std::uint16_t target_heading = 0;
};

// Kind 4: within a radius of a cell.
class CircleGoal : public OrderGoal {
public:
    int kind() const override { return 4; }
    bool reached(MotionContext& c, Unit& unit) override;
    bool contains_cell(std::int32_t x, std::int32_t z) const override;
    void goal_cells(std::vector<std::uint32_t>& out) const override;
    std::int32_t distance(std::int32_t x, std::int32_t z) const override;
    bool position(MotionContext& c, std::array<std::int32_t, 3>& out) override;

    std::int16_t cell_x = 0, cell_z = 0;
    std::int32_t radius = 0, radius_cells_sq = 0;
};

// Kind 5: between two radii of a cell.
class RingGoal : public OrderGoal {
public:
    int kind() const override { return 5; }
    bool reached(MotionContext& c, Unit& unit) override;
    bool contains_cell(std::int32_t x, std::int32_t z) const override;
    void goal_cells(std::vector<std::uint32_t>& out) const override;
    std::int32_t distance(std::int32_t x, std::int32_t z) const override;
    bool position(MotionContext& c, std::array<std::int32_t, 3>& out) override;

    std::int16_t cell_x = 0, cell_z = 0;
    std::int32_t outer = 0, inner = 0, outer_cells_sq = 0, inner_cells_sq = 0;
};

// Kind 6: the edge of a cell rectangle (standing next to a footprint).
class EdgeGoal : public OrderGoal {
public:
    int kind() const override { return 6; }
    bool reached(MotionContext& c, Unit& unit) override;
    bool contains_cell(std::int32_t x, std::int32_t z) const override;
    void goal_cells(std::vector<std::uint32_t>& out) const override;
    std::int32_t distance(std::int32_t x, std::int32_t z) const override;
    bool position(MotionContext& c, std::array<std::int32_t, 3>& out) override;

    std::int32_t min_x = 0, max_x = 0, min_z = 0, max_z = 0;
};

// Constructors (as the original's; `order` is the mission order the goal is for).
// A point at `pos`.
std::shared_ptr<PointGoal> make_point_goal(MotionContext& c, MissionOrder& order, const std::array<std::int32_t, 3>& pos);
// Following `target` (aircraft keep a distance of their first weapon's range, or 100).
std::shared_ptr<PointGoal> make_follow_goal(MotionContext& c, MissionOrder& order, Unit* target);
// Going to `target`'s piece `piece` and taking its heading.
std::shared_ptr<PointGoal> make_dock_goal(MotionContext& c, MissionOrder& order, Unit* target, std::int16_t piece);
// A point at `pos` tied to `target` (facing it, ground level).
std::shared_ptr<PointGoal> make_face_goal(MotionContext& c, MissionOrder& order, Unit* target, const std::array<std::int32_t, 3>& pos);
std::shared_ptr<ManoeuvreGoal> make_manoeuvre_goal(MotionContext& c, MissionOrder& order, const std::array<std::int32_t, 3>& pos,
                                                   const std::array<std::int32_t, 3>& vel);
std::shared_ptr<CircleGoal> make_circle_goal(MotionContext& c, MissionOrder& order, std::int32_t x, std::int32_t z, std::int32_t radius);
std::shared_ptr<RingGoal> make_ring_goal(MotionContext& c, MissionOrder& order, std::int32_t x, std::int32_t z, std::int32_t outer,
                                         std::int32_t inner);
// `cell` and `size` packed (x low, z high halves).
std::shared_ptr<EdgeGoal> make_edge_goal(MotionContext& c, MissionOrder& order, std::uint32_t cell, std::uint32_t size);

// ---------------------------------------------------------------------------
// Motion goals (owned by the motion object).

class MotionGoal {
public:
    virtual ~MotionGoal() = default;
    // Takes over `goal` (or none); the previous one hears kReplaced.
    virtual void attach(MotionContext& c, OrderGoal* goal);
    virtual void step(MotionContext& /*c*/) {}
    // The first `n` (<= 3) points of the route; leaves `out` alone without a route kind.
    virtual void route_points(MotionContext& /*c*/, std::array<std::array<std::int32_t, 3>, 3>& /*out*/, int /*n*/) {}
    // Where an aircraft should be, how that point moved and the heading to take;
    // leaves the outputs alone if the goal has none.
    virtual void flight_target(std::array<std::int32_t, 3>& /*pos*/, std::array<std::int32_t, 3>& /*delta*/, std::uint16_t& /*heading*/) {}
    virtual bool has_route() const { return attached != nullptr; }
    // The route search asks every unit in turn whether it wants a route now.
    virtual bool wants_route(MotionContext& /*c*/) { return false; }

    OrderGoal* attached = nullptr;
    Unit* unit = nullptr;
    std::uint32_t address = 0;
};

// Ground units of playing players: a route of up to 20 waypoints (whole world units).
namespace route_flags {
constexpr std::uint8_t kHasRoute = 0x1, kWantsRoute = 0x2, kBlocked = 0x4, kChanged = 0x8;
}
class RouteGoal : public MotionGoal {
public:
    void attach(MotionContext& c, OrderGoal* goal) override;
    void step(MotionContext& c) override;
    void route_points(MotionContext& c, std::array<std::array<std::int32_t, 3>, 3>& out, int n) override;
    bool has_route() const override { return (flags & route_flags::kHasRoute) != 0; }
    bool wants_route(MotionContext& c) override;
    // The route search's answer (`n` = 0: no route).
    void set_route(MotionContext& c, const std::uint32_t* points, std::int32_t n);

    std::array<std::uint32_t, 20> points{};  // x in the low, z in the high half
    std::int32_t count = 0;
    std::uint32_t search_tick = 0;
    std::uint8_t flags = 0;  // bits 4-7 uninitialised in the original
};

// Aircraft of playing players: a point to fly at.
class FlightGoal : public MotionGoal {
public:
    void attach(MotionContext& c, OrderGoal* goal) override;
    void step(MotionContext& c) override;
    void flight_target(std::array<std::int32_t, 3>& pos, std::array<std::int32_t, 3>& delta, std::uint16_t& heading) override;

    std::array<std::int32_t, 3> pos{}, delta{};
    std::uint16_t target_heading = 0;
    std::uint8_t net_flags = 0;  // network state (bit 0: changed; bits 1-2 the layer last sent)
    bool local = true;           // false: fed by the network (remote players' aircraft)
};

// Ground units of watched (remote) players: up to three waypoints from the network.
class RemoteRouteGoal : public MotionGoal {
public:
    void route_points(MotionContext& c, std::array<std::array<std::int32_t, 3>, 3>& out, int n) override;
    bool has_route() const override { return count >= 2; }

    std::array<std::uint32_t, 3> points{};
    std::int32_t count = 0;
};

// ---------------------------------------------------------------------------
// The motion object.

// Builds the unit's motion object (its goal by owner and type: playing or
// watched player, flying or not).
void create_motion(MotionContext& c, Unit& unit);
// Deletes the goal (the route search forgets it) and the motion object.
void destroy_motion(MotionContext& c, Unit& unit);

// One step of the unit's motion (the unit pass's motion call).
void run_motion(MotionContext& c, Unit& unit);

// The stale stack word the motion step leaves for the unit pass: a register of
// the pass (30 when water damage is on, else the address of the live-unit count).
std::uint32_t motion_stale_word(const UnitTickEnvironment& env);

// Parts of the step, also used on their own.
void steer_ground(MotionContext& c, Unit& unit);
void steer_air(MotionContext& c, Unit& unit);
void accelerate(MotionContext& c, Unit& unit, std::int32_t delta);
void lean(MotionContext& c, Unit& unit, const std::array<std::int32_t, 3>& impulse);
void move_unit(MotionContext& c, Unit& unit);
void notify_move_rate(MotionContext& c, Unit& unit);
void notify_water_occupancy(MotionContext& c, Unit& unit);
// Puts the unit at a position (relinking it on the grid when it changes cells or layer).
void place_unit(MotionContext& c, Unit& unit, const std::array<std::int32_t, 3>& pos, std::uint32_t layer);
// Switches between ground (1) and air (2); landing stops the unit.
void set_motion_layer(MotionContext& c, Unit& unit, std::uint32_t layer);

// Gives `order` a new goal (or none), replacing and deleting its old one.
void attach_order_goal(MotionContext& c, MissionOrder& order, std::shared_ptr<OrderGoal> goal);
// Ground units only (aircraft just lose their goal).
void attach_circle_goal(MotionContext& c, MissionOrder& order, const std::array<std::int32_t, 3>& pos, std::int32_t radius);
void attach_ring_goal(MotionContext& c, MissionOrder& order, const std::array<std::int32_t, 3>& pos, std::int32_t outer,
                      std::int32_t inner);
// An order being destroyed: its goal is detached (if it is the current one) and deleted.
void release_order_goal(MotionContext& c, Unit& unit, MissionOrder& order);

// Map cell of a 16.16 position for a footprint (the cell of its corner).
std::int16_t footprint_cell(std::int32_t pos, std::int16_t footprint);

} // namespace sim
