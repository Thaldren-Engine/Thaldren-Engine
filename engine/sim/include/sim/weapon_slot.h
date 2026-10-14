#pragma once
// A unit's weapon slots and the weapon type data they use
// (see docs/RE_MEMORY.md, "Weapons: slot pass and aiming").

#include <cstdint>

#include "sim/script_vm.h"

namespace sim {

// Weapon type flag bits (the ones the simulation reads so far).
namespace weapon_flags {
constexpr std::uint32_t kLineOfSight = 0x1, kBallistic = 0x2, kVerticalLaunch = 0x10, kWaterWeapon = 0x10000,
                        kTurret = 0x80000, kCruise = 0x2000000, kCommandFire = 0x4000000, kStockpile = 0x10000000;
}

// Which fire routine a weapon type uses (picked from its flags when the types are loaded).
enum class WeaponFireRoutine : std::uint8_t { None, Turret, VerticalLaunch, LineOfSight, Dropped };

struct WeaponType {
    std::uint32_t flags = 0;
    WeaponFireRoutine fire = WeaponFireRoutine::None;
    std::int32_t velocity = 0;          // muzzle speed per tick (16.16)
    std::int32_t range = 0;             // world units
    float energy_per_shot = 0.0f, metal_per_shot = 0.0f;
    float min_barrel_angle = 0.0f;      // radians
    std::uint16_t reload_time = 0;      // ticks
    std::uint16_t tolerance = 0, pitch_tolerance = 0;  // aim tolerances (0 = defaults)
    std::uint8_t id = 0;
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
