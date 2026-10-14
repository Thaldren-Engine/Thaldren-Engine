#pragma once
// Target acquisition (see docs/RE_MEMORY.md, "Target acquisition").
//
// How units find something to shoot at and how an attack becomes an order:
// - every player keeps lists of the enemy units it can see and has on radar,
//   rebuilt every 30 steps (the players' stage);
// - each step the player's weapon-target pass visits a few of its units and
//   points their released weapon slots at a target chosen at random from
//   the visible enemies in weapon range, preferring ones outside the weapon's
//   bad-target category;
// - idle units with "fire at will" look for a target in sight and attack it
//   (the attack order honours the unit's standing move order);
// - a damaged unit answers its attacker (retargeting).
//
// The line-of-sight test itself, the AI and the map's explored state belong to
// other systems and are reached through TargetingSystems.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/missions.h"
#include "sim/orders.h"
#include "sim/projectile.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"

namespace sim {

// A set of unit types as one bit per type (index = a type's category bit).
using CategoryMask = std::array<std::uint32_t, 16>;

// Unit type data targeting reads (in addition to the tick, mission and unit type data).
struct TargetingTypeInfo {
    std::uint16_t category_bit = 0;  // the type's bit in category masks (the original: its type index)
    std::array<CategoryMask, 3> bad_target{};  // per weapon slot: types it shoots only when nothing else qualifies
    CategoryMask no_chase{};                   // types the unit does not go after
    std::int16_t sight_distance = 0;           // world units
    std::int16_t maneuver_leash = 0;           // how far a unit on "maneuver" may chase
    std::int16_t max_water_depth = 0, min_water_depth = 0;
    std::int16_t footprint_x = 0;
    std::uint8_t transport_size = 0, transport_capacity = 0;
    bool has_build_options = false;            // the type can build (mobile build orders)
    const WeaponType* first_weapon = nullptr;  // the definition's first weapon (aircraft attack kinds)
};

// Type flag and ability bits targeting reads.
namespace targeting_type_flags {
constexpr std::uint32_t kAirBase = 0x200, kTargetingUpgrade = 0x400, kCanFly = 0x800, kHover = 0x1000;
constexpr std::uint32_t kShootMe = 0x8000, kHasWeapons = 0x10000, kAmphibious = 0x200000, kHoverAttack = 0x8000000;
constexpr std::uint32_t kKamikaze = 0x10000000;
}
namespace targeting_abilities {
constexpr std::uint32_t kAttack = 0x10, kGuard = 0x20, kPatrol = 0x40, kMove = 0x80, kLoad = 0x100, kRepair = 0x200,
                        kReclaim = 0x400, kResurrect = 0x800, kCapture = 0x1000, kSpecialAttack = 0x4000,
                        kNotTransportable = 0x80000;
}

// What a player's command or a behaviour wants of a unit; resolved into a
// mission kind per unit and target (resolve_mission). Values 2, 3 and 9 are
// OrderIntent's.
namespace intent {
constexpr std::uint8_t kDefault = 1, kMove = 2, kAttack = 3, kSpecialAttack = 4, kUnload = 5, kPickup = 6, kGuard = 7,
                       kRepair = 8, kPatrol = 9, kStop = 10, kTeleport = 11, kReclaim = 12, kCapture = 13, kBuild = 14;
}

// Per player: its weapon-target controller and its unit table (the enemy lists).
struct PlayerController {
    bool exists = false;
    std::uint16_t cursor = 0;      // unit id the weapon-target pass visited last (0 = none yet)
    std::uint32_t ai_hold_tick = 0;  // written when an AI capturer is hit (read by AI behaviours)
};
struct UnitTable {
    bool exists = false;
    int player = 0;                // player record the table was made for
    std::vector<Unit*> seen;       // enemy units in sight (last refresh), in unit table order
    std::vector<Unit*> radar;      // enemy units marked on radar (unit flag 0x100)
    bool targeting_upgrade = false;  // the player has an active targeting facility: radar dots count
    std::uint32_t refresh_tick = 0;
};
struct TargetingState {
    std::array<PlayerController, 10> controllers{};  // by player slot
    std::array<UnitTable, 10> tables{};              // by player index (slot, or the state index at creation)
};

struct TargetingEnvironment {
    std::uint32_t game_tick = 0;
    std::uint8_t sea_level = 0;
    std::uint8_t local_player = 0;   // slot of the player at this machine
    std::uint16_t unit_limit = 0;    // units per player; the weapon-target pass visits limit/30 + 1 units per step
    bool target_everything = false;  // game option: units without "shoot me" are fair targets for everyone
    std::int32_t command_mode = 0;   // game option: 1 = the alternative default-command rules
};

// Systems targeting reaches outside the simulation core.
class TargetingSystems {
public:
    virtual ~TargetingSystems() = default;
    // Line of sight: whether the player can see the unit (its own units always).
    virtual bool unit_visible(int player, Unit& unit) = 0;
    // Whether the map spot is explored for this machine's view, on the owner player's sight grid.
    virtual bool position_explored(int player, const std::array<std::int32_t, 3>& pos) = 0;
    // Flag byte of a feature type (bit 7: reclaimable/resurrectable wreck).
    virtual std::uint8_t feature_flags(std::uint16_t feature) = 0;
    // The table slot of a shot the unit's interceptor slot should chase, or -1; and its position.
    virtual std::int32_t intercept_target(Unit& unit, int slot, std::array<std::int32_t, 3>& pos) = 0;
    // The AI: its own units seen by a table refresh (statistics), and an occasional rescoring.
    virtual void ai_own_unit(int table, Unit& unit) = 0;
    virtual void ai_rescore(int table) = 0;
    // The AI's part of a refresh before the units are listed and after them (sim/ai.h).
    virtual void ai_table_begin(int) {}
    virtual void ai_table_end(int) {}
    // User interface: the "unit under attack" notice.
    virtual void under_attack(Unit& unit) = 0;
};

struct Targeting {
    std::vector<Unit>& units;  // index = unit id
    std::vector<UnitTickState>& ticks;
    std::vector<PlayerSlot>& players;
    const std::vector<std::array<std::uint8_t, 16>>& allied;  // by player slot, indexed by the other's state
    const std::vector<TargetingTypeInfo>& types;              // by type index
    const std::vector<MissionTypeInfo>& mission_types;        // by type index
    const MapGrid& map;
    std::uint32_t feature_count = 0;
    Missions& missions;  // order lists, the battle RNG and the weapon range check
    TargetingEnvironment& env;
    TargetingSystems& systems;
    TargetingState& state;
    // Set when the original would crash or read memory the model cannot know.
    std::string fault;
};

// Battle start: every player record that is not a watched one (active with controller 3)
// gets a controller and a unit table (the table under the player's state index).
void create_player_targeting(Targeting& t);

// Players' stage, per playing player: the weapon-target pass (after the AI's own work for
// an AI player), then the unit table refresh when due.
void weapon_target_pass(Targeting& t, int player, bool ai);
void refresh_unit_table(Targeting& t, int table);

// Enemy units listed for `table` within `radius` of `pos` (sight list; with a targeting
// upgrade and nobody in sight, the radar list), appended to `out`.
void enemies_in_radius(Targeting& t, int table, const std::array<std::int32_t, 3>& pos, std::int32_t radius,
                       std::vector<Unit*>& out);
// A target for weapon `slot`: up to 50 random picks among the listed enemies within sight
// (or the weapon's range when `in_weapon_range`), the nearest after a random discount.
Unit* find_best_target(Targeting& t, Unit& unit, int slot, bool in_weapon_range);
// The best target when the unit's fire order is "fire at will", else none.
Unit* fire_at_will_target(Targeting& t, Unit& unit);
// Puts an attack order on `target` in front of the unit's orders if its standing orders allow
// (or `forced`); units on "maneuver" also get a move back to where they stand.
bool issue_attack_order(Targeting& t, Unit& unit, Unit& target, bool forced);
// The mission kind `what` (an intent value) means for this unit on `target` / at `pos` (0 = none).
std::uint8_t resolve_mission(Targeting& t, std::uint8_t what, Unit& unit, Unit* target, const std::array<std::int32_t, 3>* pos);
// The victim of an attack answers it.
void retarget_attack(Targeting& t, Unit* attacker, Unit& victim);

} // namespace sim
