// The user interface's order routine on a unit range (docs/RE_MEMORY.md, "Commands"); kept
// apart from the battle so the differential harness can run it alone.

#include <cstdint>

#include "sim/commands.h"
#include "sim/orders.h"
#include "sim/targeting.h"
#include "sim/unit.h"

namespace sim::command {

namespace {
std::uint32_t table_flags(std::uint8_t kind) { return kind < mission::kCount ? mission::default_flags()[kind] : 0; }
}  // namespace

void issue_to_selected(SelectedOrderSystems& sys, Unit* first, Unit* last, std::uint8_t intent, std::uint8_t mission, bool queued,
                       Unit* hover, const std::array<std::int32_t, 3>* pos, const std::array<std::int32_t, 3>& cursor, std::int32_t param0,
                       std::int32_t param1) {
    // 0x48CF30. The unit under the cursor is the target (and is left out) when the intent
    // takes one (all but unload, stop and build) or the mission's table flags say so.
    using u32 = std::uint32_t;
    bool uses_hover;
    if (intent) uses_hover = intent != intent::kUnload && intent != intent::kStop && intent != intent::kBuild;
    else uses_hover = (table_flags(mission) >> 9) & 1u;
    Unit* target = uses_hover ? hover : nullptr;
    if (!first || !last) return;
    // The selection's centre in whole units.
    std::int32_t count = 0, sx = 0, sz = 0;
    for (Unit* u = first; u <= last; ++u) {
        if (!(u->flags & kSelectedFlag) || u == target) continue;
        ++count;
        sx = static_cast<std::int32_t>(static_cast<u32>(sx) + static_cast<u32>(static_cast<std::int16_t>(u->pos[0] >> 16)));
        sz = static_cast<std::int32_t>(static_cast<u32>(sz) + static_cast<u32>(static_cast<std::int16_t>(u->pos[2] >> 16)));
    }
    if (count == 0) return;
    const std::int32_t cx = static_cast<std::int32_t>(static_cast<u32>(sx / count) << 16);
    const std::int32_t cz = static_cast<std::int32_t>(static_cast<u32>(sz / count) << 16);
    const std::int32_t radius = static_cast<std::int32_t>(static_cast<u32>(count) * 3000u);
    for (Unit* u = first; u <= last; ++u) {
        if (!(u->flags & kSelectedFlag) || u == target) continue;
        std::uint8_t kind = mission;
        if (intent) kind = sys.resolve(intent, *u, target, cursor);
        if (kind == 0) continue;
        const std::uint32_t can_do = sys.abilities(*u);
        if (kind == mission::kStandingFireOrder && !(can_do & 0x2u)) continue;
        if (kind == mission::kStandingMoveOrder && !(can_do & 0x1u)) continue;
        // Point orders: units close to the centre keep their place in the group.
        std::array<std::int32_t, 3> spread;
        const std::array<std::int32_t, 3>* at = pos;
        if (pos && (table_flags(kind) & 2u)) {
            const std::int64_t dx = static_cast<std::int32_t>(static_cast<u32>(u->pos[0]) - static_cast<u32>(cx));
            const std::int64_t dz = static_cast<std::int32_t>(static_cast<u32>(u->pos[2]) - static_cast<u32>(cz));
            const u32 d = static_cast<u32>(static_cast<std::int32_t>((dx * dx) >> 32)) + static_cast<u32>(static_cast<std::int32_t>((dz * dz) >> 32));
            if (static_cast<std::int32_t>(d) <= radius) {
                spread = {static_cast<std::int32_t>(static_cast<u32>((*pos)[0]) + static_cast<u32>(u->pos[0]) - static_cast<u32>(cx)), (*pos)[1],
                          static_cast<std::int32_t>(static_cast<u32>((*pos)[2]) + static_cast<u32>(u->pos[2]) - static_cast<u32>(cz))};
                at = &spread;
            }
        }
        sys.issue(kind, queued, *u, target, at, param0, param1);
    }
}

}  // namespace sim::command
