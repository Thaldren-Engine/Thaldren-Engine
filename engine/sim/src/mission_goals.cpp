// Mission goals and placed units (docs/RE_MEMORY.md, "Campaign").

#include "sim/mission_goals.h"

#include "sim/battle.h"

namespace sim {

namespace {

using i32 = std::int32_t;
using u32 = std::uint32_t;

constexpr int kPlayer = 0, kOpponent = 1;

int fold(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : static_cast<unsigned char>(c); }
bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (fold(a[i]) != fold(b[i])) return false;
    return true;
}

std::uint16_t type_by_name(const Battle& b, const std::string& name) {
    for (std::size_t k = 1; k < b.types.size(); ++k)
        if (!b.types[k].tick.name.empty() && same_name(b.types[k].tick.name, name)) return static_cast<std::uint16_t>(k);
    return 0;
}

const std::string& type_name(const Battle& b, const Unit& u) {
    static const std::string none;
    const std::uint16_t t = u.id < b.ticks.size() ? b.ticks[u.id].type_index : 0;
    return t < b.types.size() ? b.types[t].tick.name : none;
}

int owner_of(const Battle& b, const Unit& u) { return u.id < b.ticks.size() ? b.ticks[u.id].owner : 0; }

// The player's units with a type (the original walks its whole part of the unit table and
// skips empty entries); `f` returns false to stop.
template <class F> void walk(Battle& b, int player, F&& f) {
    const PlayerSlot& p = b.players[static_cast<std::size_t>(player)];
    for (int id = p.first_unit; id <= p.last_unit && id < static_cast<int>(b.units.size()); ++id) {
        if (id <= 0 || b.ticks[static_cast<std::size_t>(id)].type_index == 0) continue;
        if (!f(b.units[static_cast<std::size_t>(id)])) break;
    }
}

// A unit the player can use: selectable, finished, not just handed over, not carried (or
// carried by a transport that leaves it selectable).
bool usable(const Battle& b, const Unit& u) {
    return (u.flags & unit_tick_flags::kSelectable) && u.build_fraction_left == 0.0f && b.ticks[u.id].transfer_holdoff == 0 &&
           (!u.carrier || (u.carrier->flags & unit_tick_flags::kCarrierSelectable));
}

void met(Battle& b, MissionGoal& g) {
    g.satisfied = true;
    if (!g.celebrated) {
        b.outside_cue_goal_met();
        g.celebrated = true;
    }
}

i32 iabs(i32 v) { return v < 0 ? -v : v; }

bool check_win(Battle& b, MissionGoal& g) {
    switch (g.kind) {
    case GoalKind::DestroyAllUnits:
        if (b.players[kOpponent].alive_count == 0) {
            if (!g.celebrated) {
                b.outside_cue_goal_met();
                g.celebrated = true;
            }
            return true;
        }
        return false;
    case GoalKind::BuildUnitType:
        if (g.satisfied) return true;
        if (g.type == 0) g.type = type_by_name(b, g.type_name);
        walk(b, kPlayer, [&](Unit& u) {
            if (b.ticks[u.id].type_index == g.type && u.build_fraction_left == 0.0f) met(b, g);
            return !g.satisfied;
        });
        return g.satisfied;
    case GoalKind::MoveUnitToRadius: {
        if (!g.pos_converted) {
            g.pos = screen_to_terrain(b, g.pos[0], g.pos[2]);
            g.pos_converted = true;
        }
        std::vector<Unit*> near;
        units_in_radius(b.buckets, g.pos, g.radius, near);
        for (Unit* u : near) {
            if (owner_of(b, *u) != kPlayer) continue;
            if (!g.type_name.empty() && !same_name(g.type_name, type_name(b, *u))) continue;
            if (usable(b, *u)) met(b, g);
        }
        return g.satisfied;
    }
    case GoalKind::UnitTypeNearX:
    case GoalKind::UnitTypeNearZ:
        if (!g.satisfied)
            walk(b, kPlayer, [&](Unit& u) {
                if (!g.type_name.empty() && !same_name(g.type_name, type_name(b, u))) return !g.satisfied;
                const i32 c = g.kind == GoalKind::UnitTypeNearX ? u.cell_x : u.cell_z;
                if (iabs(c - g.line) < 3) met(b, g);
                return !g.satisfied;
            });
        return g.satisfied;
    case GoalKind::VictoryTimer:
        return b.game_tick() >= g.deadline;
    default:
        return g.satisfied;  // the event-driven goals
    }
}

bool check_loss(Battle& b, MissionGoal& g) {
    switch (g.kind) {
    case GoalKind::AllUnitsKilled:
        g.satisfied = true;
        walk(b, kPlayer, [&](Unit& u) {
            if (usable(b, u)) g.satisfied = false;
            return g.satisfied;
        });
        return g.satisfied;
    case GoalKind::DeathTimer:
        return b.game_tick() >= g.deadline;
    case GoalKind::AnyUnitNearX:
    case GoalKind::AnyUnitNearZ:
        if (!g.satisfied)
            walk(b, kOpponent, [&](Unit& u) {
                const i32 c = g.kind == GoalKind::AnyUnitNearX ? u.cell_x : u.cell_z;
                if (iabs(c - g.line) < 3) g.satisfied = true;
                return !g.satisfied;
            });
        return g.satisfied;
    default:
        return g.satisfied;
    }
}

}  // namespace

