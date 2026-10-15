// Mission goals against the original: the win and loss goal objects (their checks and their
// unit-destroyed / unit-transferred handlers, 0x48EA00-0x48FD70) on a synthetic unit table,
// and the editor-position-to-terrain conversion (0x484B50) on a random heightmap. Compared
// after every call: the result, satisfied / cue flags, counts and resolved types.
// MoveUnitToRadius's circle walk (0x47E890) is verified elsewhere (los_harness) and is not
// run here; its position conversion is.
//
// Usage: goal_harness <TotalA.exe> [scenarios] [operations] [first seed]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include "remote.h"
#include "sim/battle.h"
#include "sim/battle_quiet.h"
#include "sim/mission_goals.h"

namespace {

using harness::u32;
using i32 = std::int32_t;

constexpr u32 kGameStatePtr = 0x00511DE8, kSound = 0x0047F1A0, kScreenToTerrain = 0x00484B50;
constexpr u32 gPlayers = 0x1B63, gPlayerSize = 0x14B, gTick = 0x38A47, gDefs = 0x1439B, gDefCount = 0x1438F, gSides = 0x37F5F,
              gWorldW = 0x14223, gWorldH = 0x14227, gMapW = 0x14233, gMapH = 0x14237, gSea = 0x1427F, gCells = 0x14287;
constexpr u32 kUnitSize = 0x118, kDefSize = 0x249;

using GK = sim::GoalKind;
// Vtables of the goal classes and their unit filters.
struct ClassInfo {
    GK kind;
    u32 vtable;
    u32 filter;  // 0: none
};
const ClassInfo kClasses[] = {
    {GK::KillEnemyCommander, 0x4FD960, 0}, {GK::DestroyAllUnits, 0x4FD948, 0}, {GK::KillAllMobileUnits, 0x4FD928, 0x4FD920},
    {GK::BuildUnitType, 0x4FD908, 0x4FD900}, {GK::CaptureUnitType, 0x4FD8E8, 0}, {GK::KillAllOfType, 0x4FD8D0, 0x4FD8C8},
    {GK::KillUnitType, 0x4FD8B0, 0}, {GK::UnitTypeNearX, 0x4FD870, 0x4FD868}, {GK::UnitTypeNearZ, 0x4FD850, 0x4FD848},
    {GK::VictoryTimer, 0x4FD830, 0}, {GK::CommanderKilled, 0x4FD818, 0}, {GK::AllUnitsKilled, 0x4FD800, 0x4FD7F8},
    {GK::AllUnitsKilledOfType, 0x4FD7E0, 0x4FD7D8}, {GK::UnitTypeKilled, 0x4FD7C0, 0}, {GK::DeathTimer, 0x4FD7A8, 0},
    {GK::AnyUnitNearX, 0x4FD790, 0x4FD788}, {GK::AnyUnitNearZ, 0x4FD770, 0x4FD768},
};

const char* const kNames[] = {"ARMAA", "ARMBB", "CORCC", "CORDD", "EEEE"};
constexpr int kTypes = 5, kPerPlayer = 8;

struct Scenario {
    harness::Remote& r;
    std::mt19937& g;
    u32 gs = 0, units = 0, defs = 0, side_data = 0, cells = 0, out = 0;
    sim::QuietBattleOutside outside;
    sim::HeightMap heights{64, 64};
    sim::BattleRng rng;
    std::unique_ptr<sim::Battle> b;
    struct Goal {
        sim::MissionGoal e;
        u32 at = 0;
        const ClassInfo* c = nullptr;
    };
    std::vector<Goal> goals;

    Scenario(harness::Remote& rr, std::mt19937& gg) : r(rr), g(gg) {}
    u32 pick(u32 n) { return n ? g() % n : 0; }
    u32 unit_at(int id) const { return units + static_cast<u32>(id) * kUnitSize; }
    u32 player_at(int p) const { return gs + gPlayers + static_cast<u32>(p) * gPlayerSize; }

