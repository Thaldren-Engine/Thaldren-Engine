#pragma once
// Unit mission orders (see docs/RE_MEMORY.md, "Unit orders").
//
// Every unit has two singly linked order lists. The head of the primary list
// is what the unit is doing now; the secondary list holds background orders
// that run side by side with it. Each step the unit pass runs the primary head
// (repeatedly, until it goes to sleep or waits for an event) and then every due
// secondary order through its mission behaviour, and applies the result the
// behaviour returns: keep going, advance a step counter, sleep a while, finish,
// rotate to the back of the list or clear everything.
//
// The mission behaviours themselves are hooks (`OrderSystems::run_mission`).

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "sim/rng.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"

namespace sim {

// Mission kinds. The value is the index into the mission table, which is
// sorted by the missions' lookup names.
namespace mission {
enum Kind : std::uint8_t {
    kReady, kActivate, kAirStrike, kAirToAir, kAirToGround, kAirToGroundHover, kAttackChase, kAttackKamikaze,
    kAttackNoMove, kAttackSpecial, kAttackUnitType, kBeCarried, kBuildingBuild, kBuildWeapon, kCapture, kCloakOff,
    kCloakOn, kDeactivate, kFollowGround, kGetBuilt, kGroundPickup, kGroundUnload, kGuardNoMove, kHelpBuild,
    kMakeSelectable, kMobileBuild, kMoveGround, kParalyze, kPark, kPatrol, kQueuedMove, kQueuedPatrol, kReclaim,
    kReclaimUnit, kRepairPatrol, kRepairUnit, kRepairUnitNoMove, kResurrect, kSelfDestruct, kSelfDestructFg,
    kSelfRepair, kStandby, kStandbyMine, kStandingFireOrder, kStandingMoveOrder, kStop, kSuppress, kTeleport,
    kAirEvade, kAirFollow, kAirGetRepaired, kAirHelpBuild, kAirLandIfCan, kAirLanding, kAirMobileBuild, kAirMove,
    kAirPatrol, kAirPickup, kAirReclaim, kAirReclaimUnit, kAirRepairPatrol, kAirRepairUnit, kAirSeekAttack,
    kAirSeekGuard, kAirStandby, kAirUnload, kWait, kWaitForAttack,
    kCount
};
// Flags a new order of each kind starts with (before the constructor's adjustments).
const std::array<std::uint32_t, kCount>& default_flags();
}  // namespace mission

// What a mission behaviour tells the order list to do with its order.
namespace mission_result {
constexpr std::int32_t kRestart = 0;    // step counter back to 0
constexpr std::int32_t kAdvance = 1;    // step counter + 1
constexpr std::int32_t kContinue = 2;   // nothing (4 too)
constexpr std::int32_t kSnooze = 3;     // sleep 30 + random(15) steps
constexpr std::int32_t kFinished = 5;   // remove the order (8 too)
constexpr std::int32_t kRotate = 6;     // primary: move to the back; secondary: remove and stop
constexpr std::int32_t kClearAll = 7;   // primary: drop every order; secondary: remove and stop
constexpr std::int32_t kFinishOrIdle = 9;  // primary: remove unless it is the last order, else sleep 30 + random(30)
}  // namespace mission_result

namespace order_flags {
constexpr std::uint32_t kKeep = 0x4;             // survives a partial clear
constexpr std::uint32_t kHasTarget = 0x200;      // the target unit link is kept
constexpr std::uint32_t kHasPosition = 0x400;    // the position is meaningful
constexpr std::uint32_t kIdle = 0x4000;          // made because the unit had nothing to do; inherited by orders put in front
constexpr std::uint32_t kNotCurrent = 0x10000;   // destroyed while not the primary head: weapon targets stay
constexpr std::uint32_t kSecondary = 0x40000;    // lives in the secondary list
constexpr std::uint32_t kBuilding = 0x400000;    // destroying it stops the building script
constexpr std::uint32_t kLastOrder = 0x800000;   // set by result 9
}  // namespace order_flags

// Wait mask bits (also the event bits a sleeping primary order can be woken by).
namespace order_wait {
constexpr std::uint32_t kTimer = 0x1;            // wake at `wake_tick`
constexpr std::uint32_t kNotifyDestroy = 0x2;    // run the behaviour once more (event 2) when destroyed
constexpr std::uint32_t kClearWeapons = 0x10000; // clear the three weapon targets before running
}  // namespace order_wait

struct MissionOrder;
class OrderGoal;

// A link from an order to a unit. Every unit keeps the list of links pointing
// at it (`Unit::referrers`), newest first.
struct UnitLink {
    Unit* unit = nullptr;
    UnitLink* next = nullptr;  // next link pointing at the same unit
    MissionOrder* owner = nullptr;
    OrderGoal* goal = nullptr;  // set instead of owner for links held by an order goal (sim/motion.h)
};

struct MissionOrder {
    std::uint32_t address = 0;          // the original's heap address of the order (see OrderSystems::allocate_order)
    std::uint8_t kind = 0;
    std::uint8_t step = 0;              // the behaviour's step counter
    std::uint32_t wait = 0;             // order_wait bits the order sleeps on (0 = runs)
    std::uint32_t wake_tick = 0xFFFFFFFFu;
    Unit* unit = nullptr;               // the unit whose list holds it
    UnitLink target;                    // target unit
    std::array<std::int32_t, 3> pos{};  // target position (16.16)
    std::uint32_t scratch0 = 0, scratch1 = 0;  // behaviour state, zeroed by the constructor
    std::int32_t param0 = 0, param1 = 0, param2 = 0;
    std::uint32_t flags = 0;            // order_flags
    std::uint32_t created_tick = 0;
    MissionOrder* next = nullptr;
    std::uint32_t events = 0;           // events that arrived for the order
    std::uint32_t attached = 0;         // movement goal object owned by the order (opaque handle, 0 = none)
    std::shared_ptr<OrderGoal> goal;    // that goal object, when sim/motion.h made it (attached = its address)
};

// Systems the order lists reach outside this module.
class OrderSystems {
public:
    virtual ~OrderSystems() = default;
    // One step of the order's mission behaviour (by `order.kind`); returns a
    // mission_result value. `event` is the behaviour's third argument: the events
    // that woke it (primary list), 0 (secondary list) or 2 (the order is being
    // destroyed). Several original behaviours reuse that argument slot as a local
    // variable; the secondary list leaves whatever they put there in the unit
    // pass's stale stack word, so the hook must update it the same way.
    virtual std::int32_t run_mission(Unit& unit, MissionOrder& order, std::uint32_t& event) = 0;
    // Weapon slot `slot` forgets its target (only if it has one) and the script hears it.
    virtual void clear_weapon_target(Unit& unit, int slot) = 0;
    // The weapon slots drop the targets their current order gave them (all three slots).
    virtual void release_weapon_targets(Unit& unit) = 0;
    // Called when an order of a unit with a motion object is destroyed: the
    // motion goal lets go of the order's attached object, which is deleted. Must
    // leave `order.attached` = 0.
    virtual void release_attached(Unit& unit, MissionOrder& order) = 0;
    // Network notice that the unit's script function was started.
    virtual void broadcast_script_call(Unit& unit, std::int16_t function) = 0;
    // Order memory. `address` of a new order is the original's heap address for
    // it; it only matters as the value the secondary list can leave in the stale
    // stack word. The allocator must not fail.
    virtual MissionOrder* allocate_order() = 0;
    virtual void free_order(MissionOrder* order) = 0;
};

struct OrderContext {
    std::vector<UnitTickState>& ticks;  // index = unit id
    std::vector<PlayerSlot>& players;
    BattleRng& rng;
    OrderSystems& systems;
    std::uint32_t game_tick = 0;
};

// Points `link` at `unit` (nothing if null or an empty table slot), first unlinking it.
void relink_unit(OrderContext& c, UnitLink& link, Unit* unit);
void unlink_unit(UnitLink& link);
// The release of a dying unit's links (0x489740, teardown of the kill packet): every link
// pointing at it, newest first, tells its owning order event 8, then lets go of it. Links
// held by order goals (follow, dock, face) have no owner and just let go.
void release_referrers(Unit& unit);

// Builds a new order of `kind` in `order` (fresh memory from allocate_order).
// `pos` (3 values) may be null; without a target the order loses kHasTarget,
// without a position kHasPosition.
void construct_order(OrderContext& c, MissionOrder& order, std::uint8_t kind, Unit* target, const std::int32_t* pos,
                     std::int32_t param0, std::int32_t param1, std::int32_t param2);
// Runs the order's teardown (destroy notification, building script stop,
// attached goal object, weapon targets, target link) and frees it.
void destroy_order(OrderContext& c, MissionOrder* order);

// Puts `order` into the unit's list chosen by its kSecondary flag, in front of
// `before` (which must be in that list, or null for the end).
void insert_order(Unit& unit, MissionOrder& order, MissionOrder* before);
// Puts `order` at the front of the unit's list chosen by its kSecondary flag.
void prepend_order(Unit& unit, MissionOrder& order);
// Removes `order` from the list its kSecondary flag names, if it is there, and destroys it.
void remove_order(OrderContext& c, Unit& unit, MissionOrder* order);
// Drops the unit's primary orders (all of them, or those without kKeep) and,
// with `all`, its secondary orders too.
void clear_orders(OrderContext& c, Unit& unit, bool all);

// Adds `amount` of paralysis: to the primary head if it is already a paralyze
// order, else as a new paralyze order in front.
void add_paralysis(OrderContext& c, Unit& unit, std::int32_t amount);

// The per-step order processing of the unit pass. The original's primary
// routine leaves the pass's player record address (OriginalAddresses::player
// of the slot being visited) in the stale stack word whatever it does; the
// caller sets that. The secondary routine updates `stale_word` itself: the
// event argument after a behaviour step, 15 after a random draw, the order's
// address after destroying one, unchanged if it called nothing.
void run_primary_orders(OrderContext& c, Unit& unit);
void run_secondary_orders(OrderContext& c, Unit& unit, std::uint32_t& stale_word);

} // namespace sim