bool mission_won(Battle& b) {
    MissionGoals& m = b.mission_goals;
    if (!m.active) return false;
    if (m.win.empty()) m.win.push_back(MissionGoal{GoalKind::DestroyAllUnits});
    for (MissionGoal& g : m.win)
        if (!check_win(b, g)) return false;
    return true;
}

bool mission_lost(Battle& b) {
    MissionGoals& m = b.mission_goals;
    if (m.loss.empty()) m.loss.push_back(MissionGoal{GoalKind::AllUnitsKilled});
    for (MissionGoal& g : m.loss)
        if (check_loss(b, g)) return true;
    return false;
}

static bool is_win_goal(GoalKind k) { return static_cast<int>(k) <= static_cast<int>(GoalKind::VictoryTimer); }

bool mission_goal_check(Battle& b, MissionGoal& g) { return is_win_goal(g.kind) ? check_win(b, g) : check_loss(b, g); }

void mission_goal_unit_destroyed(Battle& b, MissionGoal& g, Unit& u) {
    const int owner = owner_of(b, u);
    const std::string& name = type_name(b, u);
    switch (g.kind) {
    case GoalKind::KillEnemyCommander:
        if (owner == kOpponent && same_name(name, b.players[static_cast<std::size_t>(owner)].commander_name)) met(b, g);
        break;
    case GoalKind::KillAllMobileUnits:
        if (owner == kOpponent && b.ticks[u.id].has_motion) {
            g.count = 0;
            walk(b, kOpponent, [&](Unit& o) {
                if (b.ticks[o.id].has_motion) ++g.count;
                return g.count < 2;
            });
            if (g.count < 2) met(b, g);
        }
        break;
    case GoalKind::KillAllOfType:
        if (!g.satisfied && owner == kOpponent && same_name(g.type_name, name)) {
            g.type = type_by_name(b, g.type_name);
            g.count = 0;
            walk(b, kOpponent, [&](Unit& o) {
                if (b.ticks[o.id].type_index == g.type) ++g.count;
                return g.count < 2;
            });
            if (g.count < 2) met(b, g);
        }
        break;
    case GoalKind::KillUnitType:
        if (g.count > 0 && owner == kOpponent && same_name(g.type_name, name)) {
            --g.count;
            if (g.count < 1) met(b, g);
        }
        break;
    case GoalKind::CommanderKilled:
        if (owner == kPlayer && same_name(name, b.players[static_cast<std::size_t>(owner)].commander_name)) g.satisfied = true;
        break;
    case GoalKind::UnitTypeKilled:
        if (same_name(g.type_name, name)) {
            --g.count;
            if (g.count < 1) g.satisfied = true;
        }
        break;
    case GoalKind::AllUnitsKilledOfType:
        if (same_name(g.type_name, name)) {
            g.type = type_by_name(b, g.type_name);
            g.count = 0;
            // Both walks run; each stops after the call that reaches two.
            auto count = [&](Unit& o) {
                if (b.ticks[o.id].type_index == g.type) ++g.count;
                return g.count < 2;
            };
            walk(b, kPlayer, count);
            walk(b, kOpponent, count);
            if (g.count < 2) g.satisfied = true;
        }
        break;
    default:
        break;
    }
}