    bool setup() {
        gs = r.alloc(0x40000);
        units = r.alloc((1 + 2 * kPerPlayer) * kUnitSize + 0x100);
        defs = r.alloc((kTypes + 1) * kDefSize + 0x100);
        side_data = r.alloc(0x400);
        cells = r.alloc(64 * 64 * 13);
        out = r.alloc(0x100);
        if (!gs || !units || !defs || !side_data || !cells || !out) return false;
        // The sound call returns at once (stdcall, two arguments).
        const std::uint8_t ret8[] = {0xC2, 0x08, 0x00};
        if (!r.patch_jump(kSound, kSound) || !r.write(kSound, ret8, 3)) return false;  // (patch_jump makes the page writable)
        r.w32(kGameStatePtr, gs);
        r.w32(gs + gDefs, defs);
        r.w32(gs + gDefCount, kTypes + 1);
        for (int k = 1; k <= kTypes; ++k) {
            r.write(defs + static_cast<u32>(k) * kDefSize + 0x20, kNames[k - 1], std::strlen(kNames[k - 1]) + 1);
            const std::uint16_t index = static_cast<std::uint16_t>(k);  // the definition's own index (+0x21E)
            r.write(defs + static_cast<u32>(k) * kDefSize + 0x21E, &index, 2);
        }
        // Sides: 0 commands ARMAA, 1 CORCC.
        r.write(gs + gSides, "ARMAA", 6);
        r.write(gs + gSides + 0x232, "CORCC", 6);
        b = std::make_unique<sim::Battle>(outside, heights, rng);
        b->types.resize(kTypes + 1);
        for (int k = 1; k <= kTypes; ++k) b->types[static_cast<std::size_t>(k)].tick.name = kNames[k - 1];
        b->init_unit_table(1 + 2 * kPerPlayer);
        for (int p = 0; p < 2; ++p) {
            b->assign_unit_range(p, 1 + p * kPerPlayer, (p + 1) * kPerPlayer);
            b->players[static_cast<std::size_t>(p)].commander_name = p ? "CORCC" : "ARMAA";
            const u32 pa = player_at(p);
            r.w32(pa + 0x67, unit_at(1 + p * kPerPlayer));
            r.w32(pa + 0x6B, unit_at((p + 1) * kPerPlayer));
            r.w32(pa + 0x27, side_data + static_cast<u32>(p) * 0x100);
            r.write(side_data + static_cast<u32>(p) * 0x100 + 0x95, &p, 1);
        }
        for (int id = 1; id <= 2 * kPerPlayer; ++id) {
            const int p = id > kPerPlayer;
            r.write(unit_at(id) + 0xFF, &p, 1);
            r.w32(unit_at(id) + 0x96, player_at(p));
            r.w32(unit_at(id) + 0x92, defs);
        }
        // Heights and the map size for the position conversion.
        const std::uint8_t sea = static_cast<std::uint8_t>(pick(60));
        b->settings.sea_level = sea;
        b->map.width = 64;
        b->map.height = 64;
        std::vector<std::uint8_t> cell_bytes(64 * 64 * 13, 0);
        for (int z = 0; z < 64; ++z)
            for (int x = 0; x < 64; ++x) {
                const std::uint8_t h = static_cast<std::uint8_t>(pick(4) == 0 ? pick(250) : 40 + (x + z) % 30);
                heights.at(x, z) = h;
                cell_bytes[static_cast<std::size_t>(z * 64 + x) * 13 + 4] = h;
            }
        r.write(cells, cell_bytes.data(), cell_bytes.size());
        r.w32(gs + gCells, cells);
        r.w32(gs + gMapW, 64);
        r.w32(gs + gMapH, 64);
        r.w32(gs + gWorldW, 64 * 16);
        r.w32(gs + gWorldH, 64 * 16);
        r.write(gs + gSea, &sea, 1);
        return true;
    }

    void randomise_unit(int id) {
        sim::Unit& u = b->units[static_cast<std::size_t>(id)];
        sim::UnitTickState& t = b->ticks[static_cast<std::size_t>(id)];
        const u32 at = unit_at(id);
        const std::uint16_t type = static_cast<std::uint16_t>(pick(3) == 0 ? 0 : 1 + pick(kTypes));
        t.type_index = type;
        r.write(at + 0xA6, &type, 2);
        r.w32(at + 0x92, defs + type * kDefSize);
        t.has_motion = pick(2) != 0;
        r.w32(at, t.has_motion ? 0x1234u : 0u);
        u.build_fraction_left = pick(4) == 0 ? 0.5f : 0.0f;
        r.write(at + 0x104, &u.build_fraction_left, 4);
        u.flags = (pick(4) ? 0x20u : 0u) | (pick(2) ? 0x10000000u : 0u) | (pick(3) == 0 ? 0x40000000u : 0u);
        r.w32(at + 0x110, u.flags);
        t.transfer_holdoff = pick(5) == 0 ? 3 : 0;
        r.w32(at + 0xFB, t.transfer_holdoff);
        const int carrier = pick(5) == 0 ? 1 + static_cast<int>(pick(2 * kPerPlayer)) : 0;
        u.carrier = carrier ? &b->units[static_cast<std::size_t>(carrier)] : nullptr;
        r.w32(at + 0x86, carrier ? unit_at(carrier) : 0u);
        u.cell_x = static_cast<std::int16_t>(pick(40));
        u.cell_z = static_cast<std::int16_t>(pick(40));
        r.write(at + 0x76, &u.cell_x, 2);
        r.write(at + 0x78, &u.cell_z, 2);
    }

