#pragma once
// Mission behaviours that drive units around: aircraft (take-off, landing,
// patrols, guarding, attack runs, landing pads), transports (loading and
// unloading, on the ground and in the air) and the remaining ground orders
// (chasing attacks, kamikaze runs, fire at a position, following, repair
// patrols, resurrection, teleporting). See docs/RE_MEMORY.md, "Aircraft and
// transports" and "Mission behaviours".
//
// Like the other behaviours (sim/missions.h, sim/construction.h) each one is
// a step function of (unit, order, event) returning a mission_result. They
// steer by giving their order a movement goal (sim/motion.h); the goal objects,
// the motion layer, the spatial buckets and the players' air base lists are
// reached through UnitMissionSystems so that tests can watch each request.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "sim/economy.h"
#include "sim/footprint.h"
#include "sim/missions.h"
#include "sim/motion.h"
#include "sim/orders.h"
#include "sim/projectile.h"
#include "sim/targeting.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"

namespace sim {

// Unit type data only these behaviours read.
struct UnitMissionTypeInfo {
    std::uint16_t attack_run_length = 0;    // bombers: extra distance of the run past the drop point (world units)
    std::uint16_t kamikaze_distance = 0;    // kamikaze units go off within this distance (world units)
};

// A movement goal a behaviour gives its order: built, adjusted by setter calls in
// the order given, then attached (replacing the order's previous goal).
struct GoalRequest {
    enum class Kind : std::uint8_t {
        Point,      // a point (pos)
        Follow,     // following `target`
        Dock,       // `target`'s piece `piece`
        Face,       // a ground point (pos) tied to `target`
        Manoeuvre,  // an aircraft manoeuvre at pos with velocity vel
    };
    // Altitude, radius and heading of point goals; a manoeuvre's target heading, and its
    // altitude setter (which does nothing).
    enum class Set : std::uint8_t { Altitude, Radius, Heading, TargetHeading, ManoeuvreAltitude };
    Kind kind = Kind::Point;
    std::array<std::int32_t, 3> pos{}, vel{};
    Unit* target = nullptr;
    std::int16_t piece = 0;
    std::array<std::pair<Set, std::int32_t>, 3> sets{};
    int set_count = 0;

