#pragma once
// Meteor showers and the camera shake: two parts of a step that draw the simulation thread's
// C runtime rand() (docs/RE_MEMORY.md, "Battle step").

#include <cstdint>

#include "sim/rng.h"

namespace sim {

struct WeaponType;

// Meteor shower state (stage 8, after the wind). Configured from the map info (or the
// default shower file) at load; runs whether or not showers are enabled: every scheduled
// shower picks its origin and aim with four rand() draws, and only an enabled one drops
// meteors.
struct MeteorShower {
    bool enabled = false;               // the map asks for meteors
    std::int32_t radius = 0;            // scatter of the drop points around the aim (whole units)
    std::int32_t spawn_interval = 0;    // steps between meteors while active
    std::int32_t duration = 0;          // steps a shower lasts
    std::int32_t schedule_gap = 0;      // steps from a shower's end to the next one
    const WeaponType* weapon = nullptr; // the meteor weapon
    // Running state.
    bool active = false;
    std::uint32_t next_schedule = 0, end_tick = 0, next_spawn = 0;
    std::int16_t origin_x = 0, origin_z = 0, aim_x = 0, aim_z = 0;  // map cells

    // The configuration's floats as the loader turns them into steps (truncating).
    void configure(std::int32_t radius_units, float density, float duration_seconds, float interval_seconds);
    // At map load (after the configuration): not active, the first schedule at the spawn
    // interval (sic), the weapon (null: the first weapon definition).
    void reset(const WeaponType* meteor_weapon);
};

// One step of the shower. True when a meteor drops this step: an ownerless shot of
// `weapon` at `pos` with `vel` (16.16), announced to the network.
bool step_meteors(MeteorShower& m, std::uint32_t tick, std::int32_t map_width, std::int32_t map_height, Lcg15& rand,
                  std::int32_t (&pos)[3], std::int32_t (&vel)[3]);

// Camera shake from explosions (the view is presentation, but its decay draws rand()).
struct CameraShake {
    bool shaking = false;
    std::int32_t accum_x = 0, accum_y = 0, magnitude = 0, ticks_left = 0;
    std::int32_t offset_x = 0, offset_y = 0;  // this step's view displacement (presentation)
};
// An explosion adds to the shake (unless the player turned shaking off).
void add_camera_shake(CameraShake& s, std::int32_t dx, std::int32_t dy, std::int32_t magnitude, bool disabled);
// The step's decay (inside the camera stage).
void step_camera_shake(CameraShake& s, Lcg15& rand);

}  // namespace sim
