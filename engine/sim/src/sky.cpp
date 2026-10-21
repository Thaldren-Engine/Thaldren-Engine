// Meteor showers and the camera shake (docs/RE_MEMORY.md, "Battle step").

#include "sim/sky.h"

#include "sim/trig.h"
#include "sim/x87.h"

namespace sim {

namespace {
// r * range / 0x8000 with a signed 64-bit product, truncating toward zero.
std::int32_t scaled(std::int64_t r, std::int64_t range) { return static_cast<std::int32_t>(r * range / 0x8000); }
std::int32_t wadd(std::int32_t a, std::int32_t b) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b)); }
std::int32_t wsub(std::int32_t a, std::int32_t b) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b)); }
}  // namespace

void MeteorShower::configure(std::int32_t radius_units, float density, float duration_seconds, float interval_seconds) {
    radius = radius_units;
    spawn_interval = x87::ftol32(30.0 / static_cast<double>(density));
    duration = x87::ftol32(static_cast<double>(duration_seconds) * 30.0);
    schedule_gap = x87::ftol32(static_cast<double>(interval_seconds) * 30.0);
}

void MeteorShower::reset(const WeaponType* meteor_weapon) {
    active = false;
    next_schedule = static_cast<std::uint32_t>(spawn_interval);
    weapon = meteor_weapon;
}

bool step_meteors(MeteorShower& m, std::uint32_t tick, std::int32_t map_width, std::int32_t map_height, Lcg15& rand,
                  std::int32_t (&pos)[3], std::int32_t (&vel)[3]) {
    if (m.next_schedule <= tick) {
        // A new shower: origin anywhere on the map, aim a little up and to either side.
        m.active = true;
        m.end_tick = static_cast<std::uint32_t>(m.duration) + tick;
        m.next_schedule = static_cast<std::uint32_t>(m.schedule_gap) + m.end_tick;
        m.next_spawn = tick;
        m.origin_z = static_cast<std::int16_t>(scaled(rand.next(), map_height));
        m.origin_x = static_cast<std::int16_t>(scaled(rand.next(), map_width));
        const std::int32_t dz = scaled(rand.next(), 10) - 15;
        const std::int32_t dx = scaled(rand.next(), 30) - 15;
        m.aim_z = static_cast<std::int16_t>(dz + m.origin_z);
        m.aim_x = static_cast<std::int16_t>(dx + m.origin_x);
        if (!m.enabled) m.active = false;
    }
    if (!m.active) return false;
    bool drop = false;
    if (m.next_spawn <= tick) {
        m.next_spawn = static_cast<std::uint32_t>(m.spawn_interval) + tick;
        // Falls for 90 steps from origin to aim (cells of 16 units, 16.16), 15 units a step down.
        vel[0] = static_cast<std::int32_t>(static_cast<std::uint32_t>(m.origin_x - m.aim_x) << 20) / 90;
        vel[1] = -0xF0000;
        vel[2] = static_cast<std::int32_t>(static_cast<std::uint32_t>(m.origin_z - m.aim_z) << 20) / 90;
        const std::int32_t mag = static_cast<std::int32_t>(static_cast<std::uint32_t>(scaled(rand.next(), m.radius)) << 16);
        const auto heading = static_cast<Angle>(scaled(rand.next(), 0x10000));
        const std::int32_t s = trig::mul_sin(heading, mag);
        const std::int32_t c = trig::mul_cos(heading, mag);
        pos[0] = wsub(static_cast<std::int32_t>(static_cast<std::uint32_t>(m.aim_x) << 20), s);
        pos[1] = vel[1] * -90;
        pos[2] = wsub(static_cast<std::int32_t>(static_cast<std::uint32_t>(m.aim_z) << 20), c);
        drop = true;
    }
    if (m.end_tick <= tick) m.active = false;
    return drop;
}

void add_camera_shake(CameraShake& s, std::int32_t dx, std::int32_t dy, std::int32_t magnitude, bool disabled) {
    if (disabled) return;
    if (!s.shaking) {
        s.accum_x = 0;
        s.accum_y = 0;
    }
    s.magnitude = wadd(magnitude, s.magnitude) / 2;  // C division (cdq; sub; sar)
    s.ticks_left = s.magnitude;
    s.accum_x = wadd(s.accum_x, dx);  // 0x41C6B2: 32-bit adds that wrap
    s.accum_y = wadd(s.accum_y, dy);
    if (s.magnitude > 0) s.shaking = true;
}

void step_camera_shake(CameraShake& s, Lcg15& rand) {
    s.offset_x = s.offset_y = 0;
    if (!s.shaking) return;
    if (s.ticks_left <= 0) {
        s.shaking = false;
        return;
    }
    // Amplitudes fall off with the steps left (magnitude > 0 whenever steps are left).
    const std::int32_t ax = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.accum_x) * static_cast<std::uint32_t>(s.ticks_left)) / s.magnitude;
    const std::int32_t ay = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.accum_y) * static_cast<std::uint32_t>(s.ticks_left)) / s.magnitude;
    const std::int32_t rx = scaled(rand.next(), ax);
    const std::int32_t ry = scaled(rand.next(), ay);
    s.offset_x = wsub(rx, ax / 2);
    s.offset_y = wsub(ry, ay / 2);
    --s.ticks_left;
}

}  // namespace sim
