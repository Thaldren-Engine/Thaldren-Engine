// The unit state packets' plain forms (docs/RE_MEMORY.md, "Network and sync"): the bit
// layouts of the motion goal states and of a unit's whole state, and the order in which a
// whole state changes the receiving unit. sim::Battle uses them (net_units.cpp); the harness
// compares them with the original's routines.

#include "sim/bitstream.h"
#include "sim/motion.h"
#include "sim/net_units.h"
#include "sim/x87.h"

namespace sim {
namespace {

using i16 = std::int16_t;
using i32 = std::int32_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

constexpr u32 kDying = 0x4000u, kFractionChanged = 0x2000u, kResettle = 0x10000u;

}  // namespace

int unit_type_bits(std::size_t type_count) {
    int bits = 0;
    for (std::size_t n = type_count; n; n >>= 1) ++bits;
    return bits;
}

std::uint32_t encode_build_fraction(float f) {
    if (f == 0.0f) return 0;
    return static_cast<u32>(1 - x87::ftol32(static_cast<double>(f) * -254.0)) & 0xFFu;
}

double decode_build_fraction(std::uint32_t byte) { return static_cast<double>(byte) * static_cast<double>(0.003921569f); }

void write_point_goal_state(BitWriter& w, const NetPointGoal& p) {
    namespace pg = point_goal;
    w.put(p.flags, 8);
    if (p.flags & pg::kFollow) {
        w.put(static_cast<u32>(static_cast<i32>(p.piece)), 16);
        w.put(p.link_id, 16);
    }
    if (p.flags & pg::kRadius) w.put(static_cast<u32>(static_cast<i32>(p.radius)), 16);
    if (p.flags & pg::kAltitude) w.put(static_cast<u32>(static_cast<i32>(p.altitude)), 16);
    if (p.flags & pg::kHeading) w.put(static_cast<u32>(static_cast<i32>(p.heading)), 16);
    if (p.flags & pg::kGround)
        for (int k = 0; k < 3; ++k) w.put(static_cast<u32>(p.pos[static_cast<std::size_t>(k)]), 32);
}

void read_point_goal_state(BitReader& r, NetPointGoal& g) {
    namespace pg = point_goal;
    g.flags = static_cast<u16>(r.get(8));
    if (g.flags & pg::kFollow) {
        g.piece = static_cast<i16>(r.get(16));
        g.link_id = static_cast<u16>(r.get(16));
    } else {
        g.link_id = 0;
    }
    g.radius = (g.flags & pg::kRadius) ? static_cast<i16>(r.get(16)) : i16{0};
    g.altitude = (g.flags & pg::kAltitude) ? static_cast<i16>(r.get(16)) : i16{0};
    g.heading = (g.flags & pg::kHeading) ? static_cast<i16>(r.get(16)) : i16{0};
    if (g.flags & pg::kGround)
        for (int k = 0; k < 3; ++k) g.pos[static_cast<std::size_t>(k)] = static_cast<i32>(r.get(32));
}

void write_manoeuvre_goal_state(BitWriter& w, const NetManoeuvreGoal& m) {
    w.put(m.flags, 1);
    for (int k = 0; k < 3; ++k) w.put(static_cast<u32>(m.pos[static_cast<std::size_t>(k)]), 32);
    for (int k = 0; k < 3; ++k) w.put(static_cast<u32>(m.vel[static_cast<std::size_t>(k)]), 32);
    if (m.flags & 1) w.put(m.target_heading, 16);
}

void read_manoeuvre_goal_state(BitReader& r, NetManoeuvreGoal& g) {
    g.flags = static_cast<u16>(r.get(1));
    for (int k = 0; k < 3; ++k) g.pos[static_cast<std::size_t>(k)] = static_cast<i32>(r.get(32));
    for (int k = 0; k < 3; ++k) g.vel[static_cast<std::size_t>(k)] = static_cast<i32>(r.get(32));
    if (g.flags & 1) g.target_heading = static_cast<u16>(r.get(16));
}

void write_route_state(BitWriter& w, bool blocked, std::uint8_t flags, std::int32_t count, const std::uint32_t* points) {
    // A negative count is sent as its low two bits, with no waypoints.
    i32 n = 0;
    if (flags & route_flags::kHasRoute) n = count > 2 ? 3 : count;
    w.put_bit(blocked);
    w.put(static_cast<u32>(n), 2);
    for (i32 k = 0; k < n; ++k) {
        w.put(points[static_cast<std::size_t>(k)] & 0xFFFFu, 16);
        w.put(points[static_cast<std::size_t>(k)] >> 16, 16);
    }
}

std::uint8_t route_flags_after_send(std::uint8_t flags, std::uint8_t motion_flags) {
    return static_cast<u8>((flags & 0xF3) ^ (motion_flags & 4));
}

void read_route_state(BitReader& r, bool& blocked, std::int32_t& count, std::array<std::uint32_t, 3>& points) {
    blocked = r.get_bit();
    count = static_cast<i32>(r.get(2));
    for (i32 k = 0; k < count; ++k) {
        const u32 px = r.get(16), pz = r.get(16);
        points[static_cast<std::size_t>(k)] = (px & 0xFFFFu) | (pz << 16);
    }
}

void write_whole_state(BitWriter& w, int bits, const NetWholeState& s) {
    // 0x48B200.
    w.put(s.type, bits);
    if (s.type == 0) return;
    w.put(static_cast<u32>(static_cast<i32>(s.hit_points)), 16);
    w.put(encode_build_fraction(s.fraction_left), 8);
    w.put(s.activate, 8);
    w.put(s.layer & 3u, 2);
    if (s.carried) {
        w.put_bit(true);
        w.put(s.carrier_id, 15);
        w.put(static_cast<u32>(static_cast<i32>(s.piece)), 8);
        return;
    }
    w.put_bit(false);
    for (int k = 0; k < 3; ++k) w.put(static_cast<u32>(s.pos[static_cast<std::size_t>(k)]), 32);
    w.put(s.heading, 16);
    w.put(s.bank, 16);
    w.put(s.pitch, 16);
    if (s.has_motion) w.put(static_cast<u32>(s.speed), 32);
}

void apply_whole_state(BitReader& r, int bits, WholeStateTarget& u) {
    // 0x48B3F0.
    const i16 type = static_cast<i16>(r.get(bits));
    if (type == 0) {
        if (u.type() != 0) u.add_flags(kDying);
        return;
    }
    if (static_cast<i16>(u.type()) != type) u.make(static_cast<u16>(type));
    u.set_hit_points(static_cast<i16>(r.get(16)));
    const double f = decode_build_fraction(r.get(8));
    if (f != static_cast<double>(u.fraction_left())) {
        u.set_fraction_left(static_cast<float>(f));
        u.add_flags(kFractionChanged);
    }
    const u8 act = static_cast<u8>(r.get(8));
    u.activate(act, true);
    u.activate(static_cast<u8>(~act), false);
    const u32 layer = r.get(2);
    if (r.get_bit()) {
        const u16 carrier = static_cast<u16>(r.get(15));
        const u8 piece = static_cast<u8>(r.get_signed(8));
        u.transport(carrier, piece, static_cast<u8>(layer));
        return;
    }
    if (u.carried()) u.transport(0, 0xFF, static_cast<u8>(layer));
    std::array<i32, 3> pos{};
    for (int k = 0; k < 3; ++k) pos[static_cast<std::size_t>(k)] = static_cast<i32>(r.get(32));
    const u16 heading = static_cast<u16>(r.get(16)), bank = static_cast<u16>(r.get(16)), pitch = static_cast<u16>(r.get(16));
    if (u.same_cells(pos[0], pos[2], layer)) u.set_position(pos);
    else u.move_cells(pos, layer);
    u.add_flags(kResettle);
    u.set_angles(pitch, heading, bank);
    if (u.has_motion()) u.set_speed(static_cast<i32>(r.get(32)));
}

}  // namespace sim