    void add_goal() {
        const ClassInfo& c = kClasses[pick(sizeof kClasses / sizeof kClasses[0])];
        Goal x;
        x.c = &c;
        x.e.kind = c.kind;
        x.at = r.alloc(0x80);
        r.w32(x.at, c.vtable);
        if (c.filter) r.w32(x.at + 0xC, c.filter);
        const std::string name = pick(6) == 0 ? "ZZZZ" : kNames[pick(kTypes)];
        switch (c.kind) {
        case GK::BuildUnitType:
        case GK::KillAllOfType:
        case GK::AllUnitsKilledOfType:
            x.e.type_name = name;
            r.write(x.at + 0x10, name.c_str(), name.size() + 1);
            break;
        case GK::CaptureUnitType:
            x.e.type_name = name;
            r.write(x.at + 0xC, name.c_str(), name.size() + 1);
            break;
        case GK::KillUnitType:
        case GK::UnitTypeKilled:
            x.e.type_name = name;
            x.e.count = static_cast<i32>(pick(4));
            r.write(x.at + 0xC, name.c_str(), name.size() + 1);
            r.w32(x.at + 0x2C, static_cast<u32>(x.e.count));
            break;
        case GK::UnitTypeNearX:
        case GK::UnitTypeNearZ:
            x.e.type_name = pick(3) == 0 ? std::string() : name;
            r.write(x.at + 0x10, x.e.type_name.c_str(), x.e.type_name.size() + 1);
            x.e.line = static_cast<i32>(pick(40));
            r.w32(x.at + 0x30, static_cast<u32>(x.e.line));
            break;
        case GK::AnyUnitNearX:
        case GK::AnyUnitNearZ:
            x.e.line = static_cast<i32>(pick(40));
            r.w32(x.at + 0x10, static_cast<u32>(x.e.line));
            break;
        case GK::VictoryTimer:
        case GK::DeathTimer:
            x.e.deadline = pick(400);
            r.w32(x.at + 0xC, x.e.deadline);
            break;
        default:
            break;
        }
        goals.push_back(x);
    }

