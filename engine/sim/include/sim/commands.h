#pragma once
// Command input: how a player's actions become unit orders (docs/RE_MEMORY.md, "Commands").
//
// The original has two routes, and both end in the general order routine (sim/missions.h
// enqueue_order):
//  - the user interface acts on the local player's *selected* units (a unit flag), resolving
//    an intent (move, attack, guard, ...) per unit, spreading point orders of a group around
//    its centre, and treating a queued order that repeats an existing one as a cancel;
//  - the computer player orders single units directly.
// Orders are never sent over the network as commands: every machine orders only its own
// player's units and the units' states travel instead (see "Network and sync").
//
// Both routes are available here, plus a plain-data Command that a user interface, the
// computer player, a network layer or a replay can carry and apply between steps.

#include <array>
#include <cstdint>
#include <vector>

namespace sim {

class Battle;
struct Unit;

namespace command {

// What a Command does.
enum class Kind : std::uint8_t {
    None = 0,
    // Selection of the local player's units (the "selected" unit flag).
    Select = 1,         // units: added (queued) or replacing the selection
    Deselect = 2,       // units: removed from the selection
    ClearSelection = 3,
    // Orders to the selected units (the user interface route).
    IssueIntent = 10,   // `intent` resolved per unit (sim/targeting.h intent::*), at `pos` / `target`
    IssueMission = 11,  // `mission` (sim/missions.h mission::*) with `param0` / `param1`
    // Orders to the listed units (the computer player's route; no selection involved).
    OrderUnits = 20,    // `mission` to each listed unit
    BuildCount = 21,    // each listed factory or builder: queue of `unit_type` grows by `param0`
    MobileBuild = 22,   // each listed builder: build `unit_type` at `pos`
    // Groups (the player's group slots: 1-9 control groups).
    AssignGroup = 30,   // selected units join group `param0`
    SelectGroup = 31,   // the units of group `param0` become the selection
    // Resources.
    TransferEnergy = 40,  // to player `param0`, `amount`
    TransferMetal = 41,
};

namespace flags {
constexpr std::uint8_t kQueued = 1;  // shift held: append (or cancel a matching queued order)
}

// One command, as plain data (fixed layout, see encode / decode).
struct Command {
    Kind kind = Kind::None;
    std::uint8_t player = 0;         // the commanding player's slot
    std::uint8_t flags = 0;          // flags::*
    std::uint8_t intent = 0;         // IssueIntent
    std::uint8_t mission = 0;        // IssueMission / OrderUnits
    std::uint16_t target = 0;        // target unit id (0: none); for IssueIntent the unit under the cursor
    std::uint16_t unit_type = 0;     // BuildCount / MobileBuild
    std::array<std::int32_t, 3> pos{};  // 16.16 world position
    bool has_pos = false;
    std::int32_t param0 = 0, param1 = 0;
    float amount = 0.0f;
    std::vector<std::uint16_t> units;  // Select / Deselect / OrderUnits / BuildCount / MobileBuild

    bool queued() const { return (flags & flags::kQueued) != 0; }
};

// A command due before step `tick` runs (the step whose tick counter becomes `tick`).
struct Timed {
    std::uint32_t tick = 0;
    Command command;
};

// Applies one command to the battle now (between steps, where the original's user interface
// and computer player act). Returns false when nothing was done.
bool apply(Battle& battle, const Command& c);

// --- The routes themselves (apply() calls these; a user interface or AI may call them directly).

// The selected flag of a unit (unit flags bit 4) and the local player's selection.
constexpr std::uint32_t kSelectedFlag = 0x10u;
void set_selected(Battle& battle, Unit& unit, bool on);
void clear_selection(Battle& battle, int player);
// Orders to the player's selected units (the user interface's routine, 0x48CF30): `intent`
// nonzero is resolved per unit (with `hover` the unit under the cursor, excluded from the
// order), else `mission` is used as given. Point orders of selected units close to the
// selection's centre keep their offset from it. A queued order equal to one already queued
// cancels it (0x43AFC0).
void issue_to_selected(Battle& battle, int player, std::uint8_t intent, std::uint8_t mission, bool queued, Unit* hover,
                       const std::array<std::int32_t, 3>* pos, std::int32_t param0, std::int32_t param1);
// Orders to every unit of the player in group `group` (0x480460).
void issue_to_group(Battle& battle, int player, std::int32_t group, std::uint8_t intent, bool queued, Unit* target,
                    const std::array<std::int32_t, 3>* pos, std::int32_t param0, std::int32_t param1);
// A queued order equal to one in the unit's primary list cancels it, else the order is given
// (0x43AFC0, the user interface's per-unit step).
void issue_or_cancel(Battle& battle, std::uint8_t mission, bool queued, Unit& unit, Unit* target,
                     const std::array<std::int32_t, 3>* pos, std::int32_t param0, std::int32_t param1);
// One order to one unit (the computer player's route: the general order routine 0x43ADC0).
bool order_unit(Battle& battle, Unit& unit, std::uint8_t mission, bool queued, Unit* target,
                const std::array<std::int32_t, 3>* pos, std::int32_t param0, std::int32_t param1);

// --- Encoding (network, replay files): little-endian, fixed field order.
void encode(const Command& c, std::vector<std::uint8_t>& out);
// Reads one command at `at`, advancing it; false on malformed input.
bool decode(const std::vector<std::uint8_t>& in, std::size_t& at, Command& c);

}  // namespace command
}  // namespace sim
