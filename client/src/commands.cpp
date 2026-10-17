// Adapter between the client and the simulation's orders (see commands.h). The
// command rules follow the original's command routine as specified in
// docs/RE_MEMORY.md, "Client" (commands to a selection).

#include "client/commands.h"

#include <cmath>

#include "sim/construction.h"
#include "sim/missions.h"
#include "sim/orders.h"
#include "sim/targeting.h"
#include "sim/unit_tick.h"

// With the game-flow agent's command layer and Battle::step present, the adapter uses them;
// otherwise it carries its own copy of the rules (same spec).
#if __has_include("sim/commands.h")
#define CLIENT_GAME_FLOW 1
#include "sim/commands.h"
#else
#define CLIENT_GAME_FLOW 0
#endif

namespace client {
namespace {

bool selected_of(const sim::Battle& b, std::size_t i, int player) {
    return b.ticks[i].type_index != 0 && b.ticks[i].owner == player && (b.units[i].flags & sim::unit_tick_flags::kSelected);
}

#if !CLIENT_GAME_FLOW
// Abilities bits 0 and 1 of the unit type: whether standing move / fire orders apply.
constexpr std::uint32_t kAbilityMoveOrders = 1, kAbilityFireOrders = 2;

bool kind_allowed(const sim::Battle& b, const sim::Unit& u, std::uint8_t kind) {
    const std::uint16_t t = b.ticks[u.id].type_index;
    const std::uint32_t ab = t < b.types.size() ? b.types[t].mission.abilities : 0;
    if (kind == sim::mission::kStandingFireOrder && !(ab & kAbilityFireOrders)) return false;
    if (kind == sim::mission::kStandingMoveOrder && !(ab & kAbilityMoveOrders)) return false;
    return true;
}

// A queued command repeating one the unit already has (same kind, same target when one is
// given, position within 16 world units when one is given) cancels that order instead.
bool cancel_match(sim::Battle& b, sim::Unit& u, std::uint8_t kind, const sim::Unit* target, const WorldPos* pos) {
    sim::MissionOrder* const head = u.orders;
    for (sim::MissionOrder* o = u.orders; o; o = o->next) {
        if (o->kind != kind) continue;
        if (target && o->target.unit != target) continue;
        if (pos && (static_cast<std::uint32_t>((*pos)[0] - o->pos[0]) + 0x100000u >= 0x200001u ||
                    static_cast<std::uint32_t>((*pos)[2] - o->pos[2]) + 0x100000u >= 0x200001u))
            continue;
        sim::MissionOrder** link = (o->flags & sim::order_flags::kSecondary) ? &u.secondary_orders : &u.orders;
        while (*link && *link != o) link = &(*link)->next;
        if (!*link) return true;  // listed elsewhere: nothing happens
        *link = o->next;
        if (o != head) o->flags |= sim::order_flags::kNotCurrent;
        sim::destroy_order(b.order_context(), o);
        return true;
    }
    return false;
}
#endif  // !CLIENT_GAME_FLOW

} // namespace

#if !CLIENT_GAME_FLOW
int command_selected(sim::Battle& b, int player, std::uint8_t intent, sim::Unit* target, const WorldPos* pos, bool queued) {
    // Centre of the selection (whole world units), the target unit excluded.
    std::int64_t sx = 0, sz = 0;
    int count = 0;
    for (std::size_t i = 1; i < b.units.size(); ++i) {
        if (!selected_of(b, i, player) || &b.units[i] == target) continue;
        ++count;
        sx += static_cast<std::int16_t>(static_cast<std::uint32_t>(b.units[i].pos[0]) >> 16);
        sz += static_cast<std::int16_t>(static_cast<std::uint32_t>(b.units[i].pos[2]) >> 16);
    }
    if (count == 0) return 0;
    const std::int32_t cx = static_cast<std::int32_t>(sx / count) * 65536;
    const std::int32_t cz = static_cast<std::int32_t>(sz / count) * 65536;
    const std::int64_t spread = static_cast<std::int64_t>(count) * 3000;
    int given = 0;
    for (std::size_t i = 1; i < b.units.size(); ++i) {
        if (!selected_of(b, i, player) || &b.units[i] == target) continue;
        sim::Unit& u = b.units[i];
        const std::uint8_t kind = sim::resolve_mission(b.targeting(), intent, u, target, pos);
        if (kind == 0 || !kind_allowed(b, u, kind)) continue;
        WorldPos at{};
        const WorldPos* use = pos;
        if (pos && (sim::mission::default_flags()[kind] & 2u)) {
            // Units close to their centre keep their places around the destination.
            const std::int64_t dx = static_cast<std::int64_t>(u.pos[0]) - cx, dz = static_cast<std::int64_t>(u.pos[2]) - cz;
            if (((dx * dx) >> 32) + ((dz * dz) >> 32) <= spread) {
                at = {(*pos)[0] + u.pos[0] - cx, (*pos)[1], (*pos)[2] + u.pos[2] - cz};
                use = &at;
            }
        }
        if (queued && cancel_match(b, u, kind, target, use)) {
            ++given;
            continue;
        }
        if (b.order(u, kind, queued, target, use)) ++given;
    }
    return given;
}

int command_selected_kind(sim::Battle& b, int player, std::uint8_t kind, std::int32_t param, bool queued) {
    int given = 0;
    for (std::size_t i = 1; i < b.units.size(); ++i) {
        if (!selected_of(b, i, player)) continue;
        sim::Unit& u = b.units[i];
        if (!kind_allowed(b, u, kind)) continue;
        if (b.order(u, kind, queued, nullptr, nullptr, param)) ++given;
    }
    return given;
}

#endif  // !CLIENT_GAME_FLOW

void change_build_count(sim::Battle& b, sim::Unit& factory, std::uint16_t type, std::int32_t delta) {
    b.order_build_count(factory, type, delta);
}

std::int32_t build_count(const sim::Unit& factory, std::uint16_t type) {
    std::int32_t n = 0;
    for (const sim::MissionOrder* o = factory.orders; o; o = o->next)
        if ((o->kind == sim::mission::kBuildingBuild || o->kind == sim::mission::kMobileBuild) && o->param0 == type && o->param1 > 0)
            n += o->param1;
    for (const sim::MissionOrder* o = factory.secondary_orders; o; o = o->next)
        if (o->kind == sim::mission::kBuildingBuild && o->param0 == type && o->param1 > 0) n += o->param1;
    return n;
}

#if !CLIENT_GAME_FLOW
int command_build(sim::Battle& b, int player, std::uint16_t type, const WorldPos& pos, bool queued) {
    int given = 0;
    for (std::size_t i = 1; i < b.units.size(); ++i) {
        if (!selected_of(b, i, player)) continue;
        sim::Unit& u = b.units[i];
        if (!b.ticks[i].has_motion) continue;
        const std::uint16_t t = b.ticks[i].type_index;
        if (t >= b.types.size() || !b.types[t].construction.has_build_options) continue;
        if (queued && cancel_match(b, u, sim::mission::kMobileBuild, nullptr, &pos)) {
            ++given;
            continue;
        }
        if (b.order_mobile_build(u, type, pos, queued)) ++given;
    }
    return given;
}

#endif  // !CLIENT_GAME_FLOW

bool structure_fits(sim::Battle& b, std::uint16_t type, int cell_x, int cell_z) {
    return sim::structure_fits(b.construction(), type, static_cast<std::int16_t>(cell_x), static_cast<std::int16_t>(cell_z));
}

#if CLIENT_GAME_FLOW
namespace {
int count_selected(const sim::Battle& b, int player, const sim::Unit* except) {
    int n = 0;
    for (std::size_t i = 1; i < b.units.size(); ++i) n += selected_of(b, i, player) && &b.units[i] != except;
    return n;
}
}  // namespace

int command_selected(sim::Battle& b, int player, std::uint8_t intent, sim::Unit* target, const WorldPos* pos, bool queued) {
    const int n = count_selected(b, player, target);
    sim::command::issue_to_selected(b, player, intent, 0, queued, target, pos, 0, 0);
    return n;
}

int command_selected_kind(sim::Battle& b, int player, std::uint8_t kind, std::int32_t param, bool queued) {
    const int n = count_selected(b, player, nullptr);
    sim::command::issue_to_selected(b, player, 0, kind, queued, nullptr, nullptr, param, 0);
    return n;
}

int command_build(sim::Battle& b, int player, std::uint16_t type, const WorldPos& pos, bool queued) {
    // Every selected unit of the player whose type is a builder (type flag 0x40) gets the build
    // order: VTOL_MobileBuild for flying types (0x800), else MobileBuild (as the placement click
    // gives it, 0x419670).
    int given = 0;
    for (std::size_t i = 1; i < b.units.size(); ++i) {
        if (!selected_of(b, i, player)) continue;
        const sim::UnitTickTypeInfo* tt = b.ticks[i].tick_type;
        if (!tt || !(tt->type_flags & sim::mission_type_flags::kBuilder)) continue;
        const std::uint8_t kind = (tt->type_flags & sim::mission_type_flags::kCanFly) ? sim::mission::kAirMobileBuild : sim::mission::kMobileBuild;
        sim::command::issue_or_cancel(b, kind, queued, b.units[i], nullptr, &pos, type, 0);
        ++given;
    }
    return given;
}

void step_battle(sim::Battle& b) { b.step(); }
void end_frame(sim::Battle& b) { b.end_frame(); }
#else
void step_battle(sim::Battle& b) {
    b.advance_tick();
    b.step_units_and_projectiles();
    b.step_effects();    // stage 5
    b.players_stage();   // stage 6
    b.step_features();   // stage 7
    b.step_wind();       // stage 8
    b.step_nano();       // stage 9 (the nano streams' part of the effect sweep)
}
void end_frame(sim::Battle& b) { b.expire_lingering_sight(); }
#endif

} // namespace client
