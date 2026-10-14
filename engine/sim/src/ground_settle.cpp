#include "sim/ground_settle.h"

#include "sim/trig.h"
#include "sim/x87_math.h"
#include "sim/x87_trig.h"

namespace sim {
namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

std::int16_t whole(i32 v) { return static_cast<std::int16_t>(v >> 16); }

i32 add32(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
i32 sub32(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
i32 half_sum(i32 a, i32 b) { return add32(a, b) / 2; }

std::int32_t sample_at(const HeightMap& map, const Unit& u) { return map.sample(whole(u.pos[0]), whole(u.pos[2])); }

// |v| >> 16 as the original computes it (the absolute value of INT_MIN stays negative).
i32 abs_whole(i32 v) {
    const u32 s = static_cast<u32>(v >> 31);
    return static_cast<std::int16_t>(static_cast<i32>((static_cast<u32>(v) ^ s) - s) >> 16);
}

}  // namespace

void fit_to_slope(Unit& u, const GroundSettleType& type, std::uint8_t sea_level, std::uint32_t game_tick, const HeightMap& map,
                  GroundSettleSystems& systems) {
    if (!type.has_footprint) return;
    std::array<i32, 4> height{};
    for (int i = 0; i < 4; ++i) {
        i32 x = type.footprint[static_cast<std::size_t>(i)][0], z = type.footprint[static_cast<std::size_t>(i)][1];
        x87::rotate_point_2d(static_cast<std::int16_t>(u.heading), x, z);
        const i32 wx = static_cast<std::int16_t>(add32(x, u.pos[0]) >> 16);
        const i32 wz = static_cast<std::int16_t>(sub32(u.pos[2], z) >> 16);
        // Unsigned cell test: a corner off the map (or left of it) abandons the fit.
        const u32 cx = static_cast<u32>(wx) >> 4, cz = static_cast<u32>(wz) >> 4;
        if (cx >= static_cast<u32>(map.width() - 1) || cz >= static_cast<u32>(map.height() - 1)) return;
        const i32 fx = wx & 15, fz = wz & 15;
        const auto ix = static_cast<i32>(cx), iz = static_cast<i32>(cz);
        const i32 h00 = map.at(ix, iz), h10 = map.at(ix + 1, iz), h01 = map.at(ix, iz + 1), h11 = map.at(ix + 1, iz + 1);
        const i32 top = (h10 - h00) * fx / 16 + h00;
        const i32 bottom = (h11 - h01) * fx / 16 + h01;
        const bool hovering = (type.type_flags & settle_flags::kTypeHover) && (u.flags & unit_flags::kAlive) && !(u.flags & 0x4000u);
        if (!hovering) {
            height[static_cast<std::size_t>(i)] = (bottom - top) * fz / 16 + top;
            continue;
        }
        // Hovering: at least the water surface, plus a bob that fades with speed
        // and with the time since the current movement started.
        i32 h = (bottom - top) * fz / 16 + top;
        if (h <= static_cast<i32>(sea_level)) h = sea_level;
        const u32 clock = systems.clock_ticks();
        const auto angle = static_cast<Angle>((((clock & 0x1F) + static_cast<u32>(i) * 8) << 11) + u.hover_phase);
        const i32 half = type.max_velocity / 2;
        i32 speed = systems.motion_speed(u);
        if (speed >= half) speed = half;
        // The original divides by zero here for a type without speed.
        const i32 ratio = half ? static_cast<i32>((static_cast<i64>(speed) << 16) / half) : 0;
        const i32 amplitude = sub32(2, static_cast<i32>((static_cast<i64>(ratio) * 2) >> 16));
        u32 since = game_tick - systems.motion_start_tick(u);
        if (since >= 60) since = 60;
        const i32 fade = static_cast<i32>(static_cast<u32>(amplitude) * since / 60u);
        height[static_cast<std::size_t>(i)] = add32(trig::mul_cos(angle, sub32(amplitude, fade)), h);
    }
    const i32 front = half_sum(height[0], height[1]), back = half_sum(height[2], height[3]);
    // Only the integer part of the height changes.
    u.pos[1] = static_cast<i32>((static_cast<u32>(u.pos[1]) & 0xFFFFu) | (static_cast<u32>(half_sum(back, front)) << 16));
    const auto& f = type.footprint;
    u.bank = static_cast<std::int16_t>(x87::atan2_scaled(sub32(back, front), abs_whole(sub32(f[0][1], f[3][1]))));
    u.pitch = static_cast<std::int16_t>(x87::atan2_scaled(sub32(height[0], height[1]), abs_whole(sub32(f[0][0], f[1][0]))));
}

void settle_on_ground(Unit& u, bool has_motion, const GroundSettleType& type, std::uint8_t sea_level, std::uint32_t game_tick,
                      const HeightMap& map, GroundSettleSystems& systems, std::uint32_t& stale_word) {
    const u32 flags = u.flags;
    if (!(flags & settle_flags::kResettle) && !(type.type_flags & settle_flags::kTypeHover)) return;
    u.flags = flags & ~settle_flags::kResettle;
    if (!has_motion || (flags & settle_flags::kLayerMask) != settle_flags::kLayerGround) return;
    if (type.type_flags & settle_flags::kTypeTerrainOnly) {
        if (!(type.type_flags & settle_flags::kTypeHover)) {
            stale_word = settle_stale::kAfterTerrainSample;
            u.pos[1] = static_cast<i32>(static_cast<u32>(sample_at(map, u)) << 16);
            return;
        }
        // Hovering: never below the water surface (less the waterline).
        const i32 surface = static_cast<i32>(sea_level) - static_cast<i32>(type.waterline);
        stale_word = settle_stale::kAfterWaterSample;
        if (sample_at(map, u) > surface) {
            stale_word = settle_stale::kAfterLandSample;
            u.pos[1] = static_cast<i32>(static_cast<u32>(sample_at(map, u)) << 16);
        } else {
            u.pos[1] = static_cast<i32>(static_cast<u32>(surface) << 16);
        }
        return;
    }
    if (type.type_flags & settle_flags::kTypeWaterline) {
        // (waterline × 0xFFFF + sea level) << 16 wraps to (sea level − waterline) << 16.
        u.pos[1] = static_cast<i32>((static_cast<u32>(type.waterline) * 0xFFFFu + sea_level) << 16);
        return;
    }
    stale_word = settle_stale::kAfterSlopeFit;
    fit_to_slope(u, type, sea_level, game_tick, map, systems);
}

} // namespace sim