void mission_goal_unit_transferred(Battle& b, MissionGoal& g, Unit& u) {
    if (g.kind == GoalKind::CaptureUnitType && owner_of(b, u) == kOpponent && same_name(g.type_name, type_name(b, u))) met(b, g);
}

void mission_unit_destroyed(Battle& b, Unit& u) {
    // 0x4904C0: every win goal, then every loss goal.
    for (MissionGoal& g : b.mission_goals.win) mission_goal_unit_destroyed(b, g, u);
    for (MissionGoal& g : b.mission_goals.loss) mission_goal_unit_destroyed(b, g, u);
}

void mission_unit_transferred(Battle& b, Unit& u) {
    for (MissionGoal& g : b.mission_goals.win) mission_goal_unit_transferred(b, g, u);
    for (MissionGoal& g : b.mission_goals.loss) mission_goal_unit_transferred(b, g, u);
}

std::array<i32, 3> screen_to_terrain(const Battle& b, i32 x, i32 y) {
    // 0x484B50: walk up the screen column from 8 cells below the row until the terrain's
    // screen row (z - height/2) is at or above the wanted row, then interpolate between that
    // point and the one 16 units further down.
    const i32 W = b.map.width * 16, H = b.map.height * 16;
    if (x < 0) x = 0;
    if (x >= W) x = W - 1;
    if (y < 0) y = 0;
    if (y >= H) y = H - 1;
    const HeightMap& hm = b.height_map();
    const i32 sea = b.settings.sea_level;
    auto ground = [&](i32 px, i32 pz) {
        const i32 s = hm.sample(static_cast<std::int16_t>(px >> 16), static_cast<std::int16_t>(pz >> 16));
        return sea < s ? s : sea;
    };
    const i32 px = static_cast<i32>(static_cast<u32>(x) << 16);
    i32 pz = static_cast<i32>(static_cast<u32>((y & ~15) + 0x80) << 16);
    i32 h = 0, row = 0;
    bool found = false;
    for (i32 k = 0x80; k >= 0; k -= 16) {
        h = ground(px, pz);
        row = static_cast<std::int16_t>(pz >> 16) - (static_cast<std::int16_t>(h) >> 1);
        if (row <= y) {
            found = true;
            break;
        }
        pz -= 0x100000;
    }
    if (!found) return {px, static_cast<i32>(static_cast<u32>(h) << 16), pz};
    const i32 pz2 = pz + 0x100000;
    const i32 h2 = ground(px, pz2);
    const i32 row2 = static_cast<std::int16_t>(pz2 >> 16) - (static_cast<std::int16_t>(h2) >> 1);
    if (row < row2 || y <= row2) {
        const i32 d = row2 - row;
        if (d == 0) {
            const_cast<Battle&>(b).mission_goals.faults++;  // the original divides by zero here
        } else {
            pz += static_cast<i32>(static_cast<u32>(y - row) * 0x100000u) / d;
        }
        return {px, static_cast<i32>(static_cast<u32>(ground(px, pz)) << 16), pz};
    }
    return {px, static_cast<i32>(static_cast<u32>(h) << 16), pz};
}

