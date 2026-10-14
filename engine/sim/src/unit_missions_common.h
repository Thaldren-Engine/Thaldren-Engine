#pragma once
// Helpers shared by the movement-driven mission behaviours (air_missions.cpp,
// ground_missions.cpp). Internal to the simulation library.

#include <array>
#include <cstdint>
#include <vector>

#include "sim/construction.h"
#include "sim/footprint.h"
#include "sim/missions.h"
#include "sim/unit_missions.h"
#include "sim/x87.h"

namespace sim {
// Defined in missions.cpp (0x43A020): the way back of a patrol.
void ensure_patrol_return(Missions& m, Unit& u, MissionOrder& o);
}  // namespace sim

namespace sim::um {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using Vec = std::array<i32, 3>;

inline i32 wadd(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
inline i32 wsub(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
inline i32 wneg(i32 a) { return static_cast<i32>(0u - static_cast<u32>(a)); }
inline i32 wshl(i32 a, int n) { return static_cast<i32>(static_cast<u32>(a) << n); }
inline i32 hi16(i32 v) { return static_cast<i16>(static_cast<u32>(v) >> 16); }
inline i32 square_hi(i32 v) { return static_cast<i32>(static_cast<u32>(static_cast<std::uint64_t>(static_cast<i64>(v) * v) >> 32)); }
// The horizontal distance measure the original compares with a radius squared.
inline i32 flat_distance_sq(const Vec& a, const Vec& b) { return wadd(square_hi(wsub(a[0], b[0])), square_hi(wsub(a[2], b[2]))); }
// FCOMP with 0.0 and a test of "equal": NaN counts as equal.
inline bool is_zero(float f) { return !(f < 0.0f || f > 0.0f); }

// Unit flag and type bits.
constexpr u32 kAlive = unit_flags::kAlive, kDying = unit_tick_flags::kDying, kHasWeapons = 0x80000000u, kStatic = 0x20000000u;
constexpr u32 kFireOrders = 0x300000u, kMoveOrders = 0xC0000u;
constexpr u32 kTypeBuilder = 0x40, kTypeAirBase = 0x200, kTypeCanFly = 0x800, kTypeHover = 0x1000, kTypeAmphibious = 0x200000;
constexpr u32 kAbilityLoad = 0x100, kAbilityRepair = 0x200, kAbilityResurrect = 0x800;

UnitMissions& ctx(Missions& m);
inline UnitTickState& tick_of(Missions& m, const Unit& u) { return m.orders.ticks[u.id]; }
inline u32 type_flags(Missions& m, const Unit& u) {
    const UnitTickTypeInfo* t = tick_of(m, u).tick_type;
    return t ? t->type_flags : 0;
}
inline bool flies(Missions& m, const Unit& u) { return (type_flags(m, u) & kTypeCanFly) != 0; }
const MissionTypeInfo& mission_type(Missions& m, const Unit& u);
const TargetingTypeInfo& targeting_type(Missions& m, const Unit& u);
const MotionType& motion_type(Missions& m, const Unit& u);
const UnitMissionTypeInfo& extra_type(Missions& m, const Unit& u);
const ConstructionTypeInfo& construction_type(Missions& m, const Unit& u);
inline u32 game_tick(Missions& m) { return m.orders.game_tick; }
inline bool has_motion(Missions& m, const Unit& u) { return tick_of(m, u).has_motion; }
// Motion layer bits of the unit's motion object (1 ground, 2 air).
inline u32 motion_layer(Missions& m, const Unit& u) { return tick_of(m, u).motion.flags & 3u; }
inline PlayerSlot& owner_of(Missions& m, const Unit& u) { return m.orders.players[static_cast<std::size_t>(tick_of(m, u).owner)]; }
inline Unit* target_of(const MissionOrder& o) { return o.target.unit; }
inline u32 max_damage(const Unit& u) { return u.type ? u.type->max_damage : 0; }
// Hit points (as the original's unsigned compare of the signed word) below three quarters.
inline bool badly_damaged(const Unit& u) { return static_cast<u32>(static_cast<i32>(u.hit_points)) < (max_damage(u) >> 2) * 3; }
// The `player`'s alliance byte for the owner of `other` (0 = enemy).
bool allied(Missions& m, int player, const Unit& other);
// Whether the type is in the category mask (a 512-bit set by type index).
bool in_category(Missions& m, const CategoryMask& mask, const Unit& target);

void set_deadline(Missions& m, MissionOrder& o, u32 ticks);
void speech(Missions& m, Unit& u, u32 id, SpeechText t = SpeechText::None);
// A newly issued order announces itself once (order flag 0x2000), with a status text.
void announce(Missions& m, MissionOrder& o, SpeechText text = SpeechText::None);

// Mission kind values by the names the behaviours look up.
// New order of `kind` (target, position, parameters) in front of the unit's list / at its end.
MissionOrder* new_order(Missions& m, u8 kind, Unit* target, const i32* pos, i32 p0 = 0, i32 p1 = 0, i32 p2 = 0);
void prepend_new(Missions& m, Unit& u, u8 kind, Unit* target, const i32* pos, i32 p0 = 0, i32 p1 = 0, i32 p2 = 0);
void append_new(Missions& m, Unit& u, u8 kind, Unit* target, const i32* pos, i32 p0 = 0, i32 p1 = 0, i32 p2 = 0);

// Goals.
GoalRequest point_goal(const Vec& pos);
void attach(Missions& m, MissionOrder& o, const GoalRequest& g);
// The two halves of attach: the goal is built first, attached later (or never).
std::shared_ptr<OrderGoal> make_goal(Missions& m, MissionOrder& o, const GoalRequest& g);
void attach_made(Missions& m, MissionOrder& o, std::shared_ptr<OrderGoal> goal);
void set_goal_radius(Missions& m, MissionOrder& o, const std::shared_ptr<OrderGoal>& goal, i32 r);
// The order's goal goes (MissionOrder_AttachGoal with none).
void drop_goal(Missions& m, MissionOrder& o);
void circle_goal(Missions& m, MissionOrder& o, const Vec& pos, i32 radius);
void ring_goal(Missions& m, MissionOrder& o, const Vec& pos, i32 outer, i32 inner);
void edge_goal(Missions& m, MissionOrder& o, u32 cell, u32 size);

// A temporary list of units grown the way the original's vectors are (each growth takes a
// new block from the heap: capacity 1, 2, 4, 8, ...).
struct UnitList {
    std::vector<Unit*> v;
    std::size_t capacity = 0;
    void push(Missions& m, Unit* u);
};

// The point `dist` ahead of the heading `angle` from `from` (the original's -sin, -cos offsets).
Vec ahead(const Vec& from, u16 angle, i32 dist);
// Heading from a to b (atan2 of a - b), as Math_AngleBetweenXY (0x48A980).
u16 heading_between(const Vec& a, const Vec& b);
// ftol(hypot(dx, dz)) of two 16.16 points (low 32 bits).
i32 flat_distance(const Vec& a, const Vec& b);
// Map position snapped to the unit's footprint grid (centre of the rounded corner cell).
i32 snap_axis(i32 v, i16 footprint);

// Weapon helpers.
void aim_slot_at_point(Unit& u, const Vec& pos, int slot);  // 0x48A0A0
Unit* slot_target_unit(Missions& m, Unit& u, int slot);     // 0x48A190
i32 slot_range(Missions& m, Unit& u, u32 slot);             // 0x49ADF0
u8 first_weapon_slot(const Unit& u);                        // 0x4897E0

// The unit's script starts `name` with no arguments (0x4B0940), running it now or not.
void script_call(Missions& m, Unit& u, const char* name, bool run_now);
inline void script_call_now(Missions& m, Unit& u, const char* name) { script_call(m, u, name, true); }
// Starts `name` with arguments (0x4B0A70).
void script_call_args(Missions& m, Unit& u, const char* name, bool run_now, i32 argc, i32 a, i32 b = 0, i32 c = 0, i32 d = 0);
// Runs `name` now with in/out arguments (0x4B0BC0).
void script_query(Missions& m, Unit& u, const char* name, i32* a, i32* b = nullptr, i32* c = nullptr, i32* d = nullptr);

// The unit can repair `target` (0x4899B0).
bool can_repair(Missions& m, Unit& unit, Unit* target);

// The unit's transport drops it (packet 0x0A, layer 2 when an aircraft takes off).
void leave_carrier(Missions& m, Unit& u, u8 layer);

// Map cells and features.
const MapCell* cell_at(const MapGrid& map, const Vec& pos);  // 0x4815A0
u16 resolve_feature(Missions& m, const MapGrid& map, const MapCell& cell);  // 0x421E60

// Reclaimable wrecks with energy / metal around a point (0x47EA40): the best of three random
// picks of each kind (by amount).
struct ReclaimSites {
    bool energy_found = false, metal_found = false;
    Vec energy_pos{}, metal_pos{};
    float energy_amount = 0.0f, metal_amount = 0.0f;
};
bool pick_reclaim_sites(Missions& m, const Vec& centre, i32 radius, ReclaimSites& out);
// A unit a repair patrol would see to (0x405D90 / 0x4158D0): an ally on the ground, damaged or
// unfinished, not just hit by its own player's reclaiming.
bool repair_patrol_candidate(Missions& m, const Unit& self, const Unit& c);
// A repair order on `target` honouring the standing move order (0x43B400); false if none.
bool enqueue_repair(Missions& m, Unit& u, Unit& target, bool no_standing_move);

// Shared pieces of the behaviours (air_missions.cpp).
using Set = GoalRequest::Set;
i32 cruise_altitude(Missions& m, const Unit& u);
void take_off(Missions& m, Unit& u, MissionOrder& o);
std::int32_t head_back_onto_map(Missions& m, Unit& u, MissionOrder& o);
bool answer_attacker(Missions& m, Unit& u, MissionOrder& o, Unit& followed, u32 event);
bool seek_air_base(Missions& m, Unit& u, MissionOrder& o);
bool beyond_leash(const Unit& u, const MissionOrder& o);

// The behaviours.
std::int32_t air_move(Missions& m, Unit& u, MissionOrder& o);
std::int32_t air_standby(Missions& m, Unit& u, MissionOrder& o);
std::int32_t air_land_if_can(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t air_follow(Missions& m, Unit& u, MissionOrder& o, u32& event);
std::int32_t air_seek_attack(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t air_seek_guard(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t air_patrol(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t air_pickup(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t air_unload(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t air_landing(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t air_strike(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t air_to_ground(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t air_to_air(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t air_to_ground_hover(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t air_evade(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t air_get_repaired(Missions& m, Unit& u, MissionOrder& o);
std::int32_t air_repair_patrol(Missions& m, Unit& u, MissionOrder& o, u32& event);
std::int32_t attack_chase(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t attack_kamikaze(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t suppress(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t follow_ground(Missions& m, Unit& u, MissionOrder& o, u32& event);
std::int32_t ground_pickup(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t ground_unload(Missions& m, Unit& u, MissionOrder& o, u32 event);
std::int32_t repair_patrol(Missions& m, Unit& u, MissionOrder& o, u32& event);
std::int32_t teleport(Missions& m, Unit& u, MissionOrder& o);

}  // namespace sim::um
