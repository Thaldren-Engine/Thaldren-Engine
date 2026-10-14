#pragma once
// Weapon definition table (weapons\*.tdf -> 256 slots) built exactly as
// TotalA.exe does (see docs/RE_MEMORY.md, "Weapon definitions").

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ta/hpi.h"

namespace ta {

enum class WeaponFireFn : std::uint8_t { Unset, Turret, VLaunch, LineOfSight, Dropped };

struct WeaponDefRecord {
    bool loaded = false;           // a TDF section targeted this slot
    std::string name;              // section name (aName[32])
    std::string description;       // [64]
    WeaponFireFn fire = WeaponFireFn::Unset;
    std::vector<std::pair<std::string, std::int32_t>> damage_overrides;  // sorted as the game stores them
    bool has_damage_map = false;
    std::int32_t weapon_velocity = 0, start_velocity = 0, weapon_acceleration = 0;
    int model_slot = -1;           // slot whose 3DO model this weapon uses (-1 = none)
    std::string model_name;        // aModelName (blank when sharing an earlier weapon's model)
    std::string explosion_seq;     // "" = none
    std::string water_explosion_seq;
    std::string explosion_gaf, water_explosion_gaf;  // anims\<name>.GAF holding them
    float energy_per_shot = 0, metal_per_shot = 0, min_barrel_angle = 0;
    std::int32_t shake_magnitude = 0, shake_duration = 0;
    std::int16_t default_damage = 0, area_of_effect = 0;
    float edge_effectiveness = 0;
    std::int32_t range = 0, coverage = 0;
    std::int16_t reload_time = 0, weapon_timer = 0, turn_rate = 0, burst = 0, burst_rate = 0, spray_angle = 0;
    std::int16_t duration = 0, random_decay = 0, smoke_delay = 0, flight_time = 0, hold_time = 0;
    std::string sound_start, sound_hit, sound_water;  // sound paths ("" = none, index -1)
    std::int16_t accuracy = 0, tolerance = 0, pitch_tolerance = 0;
    std::uint8_t index = 0, fire_starter = 0, render_type = 0, color = 0, color2 = 0;
    std::uint32_t flags = 0;
};

struct WeaponDefTable {
    std::array<WeaponDefRecord, 256> slots;
    std::vector<std::string> notes;
};

struct WeaponDefOptions {
    bool lava_world = false;        // selects lava instead of water explosion keys
    bool cd_check_enabled = true;   // loose (non-archive) weapon files ignored
};

WeaponDefTable load_weapon_defs(Vfs& vfs, const WeaponDefOptions& opt);

} // namespace ta
