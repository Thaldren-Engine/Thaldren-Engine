#pragma once
// The per-step unit pass: every live unit of every playing player, in table
// order, gets its wind notification, weapons, script step, timers, health
// percentage, environment damage, healing, orders and motion
// (see docs/RE_MEMORY.md, "Unit tick").

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/unit.h"

namespace sim {

// Unit type data the unit pass and combat need (in addition to UnitTypeInfo).
struct UnitTickTypeInfo {
    float wind_generator = 0.0f;
    std::uint16_t heal_time = 0;
    std::uint32_t type_flags = 0;           // bit 12: immune to water damage, bit 26: immune to paralysis
    std::int32_t damage_modifier = 0x10000; // 16.16 scale of damage taken while armored
    std::int32_t build_time = 0;
    float build_cost_energy = 0.0f;
    float build_cost_metal = 0.0f;
    std::string name;                       // unit name, matched against the side's commander
};

// State of a unit only the unit pass touches.
struct UnitTickState {
    std::uint16_t type_index = 0;        // 0 = empty table slot
    int owner = 0;                       // player slot
    std::uint8_t damaged_timer = 0;      // counts down
    std::uint32_t transfer_holdoff = 0;  // counts down
    std::uint8_t health_percent = 0;
    std::uint8_t prev_health_percent = 0;
    std::uint8_t last_damage_kind = 0;
    bool has_motion = false;
    const UnitTickTypeInfo* tick_type = nullptr;
    std::uint16_t kill_count = 0;            // kills scored; raises the damage dealt (veterancy)
    Unit* last_attacker = nullptr;
    std::uint8_t last_attacker_slot = 10;    // player slot of the last attacker, 10 = none
    float metal_make = 0.0f;                 // metal income of the unit's resource slot
};

namespace unit_tick_flags {
constexpr std::uint32_t kSelected = 0x10, kSelectable = 0x20, kDying = 0x4000, kCarrierSelectable = 0x40000000;
}

struct PlayerSlot {
    bool active = false;
    std::uint8_t controller = 0;  // 1 local, 2 AI/remote-simulated, 3 watched (scripts only)
    std::uint8_t state = 0;       // alliance/view index; 10 = out of the game
    int first_unit = 0, last_unit = -1;  // inclusive range of the unit table owned by this slot
    std::int32_t net_id = -1;
    std::uint16_t units_killed = 0, units_lost = 0, commanders_killed = 0, commanders_lost = 0;
    std::uint16_t alive_count = 0;               // live units owned
    std::uint16_t flags = 0;                     // bit 0: still has its commander
    std::array<std::uint8_t, 10> shares_vision{}; // by the other player's state index
    std::string commander_name;                  // unit name of this player's side commander
};

struct UnitTickEnvironment {
    std::uint32_t game_tick = 0;
    bool wind_changed = false;
    std::uint16_t wind_heading = 0;
    std::int32_t wind_speed = 0;
    bool water_damage_enabled = false;
    std::int32_t water_damage = 0;
    std::uint8_t sea_level = 0;
    bool broadcast_unit_states = false;  // network game flag
    bool camera_auto_follow = false;     // "big brother" camera cycling
};

// Systems the unit pass calls into, in this order per unit.
class UnitTickSystems {
public:
    virtual ~UnitTickSystems() = default;
    virtual void weapons(Unit& unit) = 0;
    virtual void damage(Unit* attacker, Unit& target, std::int32_t amount, std::int32_t kind, std::int32_t heading) = 0;
    virtual void repair_heal(Unit& worker, Unit& target, float rate) = 0;
    virtual void primary_orders(Unit& unit) = 0;
    virtual void secondary_orders(Unit& unit) = 0;
    virtual void motion(Unit& unit) = 0;
    virtual void settle_height(Unit& unit) = 0;
    virtual void killed(Unit& unit, std::uint8_t kind) = 0;
    virtual void broadcast_player_units(int player) = 0;
    virtual void camera_follow_step() = 0;
};

// Runs one step of the unit pass. `units` and `ticks` are parallel tables
// (index = unit id); returns the number of live units visited.
std::int32_t run_unit_pass(std::vector<Unit>& units, std::vector<UnitTickState>& ticks, std::vector<PlayerSlot>& players,
                           UnitTickEnvironment& env, UnitTickSystems& systems, std::int16_t& camera_countdown);

} // namespace sim