    GoalRequest& set(Set what, std::int32_t value) {
        sets[static_cast<std::size_t>(set_count++)] = {what, value};
        return *this;
    }
};

// What these behaviours reach outside the simulation core.
class UnitMissionSystems {
public:
    virtual ~UnitMissionSystems() = default;
    // Bookkeeping of the original's heap (only tests use it): a block of `size`
    // bytes was taken (the behaviours' temporary lists grow this way).
    virtual void heap_alloc(std::uint32_t size) = 0;
    // A new movement goal for the order: allocated, built and adjusted by the setter calls
    // (MissionOrder::goal / attached are not touched yet).
    virtual std::shared_ptr<OrderGoal> make_goal(Unit& unit, MissionOrder& order, const GoalRequest& goal) = 0;
    // The order takes the goal (MissionOrder_AttachGoal 0x4388D0: the old one is detached and
    // deleted; without a motion object nothing happens and the new goal is lost).
    virtual void attach_goal(Unit& unit, MissionOrder& order, std::shared_ptr<OrderGoal> goal) = 0;
    // A setter call on a goal already built (the same setters as GoalRequest::set).
    virtual void adjust_goal(Unit& unit, OrderGoal& goal, GoalRequest::Set what, std::int32_t value) = 0;
    // Motion layer change: 1 lands (stops), 2 takes off (sim/motion.h set_motion_layer).
    virtual void set_layer(Unit& unit, std::uint32_t layer) = 0;
    // Units filed in the spatial buckets whose horizontal distance to `pos` is within
    // `radius` (16.16), bucket by bucket, as the original visits them (0x47E890).
    virtual void units_in_radius(const std::array<std::int32_t, 3>& pos, std::int32_t radius, std::vector<Unit*>& out) = 0;
    // The activated air base builders on the unit table of `player` (a player state index;
    // the table's list from its last refresh)
    // within `radius` world units of `pos` (0x40B530).
    virtual void air_bases_in_radius(int player, const std::array<std::int32_t, 3>& pos, std::int32_t radius,
                                     std::vector<Unit*>& out) = 0;
    // Teleporting: the unit is put at `pos` on `layer` (0x48A9F0), with a trail of dots from
    // where it was (0x471FD0: effect list 5, 30 steps; its particles draw the C runtime rand()).
    virtual void place_unit(Unit& unit, const std::array<std::int32_t, 3>& pos, std::uint32_t layer) = 0;
    virtual void teleport_trail(Unit& unit, const std::array<std::int32_t, 3>& to) = 0;
    // Resurrection: the wreck feature covering map cell `cell` (index) goes (0x4246B0, not
    // forced), and the new unit takes the attitude of the feature slot the wreck's own cell
    // names (`origin`'s feature_aux; 0x1420B slots, not checked against their count).
    virtual void remove_feature(std::int32_t cell) = 0;
    virtual void wreck_attitude(const MapCell& origin, Unit& unit) = 0;
    // The y of the reclaim sites a repair patrol finds (0x47EA40 never writes it: whatever the
    // stack held there; an input).
    virtual std::int32_t reclaim_site_y() = 0;
    // Network notice: the unit's script function `name` was started with arguments (0x456200).
    virtual void broadcast_script_call(Unit& unit, const char* name, std::int32_t argc, std::int32_t a, std::int32_t b,
                                       std::int32_t c, std::int32_t d) = 0;
};

struct UnitMissionEnvironment {
    std::int32_t map_width = 0, map_height = 0;  // visible map size in world units (cells x 16 - 32, cells x 16 - 128)
    std::uint8_t sea_level = 0;
    std::int32_t gravity = 0;
    // The stack address of the air repair patrol's metal site record (its event argument ends
    // up pointing there; run-varying in the original, so an input).
    std::uint32_t air_repair_patrol_site_address = 0;
};

// Context the behaviours run in (Missions::units_ext points at it).
struct UnitMissions {
    const std::vector<TargetingTypeInfo>& targeting;  // by type index
    const std::vector<MotionType>& motion;             // by type index
    const std::vector<UnitMissionTypeInfo>& types;     // by type index
    const MapGrid& map;
    const std::vector<FeatureTypeInfo>& features;
    const HeightMap& heights;
    // Explored map (per 2 x 2 cells, bit p = player p has seen it; sim/sight.h), width in entries.
    const std::vector<std::uint16_t>& explored;
    UnitMissionSystems& systems;
    UnitMissionEnvironment& env;
    // The players' storage capacities (sim/economy.h; the repair patrols weigh them), or none.
    const std::vector<PlayerEconomy>* economy = nullptr;
    // Unit type data by type index (resurrection reads the build time of the type it brings back).
    const std::vector<UnitTickTypeInfo>* tick_types = nullptr;
};

// Mission kinds handled here.
bool unit_mission_kind(std::uint8_t kind);
// One step of one of them (same contract as run_mission_behaviour).
std::int32_t run_unit_mission(Missions& m, Unit& unit, MissionOrder& order, std::uint32_t& event);

// Whether the unit could stand on the ground at `pos` (0x47E2D0): its footprint fits the
// map; on cells its owner has not explored always; else no blocking feature, yard mark or
// other unit, and the heights suit its water depth and slope limits.
bool can_land_at(Missions& m, Unit& unit, const std::array<std::int32_t, 3>& pos);

// Units filed in the spatial buckets within `radius` (16.16, horizontal) of `pos`, bucket by
// bucket (rows of buckets in z, then x) and each bucket's list in order (0x47E890; the off-map
// list and carried units are not visited).
void units_in_radius(const UnitBuckets& buckets, const std::array<std::int32_t, 3>& pos, std::int32_t radius,
                     std::vector<Unit*>& out);
// The activated air base builders on a unit table's list within `radius` world units (0x40B530).
// False when there is no such table (the original crashes).
bool air_bases_in_radius(const UnitTable& table, const std::vector<UnitTickState>& ticks, const std::array<std::int32_t, 3>& pos,
                         std::int32_t radius, std::vector<Unit*>& out);

// Unit script requests of transports. ATTACH_UNIT (0x481340): a live unit not carried by
// another transport becomes the scripted unit's cargo at `piece` (packet 0x0A with `mode` as
// its layer). DROP_UNIT (0x4813B0): the scripted unit's cargo is set down if its footprint
// fits where it hangs (ground layer).
void script_attach_unit(Missions& m, Unit& carrier, std::int32_t passenger, std::int32_t piece, std::int32_t mode);
void script_drop_unit(Missions& m, Unit& carrier, std::int32_t passenger);

} // namespace sim
