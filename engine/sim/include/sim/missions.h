#pragma once
// Mission behaviours: what one step of a unit's order does (see docs/RE_MEMORY.md,
// "Mission behaviours").
//
// The order lists (sim/orders.h) call one behaviour per order kind through
// OrderSystems::run_mission. This module implements the behaviours that need
// no unit motion: switches (activate, cloak, standing orders), waiting,
// stopping, paralysis, self-destruct, stationary attacks and guards, idle
// standby, stockpile building and stationary repair. Every other kind is
// passed on to MissionSystems::run_other, and so is everything outside the
// simulation core (target search, attack order issue, speech, effects, the
// economy's stall state).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/combat.h"
#include "sim/orders.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"

namespace sim {

// Unit type data the behaviours read (in addition to UnitTypeInfo / UnitTickTypeInfo).
struct MissionTypeInfo {
    std::uint32_t abilities = 0;     // mission_abilities bits
    std::uint16_t worker_time = 0;   // build/repair power per second
};

namespace mission_abilities {
constexpr std::uint32_t kOnOff = 1u << 2, kAttack = 1u << 4, kCloak = 1u << 13;
constexpr std::uint32_t kCountdownMask = 0x700000u;  // self-destruct countdown in seconds (bits 20-22)
}

// UnitTickTypeInfo::type_flags bits the behaviours read.
namespace mission_type_flags {
constexpr std::uint32_t kBuilder = 0x40, kCanFly = 0x800, kHover = 0x1000, kFloater = 0x80000;
}

// Which order routine called the behaviour (only an undefined read of the
// self-destruct countdown depends on it: the caller's return address).
enum class MissionCaller : std::uint8_t { Primary, Secondary };

// Fixed texts some speech calls carry.
enum class SpeechText : std::uint8_t { None, RepairAborted, RepairsUnsuccessful, UnitRepaired };

// Systems the behaviours reach outside the simulation core.
class MissionSystems {
public:
    virtual ~MissionSystems() = default;
    // Behaviours not implemented here (unit motion and construction); same contract
    // as OrderSystems::run_mission.
    virtual std::int32_t run_other(Unit& unit, MissionOrder& order, std::uint32_t& event) = 0;
    // Spoken/printed unit notices (id as the original's speech table index).
    virtual void speech(Unit& unit, std::uint32_t id, SpeechText text) = 0;
    // Enemy units of `player` the player can see (or has on radar) within `radius`
    // of `pos`, in the order the visibility system lists them.
    virtual void enemies_in_radius(int player, const std::array<std::int32_t, 3>& pos, std::int32_t radius,
                                   std::vector<Unit*>& out) = 0;
    // The unit's best target when its fire order is "fire at will", else none.
    virtual Unit* fire_at_will_target(Unit& unit) = 0;
    // Puts an attack order on `target` in front of the unit's orders if its standing
    // orders allow (or `forced`); true if it did.
    virtual bool issue_attack_order(Unit& unit, Unit& target, bool forced) = 0;
    // The mission kind an attack on `target` (or none) resolves to for this unit (0 = none).
    virtual std::uint8_t resolve_attack_mission(Unit& unit, Unit* target) = 0;
    // Books energy and metal use on the unit's resource slot; false (nothing paid) while
    // its player stalls on either.
    virtual bool use_resources_if_not_stalled(Unit& unit, float energy, float metal) = 0;
    // User interface: the unit's build/stockpile counts changed.
    virtual void build_counts_changed(Unit& unit) = 0;
    // Presentation: a nano beam from `from` to the box {min x, y, min z, max x, max y, max z}.
    virtual void nano_beam(const std::array<std::int32_t, 3>& from, const std::array<std::int32_t, 6>& box) = 0;
    // The order's movement goal object (if any) is detached from the unit's motion and
    // deleted (only called for units with a motion object). Must leave `order.attached` = 0.
    virtual void drop_goal_object(Unit& unit, MissionOrder& order) = 0;
};

struct MissionEnvironment {
    std::uint8_t sea_level = 0;
    std::int32_t gravity = 0;
    OriginalAddresses addresses;  // an undefined read can see a unit's original address
};

struct Missions {
    OrderContext& orders;
    std::vector<Unit>& units;  // index = unit id (the whole table, empty slots included)
    Combat& combat;
    UnitWorld& world;
    MissionSystems& systems;
    const std::vector<MissionTypeInfo>& types;  // by type index
    // Alliance bytes of each player slot, indexed by the other player's state index
    // (PlayerSlot::state); nonzero = allied.
    const std::vector<std::array<std::uint8_t, 16>>& allied;
    MissionEnvironment& env;
    MissionCaller caller = MissionCaller::Primary;
    // Set when the original would crash (or read memory the model cannot know);
    // the result of that step is then meaningless.
    std::string fault;
};

// Whether `kind` is implemented here (the rest goes to MissionSystems::run_other).
bool mission_implemented(std::uint8_t kind);
// One step of the order's behaviour; returns a mission_result value. `event` is the
// behaviour's third argument (none of the behaviours here reuse it as a variable).
std::int32_t run_mission_behaviour(Missions& m, Unit& unit, MissionOrder& order, std::uint32_t& event);

// Weapon slot helpers the behaviours use.
// The slot (0..2, or 3 for all: 0, 1, then 2) stops being released: an enabled slot
// marked released loses the mark and forgets its target.
void unrelease_weapon_slot(Unit& unit, int slot);
// The slot aims at `target` (by id); the unit's pending attack-related events are dropped.
void aim_weapon_at_unit(Unit& unit, const Unit& target, int slot);
// Whether the slot's weapon can reach `target` (heights against the sea, air-only
// weapons, ballistic solution, horizontal range).
bool weapon_reaches_unit(Missions& m, Unit& unit, Unit& target, int slot);

} // namespace sim
