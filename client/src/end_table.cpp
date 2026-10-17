// The end-of-battle statistics table (spec: docs/RE_MEMORY.md, "Client", "End of a battle").

#include <algorithm>

#include "client/front_end.h"

namespace client {
namespace {

// The game's float -> int conversion (0x4E43A0): truncated to 64 bits, the low 32 kept (so
// 3e9 gives -1294967296); beyond the 64-bit range (or NaN) the 64-bit indefinite value,
// whose low half is 0.
std::int32_t trunc32(double v) {
    if (!(v > -9223372036854775808.0 && v < 9223372036854775808.0)) return 0;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(v))));
}

}  // namespace

EndTable end_table(const std::array<EndPlayer, 10>& players, std::uint32_t end_step, float kill_mul, float time_mul) {
    EndTable t;
    t.maxima = {10, 10, 100, 100, 100, 100, 100};
    for (std::size_t s = 0; s < players.size(); ++s) {
        const EndPlayer& p = players[s];
        const bool playing = p.active && p.controller >= 1 && p.controller <= 3 && p.state != 10 && !p.watching;
        if (!(playing || p.units_created != 0) || !p.present) continue;
        EndRow r;
        r.slot = static_cast<int>(s);
        r.name = p.name.substr(0, std::min<std::size_t>(p.name.size(), 29));
        r.colour = p.colour;
        r.values[0] = p.kills;
        r.values[1] = p.losses;
        r.values[2] = trunc32(p.energy_produced);
        r.values[3] = trunc32(p.metal_produced);
        r.values[4] = trunc32(p.energy_wasted);
        r.values[5] = trunc32(p.metal_wasted);
        // Score: the time part from whole minutes x 2 (steps / 60), then the kills part; the
        // sum wraps in 32 bits and is at least 0.
        const std::int32_t time_part = trunc32(static_cast<double>(end_step / 60u) * static_cast<double>(time_mul));
        const std::int32_t kill_part = trunc32(static_cast<double>(p.kills) * static_cast<double>(kill_mul));
        const std::int32_t score = static_cast<std::int32_t>(static_cast<std::uint32_t>(time_part) + static_cast<std::uint32_t>(kill_part));
        r.values[6] = score < 0 ? 0 : score;
        for (std::size_t c = 0; c < 7; ++c) t.maxima[c] = std::max(t.maxima[c], r.values[c]);
        t.rows.push_back(std::move(r));
    }
    return t;
}

}  // namespace client