    bool same(const Goal& x) {
        bool ok = (r.r32(x.at + 4) != 0) == x.e.satisfied && (r.r32(x.at + 8) != 0) == x.e.celebrated;
        switch (x.e.kind) {
        case GK::KillAllMobileUnits: ok = ok && static_cast<i32>(r.r32(x.at + 0x10)) == x.e.count; break;
        case GK::BuildUnitType: ok = ok && r.rd<std::uint16_t>(x.at + 0x30) == x.e.type; break;
        case GK::KillAllOfType:
        case GK::AllUnitsKilledOfType:
            ok = ok && r.rd<std::uint16_t>(x.at + 0x30) == x.e.type && static_cast<i32>(r.r32(x.at + 0x32)) == x.e.count;
            break;
        case GK::KillUnitType:
        case GK::UnitTypeKilled: ok = ok && static_cast<i32>(r.r32(x.at + 0x2C)) == x.e.count; break;
        default: break;
        }
        return ok;
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: goal_harness <TotalA.exe> [scenarios] [operations] [first seed]\n");
        return 2;
    }
    const int scenarios = argc > 2 ? std::atoi(argv[2]) : 10;
    const int ops = argc > 3 ? std::atoi(argv[3]) : 200;
    const u32 first = argc > 4 ? static_cast<u32>(std::strtoul(argv[4], nullptr, 0)) : 1u;
    std::size_t total = 0, checks = 0, destroyed = 0, transferred = 0, conversions = 0, met = 0, faults = 0, mismatches = 0;
    for (int sc = 0; sc < scenarios; ++sc) {
        const u32 seed = first + static_cast<u32>(sc);
        std::mt19937 g(seed);
        harness::Remote r;
        if (!r.start(argv[1])) {
            std::printf("cannot start %s\n", argv[1]);
            return 1;
        }
        Scenario s(r, g);
        if (!s.setup()) return 1;
        for (int id = 1; id <= 2 * kPerPlayer; ++id) s.randomise_unit(id);
        const int ngoals = 3 + static_cast<int>(s.pick(6));
        for (int k = 0; k < ngoals; ++k) s.add_goal();
        u32 tick = 0;
        bool bad = false;
        for (int op = 0; op < ops && !bad; ++op) {
            ++total;
            for (int n = static_cast<int>(s.pick(3)); n > 0; --n) s.randomise_unit(1 + static_cast<int>(s.pick(2 * kPerPlayer)));
            for (int p = 0; p < 2; ++p) {
                const std::uint16_t alive = static_cast<std::uint16_t>(s.pick(4) == 0 ? 0 : 1 + s.pick(3));
                s.b->players[static_cast<std::size_t>(p)].alive_count = alive;
                r.write(s.player_at(p) + 0x144, &alive, 2);
            }
            tick += s.pick(20);
            r.w32(s.gs + gTick, tick);
            s.b->unit_env().game_tick = tick;
            const u32 what = s.pick(10);
            if (what < 5) {
                Scenario::Goal& x = s.goals[s.pick(static_cast<u32>(s.goals.size()))];
                u32 res = 0;
                const bool e = sim::mission_goal_check(*s.b, x.e);
                if (!r.call(r.r32(x.c->vtable), x.at, {}, &res)) {
                    std::printf("scenario %u: the original's check failed (kind %d)\n", seed, static_cast<int>(x.e.kind));
                    ++mismatches;
                    bad = true;
                    break;
                }
                ++checks;
                met += e;
                if ((res & 0xFF) != 0 != e) {
                    std::printf("scenario %u op %d: check of kind %d gives %u, engine %d\n", seed, op, static_cast<int>(x.e.kind), res & 0xFF, e);
                    ++mismatches;
                    bad = true;
                }
            } else if (what < 8) {
                const int id = 1 + static_cast<int>(s.pick(2 * kPerPlayer));
                const bool transfer = what == 7;
                for (Scenario::Goal& x : s.goals) {
                    sim::Unit& u = s.b->units[static_cast<std::size_t>(id)];
                    if (transfer) sim::mission_goal_unit_transferred(*s.b, x.e, u);
                    else sim::mission_goal_unit_destroyed(*s.b, x.e, u);
                    if (!r.call(r.r32(x.c->vtable + (transfer ? 8u : 4u)), x.at, {s.unit_at(id)})) {
                        std::printf("scenario %u: the original's event handler failed\n", seed);
                        ++mismatches;
                        bad = true;
                        break;
                    }
                }
                (transfer ? transferred : destroyed)++;
            } else {
                const i32 x = static_cast<i32>(s.pick(1100)) - 30, y = static_cast<i32>(s.pick(1100)) - 30;
                const u32 before = s.b->mission_goals.faults;
                const std::array<i32, 3> e = sim::screen_to_terrain(*s.b, x, y);
                const bool engine_fault = s.b->mission_goals.faults != before;
                const bool ok_call = r.call(kScreenToTerrain, 0, {static_cast<u32>(x), static_cast<u32>(y), s.out});
                ++conversions;
                if (engine_fault || !ok_call) {
                    ++faults;
                    if (engine_fault != !ok_call) {
                        std::printf("scenario %u: conversion of %d,%d: fault engine %d original %d\n", seed, x, y, engine_fault, !ok_call);
                        ++mismatches;
                        bad = true;
                    }
                    if (!ok_call) break;  // the original's thread died; the scenario ends
                } else if (static_cast<i32>(r.r32(s.out)) != e[0] || static_cast<i32>(r.r32(s.out + 4)) != e[1] ||
                           static_cast<i32>(r.r32(s.out + 8)) != e[2]) {
                    std::printf("scenario %u: conversion of %d,%d: original %d,%d,%d engine %d,%d,%d\n", seed, x, y, static_cast<i32>(r.r32(s.out)),
                                static_cast<i32>(r.r32(s.out + 4)), static_cast<i32>(r.r32(s.out + 8)), e[0], e[1], e[2]);
                    ++mismatches;
                    bad = true;
                }
            }
            for (const Scenario::Goal& x : s.goals)
                if (!bad && !s.same(x)) {
                    std::printf("scenario %u op %d: goal of kind %d differs: satisfied %u/%d cue %u/%d count %d type %u\n", seed, op,
                                static_cast<int>(x.e.kind), r.r32(x.at + 4), x.e.satisfied, r.r32(x.at + 8), x.e.celebrated, x.e.count, x.e.type);
                    ++mismatches;
                    bad = true;
                }
        }
    }
    std::printf("scenarios %d, operations %zu: checks %zu (%zu met), destroyed events %zu, transfer events %zu, conversions %zu (%zu faults)\n",
                scenarios, total, checks, met, destroyed, transferred, conversions, faults);
    std::printf("%zu mismatches\n", mismatches);
    return mismatches ? 1 : 0;
}