bool place_mission_units(Battle& b, const std::vector<MissionUnit>& list, const MissionOrderParser& parse, std::string& error) {
    std::vector<Unit*> placed(list.size(), nullptr);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const MissionUnit& m = list[i];
        if (m.type == 0 || m.type >= b.types.size()) continue;
        if (m.player < 0 || m.player >= 10) {
            error = "Player number " + std::to_string(m.player + 1) + " invalid for unit " + m.unit_name;
            return false;  // the original reports it and exits
        }
        const PlayerSlot& p = b.players[static_cast<std::size_t>(m.player)];
        if (!p.active || p.controller < 1 || p.controller > 3 || p.state == 10) {
            error = "Player number " + std::to_string(m.player + 1) + " invalid for unit " + m.unit_name;
            return false;
        }
        std::array<i32, 3> pos = m.pos;
        b.feature_world();  // brings the shared settings up to date
        snap_to_footprint(b.construction(), m.type, pos);
        Unit* u = b.create_unit(m.player, m.type, pos[0], pos[1], pos[2], true, 1);
        if (!u) continue;
        u->flags = (u->flags & ~0x8000u) | (m.immune ? 0x8000u : 0u);
        const i32 max = static_cast<i32>(b.types[m.type].info.max_damage);
        u->hit_points = static_cast<std::int16_t>(static_cast<u32>(max * static_cast<std::int16_t>(m.health_percent)) / 100u);
        u->heading = m.heading;
        placed[i] = u;
    }
    // Unit references: the first placed unit whose identifier, else unit name, matches.
    auto find = [&](const std::string& name) -> Unit* {
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (!placed[i]) continue;
            if (same_name(list[i].ident, name) || same_name(list[i].unit_name, name)) return placed[i];
        }
        return nullptr;
    };
    for (std::size_t i = 0; i < list.size(); ++i) {
        Unit* u = placed[i];
        if (!u || !list[i].has_orders || !parse) continue;
        const std::vector<MissionUnitOrder> orders = parse(list[i].order_text);
        bool any = false, end = false;
        b.economy();  // synchronise the shared settings before resolving intents
        Targeting& t = b.targeting();
        for (const MissionUnitOrder& o : orders) {
            using K = MissionUnitOrder::Kind;
            switch (o.kind) {
            case K::AttackAt:
            case K::Move:
            case K::Unload:
            case K::Patrol: {
                const std::uint8_t intent = o.kind == K::AttackAt ? 3 : o.kind == K::Move ? 2 : o.kind == K::Unload ? 5 : 9;
                const std::uint8_t kind = resolve_mission(t, intent, *u, nullptr, &o.pos);
                b.order(*u, kind, true, nullptr, &o.pos, o.kind == K::Patrol ? o.param : 0, 0);
                any = true;
                if (o.kind == K::AttackAt || o.kind == K::Patrol) end = true;
                break;
            }
            case K::AttackType:
                b.order(*u, mission::kAttackUnitType, true, nullptr, nullptr, o.type, 0);
                any = true;
                break;
            case K::Build:
                if (b.ticks[u->id].has_motion) b.order(*u, mission::kMobileBuild, true, nullptr, &o.pos, o.type, o.count);
                else b.order(*u, mission::kBuildingBuild, true, nullptr, nullptr, o.type, o.count);
                any = true;
                break;
            case K::BuildWeapon:
                b.order(*u, mission::kBuildWeapon, true, nullptr, nullptr, 0, o.count);
                break;
            case K::SelfDestruct:
                b.order(*u, mission::kSelfDestructFg, true, nullptr, nullptr, 1, 0);
                any = end = true;
                break;
            case K::Guard:
                if (Unit* target = find(o.ref)) {
                    const std::uint8_t kind = resolve_mission(t, 7, *u, target, nullptr);
                    b.order(*u, kind, true, target, nullptr, 0, 0);
                    any = true;
                }
                break;
            case K::LoadInto:
                if (Unit* carrier = find(o.ref)) b.transport_attach(*u, carrier, 0xFF, 0);
                break;
            case K::StandingOrders: {
                // Values not given keep the unit's current ones (the parser starts from them).
                const u32 move = o.count < 0 ? (u->flags >> 18) & 3u : static_cast<u32>(o.count) & 3u;
                const u32 fire = o.param < 0 ? (u->flags >> 20) & 3u : static_cast<u32>(o.param) & 3u;
                u->flags = (u->flags & 0xFFC3FFFFu) | ((fire << 2 | move) << 18);
                break;
            }
            case K::MakeSelectable:
                b.order(*u, mission::kMakeSelectable, true, nullptr, nullptr, 0, 0);
                any = end = true;
                break;
            case K::WaitForAttack: {
                Unit* target = o.ref_given ? find(o.ref) : nullptr;
                b.order(*u, mission::kWaitForAttack, true, target ? target : u, nullptr, 0, 0);
                any = true;
                break;
            }
            case K::Wait:
                b.order(*u, mission::kWait, true, nullptr, nullptr, o.param, o.count);
                any = true;
                break;
            }
        }
        if (any) {
            u->flags &= ~unit_tick_flags::kSelectable;
            if (!end) b.order(*u, mission::kMakeSelectable, true, nullptr, nullptr, 0, 0);
        }
    }
    if (list.empty()) b.mission_goals.active = false;
    return true;
}

}  // namespace sim
