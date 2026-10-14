#pragma once
// The effect particle lists (docs/RE_MEMORY.md, "Battle step", stage 9): smoke, steam, flame
// and wake particle emitters kept in ten ordered lists and stepped by the effect sweep.
// Their particles are pictures, but their emitters draw the simulation thread's C runtime
// rand() when they emit and while their particles age, so they are simulated here exactly
// (counts, timing, the lists' 401-entry cap and the 1000-object pool). List 6 holds the nano
// streams (sim/nano_spray.h), which are stepped by their own module in that position.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/rng.h"

namespace sim {

class HeightMap;

enum class FxKind : std::uint8_t {
    Dots,    // a stream of flame dots from one point to another (teleport)
    Flame,   // flames along a line for a while (script effects 0 and 1, burning debris)
    Wake,    // jittered wake particles drifting along a line (script effects 2-5, water wake)
    Smoke,   // rising smoke puffs (impacts, shot trails, burning features and wrecks, muzzles)
    Steam,   // a vent's endless steam (geothermal features)
};

struct FxParticle {
    std::int32_t frame = 0, frame_count = 0;    // Dots, Flame: animation frame and its wrap
    std::int32_t frame_max = 0;                  // Smoke, Steam: the frame it disappears at
    std::int32_t hold_period = 0, hold_left = 0; // Smoke, Steam: steps per frame
    std::int32_t counter = 0, period = 0;        // Flame, Wake: steps per frame / colour change
    std::int32_t expire = 0;                     // Dots, Flame, Wake: last step
    std::array<std::int32_t, 3> pos{}, vel{};   // Wake (the sea test reads the position)
};

struct FxEmitter {
    FxKind kind = FxKind::Smoke;
    std::uint8_t list = 0;
    std::uint32_t deadline = 0;   // the step emitting ends (signed compares where the original's are)
    std::uint32_t next_emit = 0;
    std::int32_t period = 1;      // steps between emissions (Smoke, Steam) / particle parameter (Flame, Wake)
    std::int32_t frames = 0;      // Smoke, Steam: frames of the smoke sequence used (clamped)
    std::int32_t hold = 7;        // Smoke, Steam: steps per frame
    std::int32_t flight = 0;      // Dots: steps to the target
    std::array<std::int32_t, 3> origin{}, vel{};
    std::vector<FxParticle> particles;
};

struct EffectLists {
    static constexpr int kLists = 10;
    static constexpr int kPool = 1000;   // emitter objects shared by all lists (with the nano streams)
    static constexpr std::size_t kCap = 400;  // a list longer than this drops its oldest first
    std::array<std::vector<FxEmitter>, kLists> lists;
    bool blocked = false;                // the pool refuses objects (the original's blocked flag)
    // Frame counts of the sequences the particles use (from the effect art).
    std::int32_t smoke1_frames = 0, smoke2_frames = 0, flame_frames = 0;
    std::string fault;                   // set where the original would divide by zero

    int live() const;
    void reset();
};

// Spawners (the original's 0x471FD0 .. 0x472C50). `nano_streams`: objects the nano streams hold
// from the same pool. Each returns false when no object was available.
struct FxContext {
    EffectLists& fx;
    Lcg15& rand;
    std::uint32_t tick;
    std::int32_t nano_streams = 0;
    std::uint8_t sea_level = 0;
    const HeightMap* heights = nullptr;
    std::int32_t wind_x = 0, wind_z = 0, gravity = 0;  // particle drift (pictures only)
};
// Smoke puff (0x472810 smoke 1, 0x4728F0 smoke 2): one puff.
bool fx_smoke(FxContext& c, const std::array<std::int32_t, 3>& pos, int list, bool second_smoke = false);
// Smoke emitter (0x472630): a puff every `period` steps for `duration` steps.
bool fx_smoke_emitter(FxContext& c, const std::array<std::int32_t, 3>& pos, std::int32_t period, std::int32_t duration, int list);
// Weapon muzzle smoke (0x4729D0): one puff of at most 3 frames, 30 steps each.
bool fx_muzzle_smoke(FxContext& c, const std::array<std::int32_t, 3>& pos, int list);
// Vent steam (0x472C50): a puff every 5 steps forever.
bool fx_steam(FxContext& c, const std::array<std::int32_t, 3>& pos, int list);
// Flames from `from` towards `to` (0x472330): `param` steps per frame, for `duration` steps.
bool fx_flame(FxContext& c, const std::array<std::int32_t, 3>& from, const std::array<std::int32_t, 3>& to, std::int32_t param,
              std::int32_t duration, int list);
// Burning debris (0x472AB0): four rand() draws (a jittered point and the duration), then flames.
bool fx_flame_jittered(FxContext& c, const std::array<std::int32_t, 3>& pos, int list);
// Wake (0x472530 colours down, 0x472430 colours up): `size` steps, particles drift from `from`
// towards `to`.
bool fx_wake(FxContext& c, const std::array<std::int32_t, 3>& from, const std::array<std::int32_t, 3>& to, std::int32_t size,
             bool rising, int list);
// Flame dots to a point (0x471FD0, the teleport).
bool fx_dots(FxContext& c, const std::array<std::int32_t, 3>& from, const std::array<std::int32_t, 3>& to, std::int32_t duration, int list);

// The effect sweep (0x471EB0) of lists [first, last): a finished emitter is removed (in
// order), the others step their particles and emit when due.
void step_effect_lists(FxContext& c, int first, int last);

}  // namespace sim
