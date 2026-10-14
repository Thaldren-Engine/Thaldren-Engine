#include "sim/nano_spray.h"

#include <cmath>

#include "sim/x87.h"

namespace sim {
namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

i32 wsub(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
i32 wadd(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
i32 wmul(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) * static_cast<u32>(b)); }

// A box {min, max} narrowed to the band 4/11 .. 7/11 of each axis: {min x, y, z, size x, y, z}.
std::array<i32, 6> narrow(const std::array<i32, 6>& b) {
    std::array<i32, 6> out{};
    for (int a = 0; a < 3; ++a) {
        const i32 lo = b[static_cast<std::size_t>(a)], d = wsub(b[static_cast<std::size_t>(a + 3)], lo);
        const i32 from = wadd(wmul(d, 4) / 11, lo), to = wadd(wmul(d, 7) / 11, lo);
        out[static_cast<std::size_t>(a)] = from;
        out[static_cast<std::size_t>(a + 3)] = wsub(to, from);
    }
    return out;
}

// A random point of a narrowed box: each axis takes rand() / 32768 of its size.
i32 sample(Lcg15& rand, i32 base, i32 size) {
    const i64 product = static_cast<i64>(static_cast<i32>(rand.next())) * size;
    return wadd(static_cast<i32>(product / 0x8000), base);
}

// One emission: five particles (each draws six values whether it is kept or not).
void emit(NanoSprayState& s, NanoStream& n, Lcg15& rand, u32 tick) {
    for (i32 i = 0; i < 5; ++i) {
        NanoParticle p;
        p.pos[0] = sample(rand, n.from[0], n.from[3]);
        p.pos[1] = sample(rand, n.from[1], n.from[4]);
        p.pos[2] = sample(rand, n.from[2], n.from[5]);
        p.target[0] = sample(rand, n.to[0], n.to[3]);
        p.target[1] = sample(rand, n.to[1], n.to[4]);
        p.target[2] = sample(rand, n.to[2], n.to[5]);
        const i32 dx = wsub(p.target[0], p.pos[0]), dy = wsub(p.target[1], p.pos[1]), dz = wsub(p.target[2], p.pos[2]);
        // Length at 53-bit precision, truncated; only its low 32 bits are kept (sign-extended).
        const double sq = (static_cast<double>(dx) * dx + static_cast<double>(dy) * dy) + static_cast<double>(dz) * dz;
        const i64 len = static_cast<i32>(x87::ftol32(std::sqrt(sq)));
        // Four world units per step: the step count is the whole part of (len << 16) / 0x40000.
        const i32 q = static_cast<i32>(static_cast<u32>(static_cast<std::uint64_t>((len * 65536) / 0x40000)));
        const auto steps = static_cast<std::int16_t>(static_cast<u32>(q) >> 16);
        if (steps == 0) continue;
        const i32 d = steps;
        auto divide = [&](i32 v) {
            if (v == INT32_MIN && d == -1) {
                s.fault = "nano particle velocity overflows";
                return v;
            }
            return v / d;
        };
        p.vel = {divide(dx), divide(dy), divide(dz)};
        p.flags = 0x100;
        p.colour = static_cast<u32>(i % 7 + 0xA1);
        p.expire = wadd(static_cast<i32>(tick), d);
        n.particles.push_back(p);
    }
    n.next_emit = tick + 1;
}

} // namespace

void spawn_nano_stream(NanoSprayState& s, Lcg15& rand, std::uint32_t tick, const std::array<std::int32_t, 6>& from,
                       const std::array<std::int32_t, 6>& to, std::int32_t duration) {
    NanoStream n;
    n.deadline = wadd(static_cast<i32>(tick), duration);
    n.from = narrow(from);
    n.to = narrow(to);
    emit(s, n, rand, tick);
    if (s.streams.size() > 400) s.streams.erase(s.streams.begin());
    s.streams.push_back(std::move(n));
}

void step_nano_streams(NanoSprayState& s, Lcg15& rand, std::uint32_t tick) {
    for (std::size_t k = 0; k < s.streams.size();) {
        NanoStream& n = s.streams[k];
        if (n.particles.empty()) {
            s.streams.erase(s.streams.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        for (std::size_t i = 0; i < n.particles.size();) {
            NanoParticle& p = n.particles[i];
            for (int a = 0; a < 3; ++a) p.pos[static_cast<std::size_t>(a)] = wadd(p.pos[static_cast<std::size_t>(a)], p.vel[static_cast<std::size_t>(a)]);
            auto c = static_cast<std::int16_t>((p.colour & 0xF) + 1);
            if (c > 7) c = 1;
            p.colour = (p.colour & 0xFFF0u) + static_cast<u32>(static_cast<i32>(c));
            if (p.expire < static_cast<i32>(tick)) n.particles.erase(n.particles.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        }
        if (static_cast<i32>(n.next_emit) <= n.deadline && n.next_emit <= tick) emit(s, n, rand, tick);
        ++k;
    }
}

} // namespace sim
