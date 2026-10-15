#pragma once
// Scripted single-player missions: the mission's win and loss goals, and the units a mission
// places at its start with their first orders (docs/RE_MEMORY.md, "Campaign").
//
// Goals are judged for the local player (slot 0, the human) against slot 1 (the opponent):
//  - win goals: the mission is won when every win goal holds (checked in order, stopping at
//    the first that does not); none given means "the opponent has no units left";
//  - loss goals: the mission is lost when any holds (checked in order, stopping at the first
//    that does); none given means "the player has no usable unit left".
// Some goals are polled when checked, others are set by unit events (a unit destroyed or
// changing hands). A goal that is met for the first time asks for the "goal met" cue
// (presentation only).
// A mission without placed units leaves the goals inactive: it never ends by them.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sim {

class Battle;
struct Unit;

enum class GoalKind : std::uint8_t {
    // Win goals.
    KillEnemyCommander,   // event: the opponent's own commander destroyed
    DestroyAllUnits,      // poll: the opponent has no live units
    KillAllMobileUnits,   // event: the opponent loses a mobile unit and at most one mobile unit is left
    BuildUnitType,        // poll: the player has a finished unit of the type
    CaptureUnitType,      // event: an opponent's unit of the type changes hands
    KillAllOfType,        // event: an opponent's unit of the type dies and at most one of the type is left
    KillUnitType,         // event: `count` opponent units of the type destroyed
    MoveUnitToRadius,     // poll: a usable unit of the player (of the type, unless any) within the circle
    UnitTypeNearX,        // poll: a unit of the player (of the type, unless any) within 2 cells of column x
    UnitTypeNearZ,        // poll: ... of row z
    VictoryTimer,         // poll: the step reached the deadline
    // Loss goals.
    CommanderKilled,      // event: the player's own commander destroyed
    AllUnitsKilled,       // poll: no usable unit of the player left
    AllUnitsKilledOfType, // event: a unit of the type dies and at most one of the type is left (player and opponent)
    UnitTypeKilled,       // event: `count` units of the type destroyed (any owner)
    DeathTimer,           // poll: the step reached the deadline
    AnyUnitNearX,         // poll: an opponent unit within 2 cells of column x
    AnyUnitNearZ,         // poll: ... of row z
};

struct MissionGoal {
    GoalKind kind = GoalKind::DestroyAllUnits;
    bool satisfied = false;
    bool celebrated = false;    // the cue was asked for
    std::string type_name;      // the unit type named by the goal ("" = any type where allowed)
    std::uint16_t type = 0;     // resolved type index (resolved when the original resolves it)
    std::int32_t count = 0;     // units left to destroy (KillUnitType, UnitTypeKilled); scratch count of the scans
    // MoveUnitToRadius: the circle's centre as given (map x and the screen-row y of the editor),
    // converted to a terrain position on the first check; radius 16.16.
    std::array<std::int32_t, 3> pos{};
    bool pos_converted = false;
    std::int32_t radius = 0;
    std::int32_t line = 0;      // the column / row (cells) of the Near goals
    std::uint32_t deadline = 0; // timers: the step
};

struct MissionGoals {
    bool active = true;  // false: the mission's goals never decide (no placed units)
    std::vector<MissionGoal> win, loss;  // at most 16 each (the original's fixed lists)
    std::uint32_t faults = 0;            // checks where the original divides by zero
};

// One order of a placed unit's first orders. Unit references are names resolved against the
// placed units (their identifier or unit name, first match in placement order, only units
// that were created).
struct MissionUnitOrder {
    enum class Kind : std::uint8_t {
        AttackAt,        // attack intent at pos
        AttackType,      // attack-unit-type mission, param = type
        Build,           // structure: build `type` x count; mobile: build `type` at pos x count
        BuildWeapon,     // count
        SelfDestruct,
        Guard,           // guard intent on `ref`
        LoadInto,        // put the unit into transport `ref`
        Move,            // move intent at pos
        StandingOrders,  // move and fire standing orders (count = move, param = fire)
        Patrol,          // patrol intent at pos, param
        MakeSelectable,
        Unload,          // unload intent at pos
        WaitForAttack,   // `ref` (empty or not found: the unit itself)
        Wait,            // param = steps, count = second value
    };
    Kind kind = Kind::Move;
    std::array<std::int32_t, 3> pos{};  // 16.16
    std::uint16_t type = 0;
    std::int32_t count = 0;
    std::int32_t param = 0;
    std::string ref;
    bool ref_given = false;   // WaitForAttack: a name was read
    bool final_order = false; // the original's "explicit end" marker: no closing make-selectable order
};

struct MissionUnit {
    std::string unit_name;   // as written (the type lookup is the importer's)
    std::string ident;
    std::uint16_t type = 0;  // battle type index, 0 = unknown type (not placed)
    int player = 0;          // slot (the file's 1-based number minus one)
    std::array<std::int32_t, 3> pos{};  // 16.16 as written (structures snap to their footprint)
    std::uint16_t heading = 0;
    std::int32_t health_percent = 100;
    bool immune = false;     // unit flag 0x8000
    bool has_orders = false; // an initial order string was given
    std::string order_text;  // the order string, turned into orders by the start's parser
};

// Turns a placed unit's order string into orders (the importer's; it is called for the placed
// units in placement order, after every unit is placed, as the original parses them).
using MissionOrderParser = std::function<std::vector<MissionUnitOrder>(const std::string&)>;

// The battle start of a mission: places the units (types, positions, health, facing), then
// gives every placed unit its first orders. Returns false (with a reason) where the original
// stops the program (a unit for a player that does not play).
bool place_mission_units(Battle& battle, const std::vector<MissionUnit>& units, const MissionOrderParser& parse, std::string& error);

// Win and loss checks of the local player's turn (provider 1). The goals' event handlers.
bool mission_won(Battle& battle);
bool mission_lost(Battle& battle);
void mission_unit_destroyed(Battle& battle, Unit& unit);
void mission_unit_transferred(Battle& battle, Unit& unit);
// One goal: its check (win goals: met; loss goals: lost) and its event handlers.
bool mission_goal_check(Battle& battle, MissionGoal& goal);
void mission_goal_unit_destroyed(Battle& battle, MissionGoal& goal, Unit& unit);
void mission_goal_unit_transferred(Battle& battle, MissionGoal& goal, Unit& unit);

// The editor's map position (x, screen row y) to a terrain position (0x484B50).
std::array<std::int32_t, 3> screen_to_terrain(const Battle& battle, std::int32_t x, std::int32_t y);

}  // namespace sim
