#pragma once
// A unit's weapon slots and the weapon type data they use
// (see docs/RE_MEMORY.md, "Weapons: slot pass and aiming").

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sim/script_vm.h"

namespace sim {

// Weapon type flag bits (see docs/RE_MEMORY.md, "Weapon definitions").
namespace weapon_flags {
constexpr std::uint32_t kLineOfSight = 1u << 0, kBallistic = 1u << 1, kBeam = 1u << 3, kVerticalLaunch = 1u << 4,
                        kMeteor = 1u << 5, kParalyzer = 1u << 7, kDropped = 1u << 8, kStartSmoke = 1u << 9,
                        kEndSmoke = 1u << 10, kSoundTrigger = 1u << 11, kGuidance = 1u << 12, kTracks = 1u << 13,
                        kUnitsOnly = 1u << 14, kGroundBounce = 1u << 15, kWaterWeapon = 1u << 16, kSmokeTrail = 1u << 18,
                        kTurret = 1u << 19, kSelfPropelled = 1u << 20, kPropeller = 1u << 21, kNoExplode = 1u << 22,
                        kBurnBlow = 1u << 23, kTwoPhase = 1u << 24, kCruise = 1u << 25, kCommandFire = 1u << 26,
                        kNoAutoRange = 1u << 27, kStockpile = 1u << 28, kTargetable = 1u << 29,
                        kInterceptor = 1u << 30;
}

// Which fire routine a weapon type uses (picked from its flags when the types are loaded).
enum class WeaponFireRoutine : std::uint8_t { None, Turret, VerticalLaunch, LineOfSight, Dropped };

struct WeaponType {
    std::uint8_t index = 0;             // definition slot (identifies the weapon in network packets)
    std::uint32_t flags = 0;            // weapon_flags
    WeaponFireRoutine fire = WeaponFireRoutine::None;
    std::int32_t velocity = 0;          // muzzle speed per tick (16.16)
    std::int32_t start_velocity = 0;
    std::int32_t acceleration = 0;
    std::int32_t range = 0;             // world units
    float energy_per_shot = 0.0f, metal_per_shot = 0.0f;
    float min_barrel_angle = 0.0f;      // radians
    std::uint16_t reload_time = 0;      // ticks
    std::uint16_t tolerance = 0, pitch_tolerance = 0;  // aim tolerances (0 = defaults)
    std::uint16_t accuracy = 0;         // spread of turret shots (angle units)
    std::int32_t coverage = 0;          // interceptors: half-width of the square they defend (world units)
    std::uint16_t weapon_timer = 0;     // steps
    std::uint16_t turn_rate = 0;        // angle per step
    std::uint16_t burst = 0, burst_rate = 0, spray_angle = 0;
    std::uint16_t duration = 0;         // beam length in steps
    std::uint16_t random_decay = 0, smoke_delay = 0, flight_time = 0, hold_time = 0;
    std::uint16_t area_of_effect = 0;   // diameter, world units
    std::uint16_t default_damage = 0;
    float edge_effectiveness = 0.0f;
    std::int32_t shake_magnitude = 0, shake_duration = 0;
    std::uint16_t sound_start = 0xFFFF, sound_hit = 0xFFFF, sound_water = 0xFFFF;  // 0xFFFF = none
    // Damage by unit name, in the order of a case-insensitive sort (first spelling
    // of a name wins); other units take default_damage.
    std::vector<std::pair<std::string, std::int32_t>> damage;
};

// Receives the aim script's answer: any nonzero return marks the aim as done.
class AimResultSink : public ScriptReturnSink {
public:
    void on_return(std::int32_t value) override {
        if (value != 0) result = 1;
    }
    std::uint32_t handle() const override { return handle_word; }
    std::int32_t result = 0;
    std::uint32_t handle_word = 1;  // what scripts see as the sink (the original's slot address + 4)
};

namespace slot_flags {
constexpr std::uint8_t kAiming = 1, kEnabled = 2, kScriptIndexMask = 0x0C;  // script index = (flags >> 2) & 3
constexpr std::uint8_t kReleased = 0x10;  // targets released by an order (set once)
}

// target_z of a slot aimed at a unit (target_x is then its id; 0 = no target).
constexpr std::int16_t kUnitTarget = -0x8000;

// Scripts keep a pointer to `aim` while an aim call runs, so a slot must not move then.
struct WeaponSlot {
    std::int16_t target_x = 0;            // ground point x (world units) or target unit id
    std::int16_t target_z = kUnitTarget;  // ground point z, or kUnitTarget
    AimResultSink aim;
    const WeaponType* type = nullptr;
    std::int32_t aux = 0;                 // set at slot setup, used by ballistic projectiles
    std::uint16_t reload = 0;             // ticks until the next shot
    std::uint16_t aim_heading = 0;        // last aim sent to the script, relative to the unit
    std::uint16_t aim_pitch = 0;
    std::uint8_t stockpile = 0;           // stored shots of a stockpile weapon
    std::uint8_t flags = 0;
};

} // namespace sim
