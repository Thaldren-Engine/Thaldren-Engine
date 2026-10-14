#pragma once
// Nano spray: the particle streams builders, repairers and reclaimers emit
// (see docs/RE_MEMORY.md, "Construction and reclaim").
//
// They look like presentation, but the original fills them on the simulation
// thread with the C runtime's rand(), the same stream wind, meteors and
// burning features draw from: every stream draws 30 values when it starts (in
// the unit pass, from the order behaviour) and 30 more on the next step's
// effect sweep (stage 9). How many streams exist and how long they live (their
// particles' flight times) therefore decide the rand() sequence. The engine
// keeps them exactly; the renderer only reads them.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/rng.h"

namespace sim {

struct NanoParticle {
    std::array<std::int32_t, 3> pos{};     // 16.16
    std::array<std::int32_t, 3> target{};  // where it flies to
    std::array<std::int32_t, 3> vel{};     // per step
    std::uint32_t flags = 0x100;
    std::uint32_t colour = 0;              // low nibble cycles 1..7: palette index 0xA0 + nibble
    std::int32_t expire = 0;               // last step it lives
};

struct NanoStream {
    std::int32_t deadline = 0;    // last step it emits
    std::uint32_t next_emit = 0;  // step of its next emission
    // Source and target boxes, already narrowed to their central band: min x, y, z, then sizes.
    std::array<std::int32_t, 6> from{}, to{};
    std::vector<NanoParticle> particles;
};

struct NanoSprayState {
    std::vector<NanoStream> streams;  // oldest first (the original's effect list 6)
    std::string fault;                // set where the original would fault
};

// Starts a stream from the box `from` to the box `to` ({min x, y, z, max x, y, z}, 16.16)
// lasting `duration` steps; it emits at once (30 rand() draws). The list keeps at most
// 401 streams: with more than 400 the oldest is dropped first.
void spawn_nano_stream(NanoSprayState& s, Lcg15& rand, std::uint32_t tick, const std::array<std::int32_t, 6>& from,
                       const std::array<std::int32_t, 6>& to, std::int32_t duration = 1);
// The streams' part of the effect sweep (stage 9): finished streams go, the others move
// their particles, drop the expired ones and emit again while due.
void step_nano_streams(NanoSprayState& s, Lcg15& rand, std::uint32_t tick);

// A box of one point.
inline std::array<std::int32_t, 6> point_box(const std::array<std::int32_t, 3>& p) { return {p[0], p[1], p[2], p[0], p[1], p[2]}; }

} // namespace sim
