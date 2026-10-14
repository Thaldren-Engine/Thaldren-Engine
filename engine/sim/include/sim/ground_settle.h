#pragma once
// Ground settle: after its motion step a unit's height (and for most ground
// units its pitch and bank) is fitted to the terrain or the water surface
// (see docs/RE_MEMORY.md, "Ground settle").

#include <array>
#include <cstdint>

#include "sim/heightmap.h"
#include "sim/unit.h"

namespace sim {

// Unit type data ground settle needs.
struct GroundSettleType {
    // bit 12: stays at or above the water surface (hovers),
    // bit 19: held at a fixed depth below the water surface (waterline),
    // bit 20: follows the terrain height without tilting.
    std::uint32_t type_flags = 0;
    std::uint8_t waterline = 0;
    std::int32_t max_velocity = 0;  // 16.16
    // The four corners of the model's footprint shape (x, z of the first four
    // vertices of its selection primitive), if the model has one.
    bool has_footprint = false;
    std::array<std::array<std::int32_t, 2>, 4> footprint{};
};

namespace settle_flags {
constexpr std::uint32_t kResettle = 0x10000;  // unit flags: settle at the next step
constexpr std::uint32_t kLayerMask = 0x3, kLayerGround = 0x1;
constexpr std::uint32_t kTypeHover = 0x1000, kTypeWaterline = 0x80000, kTypeTerrainOnly = 0x100000;
}  // namespace settle_flags

// What ground settle reads from outside the unit.
class GroundSettleSystems {
public:
    virtual ~GroundSettleSystems() = default;
    // The motion object's current speed (16.16) and the step its current
    // movement started; read only for hovering units.
    virtual std::int32_t motion_speed(Unit& unit) = 0;
    virtual std::uint32_t motion_start_tick(Unit& unit) = 0;
    // Real-time clock in simulation steps (milliseconds × tick rate / 1000). The
    // original reads the wall clock here, so the hover bob is not deterministic;
    // the value is an input to the simulation.
    virtual std::uint32_t clock_ticks() = 0;
};

// The return addresses the original's settle routine leaves in the unit pass's
// stale stack word (the last call it made).
namespace settle_stale {
constexpr std::uint32_t kAfterWaterSample = 0x0048A8FF, kAfterLandSample = 0x0048A909, kAfterTerrainSample = 0x0048A92C,
                        kAfterSlopeFit = 0x0048A96F;
}

// Settles `unit` (called after its motion step). `has_motion`: the unit has a
// motion object. Updates `stale_word` like the original (unchanged when it made no call).
void settle_on_ground(Unit& unit, bool has_motion, const GroundSettleType& type, std::uint8_t sea_level, std::uint32_t game_tick,
                      const HeightMap& map, GroundSettleSystems& systems, std::uint32_t& stale_word);

// The slope fit on its own: samples the terrain under the four footprint
// corners (rotated by the heading), sets the integer part of the height to
// their average and the pitch and bank to the slopes. Hovering units are held
// at the water surface and bob.
void fit_to_slope(Unit& unit, const GroundSettleType& type, std::uint8_t sea_level, std::uint32_t game_tick, const HeightMap& map,
                  GroundSettleSystems& systems);

} // namespace sim
