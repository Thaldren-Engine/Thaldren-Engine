// Differential tester for the movement classes' passability and the route
// search (sim/route_search.h). Inside a suspended TotalA.exe it builds a
// random map (heights, sea level, blocking and non-blocking features, multi-cell
// feature parts, explored bits), the real movement classes of
// gamedata\moveinfo.tdf, three players with ground units carrying motion
// objects, route goals and order goals (circle, ring, edge, point) written
// field by field, then runs the original grid build and search constructor.
// Each step applies random operations (goals asking for routes, goal changes
// with the search forgetting the old one, unit moves with the release and
// claim passability updates, alive flags, rectangle updates, feature and
// explored changes, placement checks) and one step of the original route
// search, and compares the whole search object (open heap, node pool, cell
// states, touched bitmap, budgets and cursors), every class grid and lock step,
// the program-wide counters, route goals, order events and the units' footprint
// steps with the engine.
//
// Usage: route_harness <TotalA.exe> <game_dir> [scenarios] [steps] [first seed]

#define NOMINMAX
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/heightmap.h"
#include "sim/motion.h"
#include "sim/orders.h"
#include "sim/route_search.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"
#include "ta/hpi.h"
#include "ta/unitdefs.h"

namespace {

using harness::u32;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using i16 = std::int16_t;
using i32 = std::int32_t;

constexpr u32 kGameStatePtr = 0x00511DE8;
constexpr u32 kAlloc = 0x004B4F10, kFree = 0x004B4F20;
constexpr u32 kSearchCtor = 0x0040E9E0, kSearchTick = 0x0040EB70, kForget = 0x0040E9C0, kRebuild = 0x00440940,
              kUpdateAll = 0x00440A40, kAfterRelease = 0x00440A70, kCanPlace = 0x0047DB70, kYardFits = 0x0047D2E0;
constexpr u32 kMoveInfo = 0x00512358, kRefreshCounter = 0x00511A38, kScales = 0x005119E8, kLoads = 0x00511A10;
constexpr u32 kRouteGoalVt = 0x004FD458, kCircleVt = 0x004FD328, kRingVt = 0x004FD358, kEdgeVt = 0x004FD388, kPointVt = 0x004FD3B8;
constexpr u32 kUnitSize = 0x118, kPlayerSize = 0x14B, kDefSize = 0x249, kObjStride = 0x200;
constexpr int kUnits = 20;  // table entries 0..19 (three players of six units)
constexpr int kMaxMap = 128;

enum : u32 {
    uMotion = 0x00, uHeading = 0x66, uPosX = 0x6A, uPosY = 0x6E, uPosZ = 0x72, uCell = 0x76, uFootprint = 0x7E, uDefIndex = 0xA6,
    uFlags = 0x110,
    pActive = 0x00, pFirst = 0x67, pLast = 0x6B, pController = 0x73, pState = 0x146,
    mGoal = 0x00, mClass = 0x04, mClaim = 0x26,
    oEvents = 0x4E,
    // object block of each unit
    obMotion = 0x00, obGoal = 0x40, obOrder = 0xB0, obOrderGoal = 0x110,
};

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8v(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void bytes(std::initializer_list<std::uint8_t> l) { for (auto v : l) b.push_back(v); }
    void align() { while (b.size() % 16) b.push_back(0xCC); }
};

struct Layout {
    u32 heap = 0, scenario_heap = 0, bump_ptr = 0, stubs = 0, ret_val = 0, gs = 0, units = 0, cells = 0, fdefs = 0, explored = 0, objs = 0,
        defs = 0, names = 0, search = 0;
};

bool setup(harness::Remote& r, Layout& L) {
    L.heap = r.alloc(64u << 20);
    L.bump_ptr = r.alloc(0x1000);
    L.stubs = r.alloc(0x1000, PAGE_EXECUTE_READWRITE);
    L.ret_val = r.alloc(0x1000);
    L.gs = r.alloc(0x40000);
    L.units = r.alloc(static_cast<SIZE_T>(kUnits + 2) * kUnitSize);
    L.cells = r.alloc(static_cast<SIZE_T>(kMaxMap) * kMaxMap * 13);
    L.fdefs = r.alloc(0x2000);
    L.explored = r.alloc(0x4000);
    L.objs = r.alloc(static_cast<SIZE_T>(kUnits + 2) * kObjStride);
    L.defs = r.alloc(16 * kDefSize);
    L.names = r.alloc(0x1000);
    L.search = r.alloc(0x1000);
    if (!L.heap || !L.stubs || !L.gs || !L.units || !L.cells || !L.objs || !L.search) return false;
    Emitter e;
    auto here = [&]() { return L.stubs + static_cast<u32>(e.b.size()); };
    // Allocators: a bump heap. The size is at [esp+8] (tagged allocator), [esp+4] (malloc), [esp+12].
    const int size_at[3] = {8, 4, 12};
    const u32 targets[3] = {0x004D83B0, kAlloc, 0x004D84A0};
    for (int k = 0; k < 3; ++k) {
        const u32 at = here();
        e.u8v(0xA1); e.u32le(L.bump_ptr);
        e.u8v(0x8B); e.u8v(0x4C); e.u8v(0x24); e.u8v(static_cast<std::uint8_t>(size_at[k]));
        e.u8v(0x83); e.u8v(0xC1); e.u8v(0x03); e.u8v(0x83); e.u8v(0xE1); e.u8v(0xFC);
        e.u8v(0x01); e.u8v(0x0D); e.u32le(L.bump_ptr);
        e.u8v(0xC3);
        e.align();
        if (!r.patch_jump(targets[k], at)) return false;
    }
    {  // free: nothing
        const u32 at = here();
        e.u8v(0xC3);
        e.align();
        if (!r.patch_jump(kFree, at)) return false;
    }
    {  // structure yard placement (def, cell, 0, 0): counts the call, returns the scripted answer
        const u32 at = here();
        e.u8v(0xFF); e.u8v(0x05); e.u32le(L.ret_val + 4);  // inc dword [count]
        e.u8v(0x8B); e.u8v(0x44); e.u8v(0x24); e.u8v(0x08);  // mov eax,[esp+8] (cell)
        e.u8v(0xA3); e.u32le(L.ret_val + 8);
        e.u8v(0xA1); e.u32le(L.ret_val);
        e.bytes({0xC2, 0x10, 0x00});
        e.align();
        if (!r.patch_jump(kYardFits, at)) return false;
    }
    if (!r.write(L.stubs, e.b.data(), e.b.size())) return false;
    r.w32(L.bump_ptr, L.heap);
    r.w32(kGameStatePtr, L.gs);
    L.scenario_heap = L.heap;
    return true;
}

struct Systems : sim::MotionSystems, sim::RouteSystems {
    bool yard_answer = true;
    int yard_calls = 0;
    bool can_place(sim::Unit&, i16, i16, u32) override { return true; }
    void clear_footprint(sim::Unit&) override {}
    void claim_footprint(sim::Unit&) override {}
    void update_sight(sim::Unit&) override {}
    bool off_map(const sim::Unit&) override { return false; }
    u8 bucket_height(const sim::Unit&) override { return 0; }
    void route_search_forget(sim::MotionGoal&) override {}
    void set_activated(sim::Unit&, bool) override {}
    u32 allocate_object(u32) override { return 0; }
    void free_object(u32) override {}
    bool structure_fits(u16, i16, i16) override {
        ++yard_calls;
        return yard_answer;
    }
};

struct Stats {
    std::size_t scenarios = 0, steps = 0, ops = 0, searches = 0, routes = 0, no_routes = 0, rounds = 0, expansions_left = 0, places = 0,
                place_true = 0, yard_calls = 0, mismatches = 0, failures = 0, faults = 0, probe_hits = 0, probe_partial = 0, max_open = 0;
    std::map<std::string, std::size_t> by_op;
};

struct Scenario {
    int w = 0, h = 0;
    std::vector<sim::MoveClass> classes;
};

bool run_scenario(harness::Remote& r, const Layout& L, const ta::UnitDefContext& ctx, int steps, unsigned seed, Stats& st) {
    std::mt19937 g(seed);
    auto rnd = [&](int n) { return n <= 1 ? 0 : static_cast<int>(g() % static_cast<u32>(n)); };
    // Fresh memory.
    std::vector<std::uint8_t> zero(0x400000, 0);
    r.w32(L.bump_ptr, L.scenario_heap);
    r.write(L.scenario_heap, zero.data(), zero.size());
    r.write(L.gs, zero.data(), 0x40000);
    r.write(L.units, zero.data(), (kUnits + 2) * kUnitSize);
    r.write(L.objs, zero.data(), (kUnits + 2) * kObjStride);
    r.write(L.search, zero.data(), 0x1000);

    // --- map -----------------------------------------------------------------
    const int W = 24 + 2 * rnd(53), H = 24 + 2 * rnd(53);
    sim::MapGrid map;
    map.width = W;
    map.height = H;
    map.cells.assign(static_cast<std::size_t>(W * H), sim::MapCell{});
    {
        std::vector<int> hts(static_cast<std::size_t>(W * H), 40 + rnd(40));
        const int bumps = 3 + rnd(10);
        for (int b = 0; b < bumps; ++b) {
            const int cx = rnd(W), cz = rnd(H), rad = 3 + rnd(12), amp = rnd(160) - 60;
            for (int z = 0; z < H; ++z)
                for (int x = 0; x < W; ++x) {
                    const int d2 = (x - cx) * (x - cx) + (z - cz) * (z - cz);
                    if (d2 < rad * rad) hts[static_cast<std::size_t>(z * W + x)] += amp * (rad * rad - d2) / (rad * rad);
                }
        }
        if (rnd(2)) {  // a cliff line
            const int cx = rnd(W);
            for (int z = 0; z < H; ++z)
                for (int x = cx; x < W; ++x) hts[static_cast<std::size_t>(z * W + x)] += 30 + rnd(3);
        }
        for (int i = 0; i < W * H; ++i) {
            int v = hts[static_cast<std::size_t>(i)] + rnd(3);
            map.cells[static_cast<std::size_t>(i)].height = static_cast<u8>(std::clamp(v, 0, 255));
        }
        for (int z = 0; z < H; ++z)
            for (int x = 0; x < W; ++x) {
                sim::MapCell& c = map.cells[static_cast<std::size_t>(z * W + x)];
                u8 mx = c.height, mn = c.height;
                auto take = [&](int xx, int zz) {
                    const u8 v = map.cells[static_cast<std::size_t>(zz * W + xx)].height;
                    mx = std::max(mx, v);
                    mn = std::min(mn, v);
                };
                if (x < W - 1) take(x + 1, z);
                if (z < H - 1) take(x, z + 1);
                if (x < W - 1 && z < H - 1) take(x + 1, z + 1);
                c.height_max = mx;
                c.height_min = mn;
            }
    }
    const u8 sea = static_cast<u8>(rnd(3) == 0 ? 0 : rnd(90));
    // Features: a few types, some blocking; single cells and 2 x 2 features.
    const int ftypes = 1 + rnd(6);
    std::vector<u8> fblocks(static_cast<std::size_t>(ftypes));
    for (auto& b : fblocks) b = rnd(2) ? 1 : 0;
    const int nfeat = rnd(W * H / 12);
    // Single cells first, then the bigger features (whose own cell always holds a known type, as on real maps).
    std::vector<int> kinds(static_cast<std::size_t>(nfeat));
    for (auto& k : kinds) k = rnd(10);
    std::stable_partition(kinds.begin(), kinds.end(), [](int k) { return k < 6 || k == 9; });
    for (int k = 0; k < nfeat; ++k) {
        const int x = rnd(W - 1), z = rnd(H - 1);
        sim::MapCell& c = map.cells[static_cast<std::size_t>(z * W + x)];
        const int kind = kinds[static_cast<std::size_t>(k)];
        if (kind < 6) {
            c.feature = static_cast<u16>(rnd(ftypes + 1));  // may be past the table (counts as blocking)
        } else if (kind < 9) {
            c.feature = static_cast<u16>(rnd(ftypes));
            // 2 x 2: the other three cells point back to this one
            const int dx[3] = {1, 0, 1}, dz[3] = {0, 1, 1};
            for (int q = 0; q < 3; ++q) {
                sim::MapCell& p = map.cells[static_cast<std::size_t>((z + dz[q]) * W + x + dx[q])];
                p.feature = 0xFFFE;
                p.feature_aux = static_cast<u16>(dz[q] | (dx[q] << 8));
            }
        } else {
            c.feature = static_cast<u16>(0xFFFB + rnd(3));
        }
    }
    std::vector<u16> explored(static_cast<std::size_t>((W >> 1) * (H >> 1)));
    for (auto& e : explored) e = rnd(4) ? 0xFFFF : static_cast<u16>(g());

    // --- movement classes -------------------------------------------------------
    std::array<sim::MoveClass, sim::kMoveClasses> classes{};
    std::vector<int> named;
    for (std::size_t i = 0; i < classes.size(); ++i) {
        const auto& m = ctx.movement_classes[i];
        sim::MoveClass& c = classes[i];
        c.named = !ctx.movement_class_names[i].empty();
        c.footprint_x = m.footprint_x;
        c.footprint_z = m.footprint_z;
        c.max_water_depth = m.max_water_depth;
        c.min_water_depth = m.min_water_depth;
        c.max_slope = m.max_slope;
        c.bad_slope = m.bad_slope;
        c.max_water_slope = m.max_water_slope;
        c.bad_water_slope = m.bad_water_slope;
        c.lock_tick = rnd(3) == 0 ? static_cast<u32>(rnd(400)) : 0;  // left over from an earlier battle
        if (c.named) named.push_back(static_cast<int>(i));
    }
    if (named.empty()) return false;
    for (std::size_t i = 0; i < classes.size(); ++i) {
        const sim::MoveClass& c = classes[i];
        std::uint8_t blk[0x20] = {};
        const u32 name = c.named ? L.names + static_cast<u32>(i) * 8 : 0u;
        std::memcpy(blk + 0, &name, 4);
        std::memcpy(blk + 4, &c.footprint_x, 2);
        std::memcpy(blk + 6, &c.footprint_z, 2);
        std::memcpy(blk + 8, &c.max_water_depth, 2);
        std::memcpy(blk + 0xA, &c.min_water_depth, 2);
        blk[0xC] = c.max_slope;
        blk[0xD] = c.bad_slope;
        blk[0xE] = c.max_water_slope;
        blk[0xF] = c.bad_water_slope;
        std::memcpy(blk + 0x1C, &c.lock_tick, 4);
        r.write(kMoveInfo + static_cast<u32>(i) * 0x20, blk, sizeof blk);
    }

    // --- players and units ------------------------------------------------------
    std::vector<sim::PlayerSlot> players(11);
    std::vector<sim::Unit> units(kUnits);
    std::vector<sim::UnitTickState> ticks(kUnits);
    for (int i = 0; i < kUnits; ++i) units[static_cast<std::size_t>(i)].id = static_cast<u16>(i);
    for (int p = 0; p < 3; ++p) {
        sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        s.active = rnd(8) != 0;
        s.controller = static_cast<u8>(1 + rnd(3));
        s.state = static_cast<u8>(rnd(8) == 0 ? 10 : p);
        s.first_unit = 1 + 6 * p;
        s.last_unit = 6 + 6 * p;
    }
    // Types 1-4: ground units of four named classes; 5-10: placement checks (some structures).
    std::vector<sim::RouteType> rtypes(11);
    std::vector<sim::MotionType> mtypes(11);
    for (int k = 1; k <= 4; ++k) {
        sim::RouteType& t = rtypes[static_cast<std::size_t>(k)];
        t.move_class = static_cast<std::int8_t>(named[static_cast<std::size_t>(rnd(static_cast<int>(named.size())))]);
        const sim::MoveClass& c = classes[static_cast<std::size_t>(t.move_class)];
        t.footprint_x = c.footprint_x;
        t.footprint_z = c.footprint_z;
        t.movement = 1;
        mtypes[static_cast<std::size_t>(k)].has_move_class = true;
    }
    for (int k = 5; k <= 10; ++k) {
        sim::RouteType& t = rtypes[static_cast<std::size_t>(k)];
        t.footprint_x = static_cast<i16>(rnd(5));
        t.footprint_z = static_cast<i16>(rnd(5));
        t.movement = static_cast<u8>(rnd(4) == 0 ? 0 : 1);
        t.max_slope = static_cast<u8>(rnd(40));
        t.max_water_slope = static_cast<u8>(rnd(80));
        t.max_water_depth = static_cast<i16>(rnd(4) == 0 ? 10000 : rnd(60));
        t.min_water_depth = static_cast<i16>(rnd(4) == 0 ? -10000 : rnd(30) - 10);
    }
    // Remote defs for the placement checks.
    for (int k = 0; k <= 10; ++k) {
        std::vector<std::uint8_t> d(kDefSize, 0);
        const sim::RouteType& t = rtypes[static_cast<std::size_t>(k)];
        std::memcpy(&d[0x14A], &t.footprint_x, 2);
        std::memcpy(&d[0x14C], &t.footprint_z, 2);
        std::memcpy(&d[0x1BE], &t.max_water_depth, 2);
        std::memcpy(&d[0x1C0], &t.min_water_depth, 2);
        d[0x228] = t.max_slope;
        d[0x229] = t.max_water_slope;
        d[0x22F] = t.movement;
        r.write(L.defs + static_cast<u32>(k) * kDefSize, d.data(), d.size());
    }

    // Unit objects: motion, route goal, order, order goal.
    std::vector<std::shared_ptr<sim::RouteGoal>> rgoals(kUnits);
    std::vector<std::unique_ptr<sim::MissionOrder>> orders(kUnits);
    auto obj = [&](int i, u32 off) { return L.objs + static_cast<u32>(i) * kObjStride + off; };
    auto unit_addr = [&](int i) { return L.units + static_cast<u32>(i) * kUnitSize; };
    std::vector<sim::Unit*> placed(static_cast<std::size_t>(W * H), nullptr);
    auto occupy = [&](int i, bool on) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        for (int dz = 0; dz < u.footprint_z; ++dz)
            for (int dx = 0; dx < u.footprint_x; ++dx) {
                const int x = u.cell_x + dx, z = u.cell_z + dz;
                if (x < 0 || z < 0 || x >= W || z >= H) continue;
                sim::MapCell& c = map.cells[static_cast<std::size_t>(z * W + x)];
                if (on && c.unit == 0) c.unit = static_cast<u16>(i);
                if (!on && c.unit == i) c.unit = 0;
            }
    };
    sim::HeightMap hm(1, 1);
    Systems sys;
    sim::MotionContext mc{units, ticks, players, mtypes, hm, sys, {}, {}};
    for (int i = 1; i < kUnits; ++i) {
        const bool in_range = i <= 18;
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = ticks[static_cast<std::size_t>(i)];
        if (!in_range || rnd(8) == 0) continue;  // empty entry
        t.type_index = static_cast<u16>(1 + rnd(4));
        t.owner = (i - 1) / 6;
        const sim::RouteType& ty = rtypes[t.type_index];
        u.footprint_x = ty.footprint_x;
        u.footprint_z = ty.footprint_z;
        u.cell_x = static_cast<i16>(rnd(W - 2));
        u.cell_z = static_cast<i16>(rnd(H - 2));
        u.heading = static_cast<u16>(g());
        u.pos = {((u.cell_x * 2 + u.footprint_x) << 19), 0, ((u.cell_z * 2 + u.footprint_z) << 19)};
        u.flags = rnd(6) ? sim::unit_flags::kAlive : 0u;
        t.has_motion = rnd(10) != 0;
        if (t.has_motion) {
            t.motion.has_move_class = rnd(12) != 0;
            t.motion.claim_tick = static_cast<u32>(rnd(300));
            auto rg = std::make_shared<sim::RouteGoal>();
            rg->address = obj(i, obGoal);
            rg->unit = &u;
            rg->flags = static_cast<u8>(rnd(16));
            rg->search_tick = static_cast<u32>(rnd(200));
            rg->count = rnd(4);
            for (int k = 0; k < rg->count; ++k) rg->points[static_cast<std::size_t>(k)] = g() & 0x03FF03FFu;
            rgoals[static_cast<std::size_t>(i)] = rg;
            t.motion.goal = rg;
        }
        if (u.flags & sim::unit_flags::kAlive && rnd(4)) occupy(i, true);
    }
    // Cells held by units outside the table's live entries (structures without motion).
    for (int k = rnd(6); k > 0; --k) {
        const int x = rnd(W), z = rnd(H);
        map.cells[static_cast<std::size_t>(z * W + x)].unit = static_cast<u16>(1 + rnd(kUnits - 1));
    }

    // Order goals: written into both.
    auto set_order_goal = [&](int i) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        auto& rg = rgoals[static_cast<std::size_t>(i)];
        if (!rg) return;
        if (!orders[static_cast<std::size_t>(i)]) {
            orders[static_cast<std::size_t>(i)] = std::make_unique<sim::MissionOrder>();
            orders[static_cast<std::size_t>(i)]->address = obj(i, obOrder);
            orders[static_cast<std::size_t>(i)]->unit = &u;
        }
        sim::MissionOrder& o = *orders[static_cast<std::size_t>(i)];
        std::uint8_t blk[0x40] = {};
        const u32 oaddr = o.address;
        std::memcpy(blk + 4, &oaddr, 4);
        const int kind = rnd(10);
        std::shared_ptr<sim::OrderGoal> og;
        auto put16 = [&](int off, i32 v) { const i16 s = static_cast<i16>(v); std::memcpy(blk + off, &s, 2); };
        auto put32 = [&](int off, i32 v) { std::memcpy(blk + off, &v, 4); };
        if (kind < 4) {
            auto c = std::make_shared<sim::CircleGoal>();
            c->cell_x = static_cast<i16>(rnd(W + 8) - 4);
            c->cell_z = static_cast<i16>(rnd(H + 8) - 4);
            c->radius = rnd(4) == 0 ? 0 : rnd(120);
            c->radius_cells_sq = (c->radius / 16) * (c->radius / 16);
            const u32 vt = kCircleVt;
            std::memcpy(blk, &vt, 4);
            put16(8, c->cell_x);
            put16(0xA, c->cell_z);
            put32(0xC, c->radius);
            put32(0x10, c->radius_cells_sq);
            og = c;
        } else if (kind < 6) {
            auto c = std::make_shared<sim::RingGoal>();
            c->cell_x = static_cast<i16>(rnd(W));
            c->cell_z = static_cast<i16>(rnd(H));
            c->inner = rnd(100);
            c->outer = c->inner + rnd(150);
            c->inner_cells_sq = (c->inner / 16) * (c->inner / 16);
            c->outer_cells_sq = (c->outer / 16) * (c->outer / 16);
            const u32 vt = kRingVt;
            std::memcpy(blk, &vt, 4);
            put16(8, c->cell_x);
            put16(0xA, c->cell_z);
            put32(0xC, c->inner);
            put32(0x10, c->outer);
            put32(0x14, c->inner_cells_sq);
            put32(0x18, c->outer_cells_sq);
            og = c;
        } else if (kind < 8) {
            auto c = std::make_shared<sim::EdgeGoal>();
            const int cx = rnd(W), cz = rnd(H), sx = 1 + rnd(4), sz = 1 + rnd(4);
            c->min_x = cx - u.footprint_x;
            c->min_z = cz - u.footprint_z;
            c->max_x = cx + sx;
            c->max_z = cz + sz;
            const u32 vt = kEdgeVt;
            std::memcpy(blk, &vt, 4);
            put32(8, c->min_x);
            put32(0xC, c->max_x);
            put32(0x10, c->min_z);
            put32(0x14, c->max_z);
            og = c;
        } else {
            auto c = std::make_shared<sim::PointGoal>();
            c->flags = sim::point_goal::kGround | sim::point_goal::kAltitude;
            c->piece = -1;
            c->pos = {(rnd(W) << 20) + rnd(0x100000), 0, (rnd(H) << 20) + rnd(0x100000)};
            const u32 vt = kPointVt;
            std::memcpy(blk, &vt, 4);
            put16(8, c->flags);
            put16(0x10, -1);
            const u32 ua = unit_addr(i);
            std::memcpy(blk + 0x12, &ua, 4);
            put32(0x26, c->pos[0]);
            put32(0x2A, c->pos[1]);
            put32(0x2E, c->pos[2]);
            og = c;
        }
        og->order = &o;
        og->owner = &u;
        og->address = obj(i, obOrderGoal);
        r.write(og->address, blk, sizeof blk);
        o.goal = og;
        o.attached = og->address;
        rg->attached = og.get();
    };
    for (int i = 1; i < kUnits; ++i)
        if (rgoals[static_cast<std::size_t>(i)] && rnd(5)) set_order_goal(i);
    for (auto& rg : rgoals)  // (the route goal's own rule: no order goal, no wish for a route)
        if (rg && !rg->attached) rg->flags = static_cast<u8>(rg->flags & ~sim::route_flags::kWantsRoute);

    // --- remote state -------------------------------------------------------------
    auto write_cells = [&]() {
        std::vector<std::uint8_t> rc(static_cast<std::size_t>(W * H) * 13, 0);
        for (int i = 0; i < W * H; ++i) {
            const sim::MapCell& c = map.cells[static_cast<std::size_t>(i)];
            std::uint8_t* p = &rc[static_cast<std::size_t>(i) * 13];
            std::memcpy(p, &c.unit, 2);
            std::memcpy(p + 2, &c.unit2, 2);
            p[4] = c.height;
            p[5] = c.height_max;
            p[6] = c.height_min;
            p[7] = c.metal;
            std::memcpy(p + 8, &c.feature, 2);
            std::memcpy(p + 10, &c.feature_aux, 2);
            p[12] = c.flags;
        }
        r.write(L.cells, rc.data(), rc.size());
    };
    write_cells();
    {
        std::vector<std::uint8_t> fd(static_cast<std::size_t>(ftypes) * 0x100 + 0x100, 0);
        for (int k = 0; k < ftypes; ++k) fd[static_cast<std::size_t>(k) * 0x100 + 0xFE] = fblocks[static_cast<std::size_t>(k)] ? 0x40 | static_cast<u8>(rnd(64)) : static_cast<u8>(rnd(64) & ~0x40);
        r.write(L.fdefs, fd.data(), fd.size());
    }
    r.write(L.explored, explored.data(), explored.size() * 2);
    r.w32(L.gs + 0x14233, static_cast<u32>(W));
    r.w32(L.gs + 0x14237, static_cast<u32>(H));
    r.w32(L.gs + 0x14287, L.cells);
    r.write(L.gs + 0x1427F, &sea, 1);
    r.w32(L.gs + 0x14253, static_cast<u32>(ftypes));
    r.w32(L.gs + 0x1426F, L.fdefs);
    r.w32(L.gs + 0x14273, L.explored);
    r.w32(L.gs + 0x14357, L.units);
    r.w32(L.gs + 0x1435B, unit_addr(kUnits - 1));
    sim::RouteEnvironment env;
    env.sea_level = sea;
    env.game_tick = static_cast<u32>(rnd(3) == 0 ? rnd(30) : rnd(300));
    env.player_count = static_cast<u16>(1 + rnd(4));
    env.units_per_player = static_cast<u16>(1 + rnd(20));
    r.w32(L.gs + 0x38A47, env.game_tick);
    r.write(L.gs + 0x2A3C, &env.player_count, 2);
    r.write(L.gs + 0x1434F, &env.units_per_player, 2);
    for (int p = 0; p < 10; ++p) {
        const sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        const u32 pa = L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize;
        r.w32(pa + pActive, s.active ? 1u : 0u);
        r.write(pa + pController, &s.controller, 1);
        r.write(pa + pState, &s.state, 1);
        if (p < 3) {
            r.w32(pa + pFirst, unit_addr(s.first_unit));
            r.w32(pa + pLast, unit_addr(s.last_unit));
        }
    }
    auto write_unit = [&](int i) {
        const sim::Unit& u = units[static_cast<std::size_t>(i)];
        const sim::UnitTickState& t = ticks[static_cast<std::size_t>(i)];
        std::vector<std::uint8_t> b(kUnitSize, 0);
        const u32 mo = t.has_motion ? obj(i, obMotion) : 0u;
        std::memcpy(&b[uMotion], &mo, 4);
        std::memcpy(&b[uHeading], &u.heading, 2);
        std::memcpy(&b[uPosX], &u.pos[0], 4);
        std::memcpy(&b[uPosY], &u.pos[1], 4);
        std::memcpy(&b[uPosZ], &u.pos[2], 4);
        std::memcpy(&b[uCell], &u.cell_x, 2);
        std::memcpy(&b[uCell + 2], &u.cell_z, 2);
        std::memcpy(&b[uFootprint], &u.footprint_x, 2);
        std::memcpy(&b[uFootprint + 2], &u.footprint_z, 2);
        std::memcpy(&b[uDefIndex], &t.type_index, 2);
        std::memcpy(&b[uFlags], &u.flags, 4);
        r.write(unit_addr(i), b.data(), b.size());
        if (t.has_motion) {
            std::uint8_t m[0x30] = {};
            const u32 ga = obj(i, obGoal);
            std::memcpy(m + mGoal, &ga, 4);
            const u32 cls = t.motion.has_move_class ? kMoveInfo + static_cast<u32>(rtypes[t.type_index].move_class) * 0x20 : 0u;
            std::memcpy(m + mClass, &cls, 4);
            std::memcpy(m + mClaim, &t.motion.claim_tick, 4);
            r.write(obj(i, obMotion), m, sizeof m);
        }
    };
    auto write_goal = [&](int i) {
        const auto& rg = rgoals[static_cast<std::size_t>(i)];
        if (!rg) return;
        std::uint8_t b[0x68] = {};
        const u32 vt = kRouteGoalVt, att = rg->attached ? rg->attached->address : 0u, ua = unit_addr(i);
        std::memcpy(b, &vt, 4);
        std::memcpy(b + 4, &att, 4);
        std::memcpy(b + 8, &ua, 4);
        std::memcpy(b + 0xC, rg->points.data(), 80);
        std::memcpy(b + 0x5C, &rg->count, 4);
        std::memcpy(b + 0x60, &rg->search_tick, 4);
        b[0x64] = rg->flags;
        r.write(rg->address, b, sizeof b);
    };
    for (int i = 0; i < kUnits; ++i) {
        write_unit(i);
        write_goal(i);
    }
    // Program-wide counters left by an earlier battle.
    sim::RouteSearch search;
    search.refresh_counter = rnd(150);
    for (auto& v : search.load) v = rnd(3) ? 0 : rnd(40);
    r.w32(kRefreshCounter, static_cast<u32>(search.refresh_counter));
    r.write(kLoads, search.load.data(), 40);

    sim::RouteContext c{map, units, ticks, players, rtypes, classes, fblocks, explored, search, mc, sys, env, {}};
    auto sync_env = [&]() {
        c.env = env;
        mc.env.game_tick = env.game_tick;
        mc.env.sea_level = env.sea_level;
        r.w32(L.gs + 0x38A47, env.game_tick);
    };
    sync_env();

    // --- build and set up -------------------------------------------------------------
    if (!r.call(kRebuild, 0, {})) { std::printf("original call failed at line %d\n", __LINE__); return false; }
    sim::build_passability(c);
    if (!r.call(kSearchCtor, L.search, {})) { std::printf("original call failed at line %d\n", __LINE__); return false; }
    sim::init_route_search(c);

    auto fail = [&](const std::string& what) {
        std::printf("scenario %u: %s\n", seed, what.c_str());
        ++st.mismatches;
        return true;
    };
    auto engine_faulted = [&]() {
        if (c.fault.empty()) return false;
        std::printf("scenario %u: engine fault (original would crash or read wild memory): %s\n", seed, c.fault.c_str());
        ++st.faults;
        return true;
    };
    auto goal_address = [&](const sim::MotionGoal* gl) { return gl ? gl->address : 0u; };
    auto unit_id_of = [&](u32 a) -> int { return a ? static_cast<int>((a - L.units) / kUnitSize) : -1; };

    std::string bad;
    auto compare = [&](const std::string& where) -> bool {
        bad.clear();
        auto mism = [&](const std::string& s) {
            if (bad.empty()) bad = s;
        };
        // Classes.
        for (std::size_t k = 0; k < classes.size(); ++k) {
            const sim::MoveClass& m = classes[k];
            std::uint8_t blk[0x20];
            r.read(kMoveInfo + static_cast<u32>(k) * 0x20, blk, sizeof blk);
            u32 lock, width, height, grid;
            std::memcpy(&width, blk + 0x10, 4);
            std::memcpy(&height, blk + 0x14, 4);
            std::memcpy(&grid, blk + 0x18, 4);
            std::memcpy(&lock, blk + 0x1C, 4);
            if (lock != m.lock_tick) mism("class " + std::to_string(k) + " lock step orig " + std::to_string(lock) + " ours " + std::to_string(m.lock_tick));
            if (static_cast<i32>(width) != m.width || static_cast<i32>(height) != m.height) mism("class " + std::to_string(k) + " size");
            if (!m.named || !grid) continue;
            std::vector<u32> gw(m.grid.size());
            r.read(grid, gw.data(), gw.size() * 4);
            for (i32 z = 0; z < m.height && bad.empty(); ++z)
                for (i32 x = 0; x < m.width; ++x) {
                    const u32 o = (gw[static_cast<std::size_t>((z >> 4) * m.width + x)] >> ((z & 15) * 2)) & 3u;
                    if (o != m.grade(x, z)) {
                        mism("class " + std::to_string(k) + " grade at (" + std::to_string(x) + "," + std::to_string(z) + ") orig " + std::to_string(o) +
                             " ours " + std::to_string(m.grade(x, z)));
                        break;
                    }
                }
        }
        // Program-wide counters.
        if (static_cast<i32>(r.r32(kRefreshCounter)) != search.refresh_counter) mism("refresh counter");
        {
            std::array<i32, 10> sc{}, ld{};
            r.read(kScales, sc.data(), 40);
            r.read(kLoads, ld.data(), 40);
            if (sc != search.estimate_scale) mism("estimate scales");
            if (ld != search.load) mism("loads");
        }
        // Search object.
        std::uint8_t so[0xC9];
        r.read(L.search, so, sizeof so);
        auto s32 = [&](u32 off) { i32 v; std::memcpy(&v, so + off, 4); return v; };
        const u32 pool = static_cast<u32>(s32(0)), heap = static_cast<u32>(s32(4));
        const sim::RouteSearch& s = search;
        auto chk = [&](const char* name, i32 o, i32 e) {
            if (o != e) mism(std::string("search ") + name + " orig " + std::to_string(o) + " ours " + std::to_string(e));
        };
        chk("free head", s32(8), s.free_head);
        chk("pool count", s32(0xC), s.pool_count);
        chk("open count", s32(0x14), s.open_count);
        chk("hold", s32(0x18), s.hold);
        chk("width", s32(0x20), s.width);
        chk("height", s32(0x24), s.height);
        chk("cell count", s32(0x28), s.cell_count);
        chk("start", s32(0x30), static_cast<i32>(s.start_cell));
        chk("found", s32(0x34), static_cast<i32>(s.found_cell));
        chk("nearest x", s32(0x38), s.nearest_x);
        chk("nearest z", s32(0x3C), s.nearest_z);
        chk("probe best", s32(0x40), s.probe_best);
        chk("turn range", s32(0x44), s.turn_range);
        chk("divisor", s32(0x48), s.budget_divisor);
        chk("work", s32(0x4C), s.work);
        chk("scale", s32(0x50), s.scale);
        chk("base", s32(0x54), s.base_budget);
        chk("active unit", unit_id_of(static_cast<u32>(s32(0x58))), s.active ? s.active->id : -1);
        chk("goal", s32(0x5C), static_cast<i32>(goal_address(s.goal)));
        // (Left over after a search ends; the order goal may have been deleted since, so only while serving.)
        if (s.active) chk("order goal", s32(0x60), static_cast<i32>(s.order_goal ? s.order_goal->address : 0u));
        chk("class", s32(0x64) ? static_cast<i32>((static_cast<u32>(s32(0x64)) - kMoveInfo) / 0x20) : -1, s.move_class);
        for (int k = 0; k < 8; ++k) {
            chk("turn cost", so[0x68 + k], s.turn_cost[static_cast<std::size_t>(k)]);
            chk("dir cost", so[0x70 + k], s.dir_cost[static_cast<std::size_t>(k)]);
        }
        chk("player", so[0x78], s.player);
        for (int p = 0; p < 10; ++p) {
            const i32 cur = s32(0x79 + 4 * static_cast<u32>(p));
            chk("cursor", cur ? unit_id_of(static_cast<u32>(cur)) : 0, s.unit_cursor[static_cast<std::size_t>(p)]);
            chk("credit", s32(0xA1 + 4 * static_cast<u32>(p)), s.credit[static_cast<std::size_t>(p)]);
        }
        // Pool and heap.
        if (bad.empty() && s.pool_count > 0) {
            std::vector<std::uint8_t> pb(static_cast<std::size_t>(s.pool_count) * 20);
            r.read(pool, pb.data(), pb.size());
            for (i32 k = 0; k < s.pool_count; ++k) {
                const std::uint8_t* p = &pb[static_cast<std::size_t>(k) * 20];
                i32 hi, g0, f0;
                u32 cell;
                i16 step, depth;
                std::memcpy(&hi, p, 4);
                std::memcpy(&cell, p + 4, 4);
                std::memcpy(&g0, p + 8, 4);
                std::memcpy(&f0, p + 12, 4);
                std::memcpy(&step, p + 16, 2);
                std::memcpy(&depth, p + 18, 2);
                const sim::RouteNode& n = s.pool[static_cast<std::size_t>(k)];
                if (hi != n.heap_index || cell != n.cell || g0 != n.g || f0 != n.f || depth != n.depth || (n.g != 0 && step != n.step)) {
                    mism("pool node " + std::to_string(k) + " orig (" + std::to_string(hi) + "," + std::to_string(cell) + "," + std::to_string(g0) + "," +
                         std::to_string(f0) + "," + std::to_string(step) + "," + std::to_string(depth) + ") ours (" + std::to_string(n.heap_index) + "," +
                         std::to_string(n.cell) + "," + std::to_string(n.g) + "," + std::to_string(n.f) + "," + std::to_string(n.step) + "," +
                         std::to_string(n.depth) + ")");
                    break;
                }
            }
        }
        if (bad.empty() && s.open_count > 0) {
            std::vector<u32> hb(static_cast<std::size_t>(s.open_count));
            r.read(heap, hb.data(), hb.size() * 4);
            for (i32 k = 0; k < s.open_count; ++k)
                if (static_cast<i32>((hb[static_cast<std::size_t>(k)] - pool) / 20) != s.heap[static_cast<std::size_t>(k)]) {
                    mism("open heap entry " + std::to_string(k));
                    break;
                }
        }
        // Cell states and the touched bitmap.
        if (bad.empty()) {
            const u32 cs = static_cast<u32>(s32(0x1C)), tb = static_cast<u32>(s32(0x2C));
            std::vector<std::uint8_t> cb(static_cast<std::size_t>(s.cell_count) * 4);
            r.read(cs, cb.data(), cb.size());
            for (i32 k = 0; k < s.cell_count; ++k) {
                const std::uint8_t* p = &cb[static_cast<std::size_t>(k) * 4];
                u16 node;
                std::memcpy(&node, p + 2, 2);
                if (p[0] != s.cell_flags[static_cast<std::size_t>(k)] || p[1] != s.cell_dir[static_cast<std::size_t>(k)] ||
                    node != s.cell_node[static_cast<std::size_t>(k)]) {
                    mism("cell state " + std::to_string(k) + " (" + std::to_string(k % W) + "," + std::to_string(k / W) + ") orig " +
                         std::to_string(p[0]) + "/" + std::to_string(p[1]) + "/" + std::to_string(node) + " ours " +
                         std::to_string(s.cell_flags[static_cast<std::size_t>(k)]) + "/" + std::to_string(s.cell_dir[static_cast<std::size_t>(k)]) + "/" +
                         std::to_string(s.cell_node[static_cast<std::size_t>(k)]));
                    break;
                }
            }
            std::vector<u32> tw(s.touched.size());
            r.read(tb, tw.data(), tw.size() * 4);
            if (tw != s.touched) mism("touched bitmap");
        }
        // Units, route goals, orders.
        for (int i = 1; i < kUnits; ++i) {
            const sim::UnitTickState& t = ticks[static_cast<std::size_t>(i)];
            if (!t.has_motion) continue;
            if (r.r32(obj(i, obMotion) + mClaim) != t.motion.claim_tick) mism("unit " + std::to_string(i) + " footprint step");
            const auto& rg = rgoals[static_cast<std::size_t>(i)];
            if (!rg) continue;
            std::uint8_t b[0x68];
            r.read(rg->address, b, sizeof b);
            i32 count;
            u32 stick;
            std::memcpy(&count, b + 0x5C, 4);
            std::memcpy(&stick, b + 0x60, 4);
            if (count != rg->count) mism("unit " + std::to_string(i) + " route count orig " + std::to_string(count) + " ours " + std::to_string(rg->count));
            for (i32 k = 0; k < count && k < 20 && k < rg->count; ++k) {
                u32 p;
                std::memcpy(&p, b + 0xC + 4 * k, 4);
                if (p != rg->points[static_cast<std::size_t>(k)]) {
                    mism("unit " + std::to_string(i) + " route point " + std::to_string(k));
                    break;
                }
            }
            if (stick != rg->search_tick) mism("unit " + std::to_string(i) + " route search step");
            if ((b[0x64] & 0xF) != (rg->flags & 0xF)) mism("unit " + std::to_string(i) + " route flags");
            const auto& o = orders[static_cast<std::size_t>(i)];
            if (o && r.r32(o->address + oEvents) != o->events) mism("unit " + std::to_string(i) + " order events");
        }
        if (!bad.empty()) {
            std::printf("scenario %u %s: %s\n", seed, where.c_str(), bad.c_str());
            ++st.mismatches;
            return false;
        }
        return true;
    };

    if (engine_faulted()) return true;
    if (!compare("after the build")) return true;

    auto pack = [](i32 x, i32 z) { return (static_cast<u32>(x) & 0xFFFFu) | (static_cast<u32>(z) << 16); };
    for (int step = 0; step < steps; ++step) {
        env.game_tick += rnd(6) == 0 ? static_cast<u32>(1 + rnd(40)) : 1u;
        sync_env();
        const int nops = rnd(4);
        for (int op = 0; op < nops; ++op) {
            const int kind = rnd(100);
            std::string name;
            const int i = 1 + rnd(kUnits - 1);
            sim::Unit& u = units[static_cast<std::size_t>(i)];
            sim::UnitTickState& t = ticks[static_cast<std::size_t>(i)];
            auto& rg = rgoals[static_cast<std::size_t>(i)];
            if (kind < 30) {
                name = "ask for a route";
                if (!rg || !rg->attached) continue;  // a goal without an order goal never wants a route
                rg->flags = static_cast<u8>(rg->flags | sim::route_flags::kWantsRoute);
                rg->search_tick = env.game_tick - static_cast<u32>(rnd(100));
                write_goal(i);
            } else if (kind < 42) {
                name = "new order goal";
                if (!rg) continue;
                if (!r.call(kForget, L.search, {rg->address})) { std::printf("original call failed at line %d\n", __LINE__); return false; }
                sim::route_search_forget(search, *rg);
                set_order_goal(i);
                rg->flags = static_cast<u8>(rg->flags | sim::route_flags::kWantsRoute);
                write_goal(i);
            } else if (kind < 47) {
                name = "forget";
                if (!rg) continue;
                if (!r.call(kForget, L.search, {rg->address})) { std::printf("original call failed at line %d\n", __LINE__); return false; }
                sim::route_search_forget(search, *rg);
            } else if (kind < 62) {
                name = "unit moves";
                if (!t.has_motion || !(u.flags & sim::unit_flags::kAlive)) continue;
                occupy(i, false);
                write_cells();
                if (!r.call(kAfterRelease, 0, {L.units + static_cast<u32>(i) * kUnitSize})) { std::printf("original call failed at line %d\n", __LINE__); return false; }
                sim::passability_after_release(c, u);
                u.cell_x = static_cast<i16>(std::clamp(u.cell_x + rnd(5) - 2, 0, W - 1));
                u.cell_z = static_cast<i16>(std::clamp(u.cell_z + rnd(5) - 2, 0, H - 1));
                occupy(i, true);
                t.motion.claim_tick = env.game_tick;
                write_cells();
                write_unit(i);
                if (!r.call(kUpdateAll, 0, {pack(u.cell_x, u.cell_z), pack(u.footprint_x, u.footprint_z)})) { std::printf("original call failed at line %d\n", __LINE__); return false; }
                sim::update_passability(c, u.cell_x, u.cell_z, u.footprint_x, u.footprint_z);
            } else if (kind < 67) {
                name = "release without motion";
                if (t.type_index == 0) continue;
                const bool had = t.has_motion;
                t.has_motion = false;
                write_unit(i);
                if (!r.call(kAfterRelease, 0, {L.units + static_cast<u32>(i) * kUnitSize})) { std::printf("original call failed at line %d\n", __LINE__); return false; }
                sim::passability_after_release(c, u);
                t.has_motion = had;
                write_unit(i);
            } else if (kind < 72) {
                name = "alive flag";
                u.flags ^= sim::unit_flags::kAlive;
                write_unit(i);
            } else if (kind < 77) {
                name = "rectangle update";
                const i16 x = static_cast<i16>(rnd(W + 4) - 2), z = static_cast<i16>(rnd(H + 4) - 2), w = static_cast<i16>(rnd(6)),
                          h = static_cast<i16>(rnd(6));
                if (!r.call(kUpdateAll, 0, {pack(x, z), pack(w, h)})) { std::printf("original call failed at line %d\n", __LINE__); return false; }
                sim::update_passability(c, x, z, w, h);
            } else if (kind < 82) {
                name = "feature change";
                const int x = rnd(W), z = rnd(H);
                sim::MapCell& cell = map.cells[static_cast<std::size_t>(z * W + x)];
                if (cell.feature == 0xFFFE) continue;
                cell.feature = rnd(2) ? 0xFFFF : static_cast<u16>(rnd(ftypes));
                write_cells();
                if (!r.call(kUpdateAll, 0, {pack(x, z), pack(1, 1)})) { std::printf("original call failed at line %d\n", __LINE__); return false; }
                sim::update_passability(c, static_cast<i16>(x), static_cast<i16>(z), 1, 1);
            } else if (kind < 85) {
                name = "explored change";
                if (explored.empty()) continue;
                explored[static_cast<std::size_t>(rnd(static_cast<int>(explored.size())))] = static_cast<u16>(g());
                r.write(L.explored, explored.data(), explored.size() * 2);
            } else {
                name = "placement check";
                const u16 type = static_cast<u16>(rnd(11));
                const u16 ex = static_cast<u16>(rnd(kUnits));
                const i16 x = static_cast<i16>(rnd(W + 6) - 3), z = static_cast<i16>(rnd(H + 6) - 3);
                const u32 mode = static_cast<u32>(rnd(4));
                sys.yard_answer = rnd(2) != 0;
                r.w32(L.ret_val, sys.yard_answer ? 1u : 0u);
                r.w32(L.ret_val + 4, 0);
                sys.yard_calls = 0;
                u32 res = 0;
                if (!r.call(kCanPlace, 0, {L.defs + type * kDefSize, ex, pack(x, z), mode}, &res)) { std::printf("original call failed at line %d\n", __LINE__); return false; }
                const bool ours = sim::can_place_footprint(c, type, ex, x, z, mode);
                ++st.places;
                st.place_true += ours;
                st.yard_calls += static_cast<std::size_t>(sys.yard_calls);
                if ((res != 0) != ours || static_cast<int>(r.r32(L.ret_val + 4)) != sys.yard_calls) {
                    return fail("step " + std::to_string(step) + " placement check type " + std::to_string(type) + " at (" + std::to_string(x) + "," +
                                std::to_string(z) + ") mode " + std::to_string(mode) + ": orig " + std::to_string(res) + " ours " + std::to_string(ours));
                }
            }
            ++st.ops;
            ++st.by_op[name];
            if (engine_faulted()) return true;
            if (!compare("step " + std::to_string(step) + " after " + name)) return true;
        }
        // The route search's step.
        std::vector<u32> asked(kUnits, 0);
        for (int i = 1; i < kUnits; ++i)
            if (rgoals[static_cast<std::size_t>(i)]) asked[static_cast<std::size_t>(i)] = rgoals[static_cast<std::size_t>(i)]->search_tick;
        const i32 routes_before = 0;
        (void)routes_before;
        std::vector<i32> counts(kUnits, -1);
        std::vector<u32> events(kUnits, 0);
        for (int i = 1; i < kUnits; ++i) {
            if (rgoals[static_cast<std::size_t>(i)]) counts[static_cast<std::size_t>(i)] = rgoals[static_cast<std::size_t>(i)]->count;
            if (orders[static_cast<std::size_t>(i)]) events[static_cast<std::size_t>(i)] = orders[static_cast<std::size_t>(i)]->events;
        }
        sim::run_route_search(c);
        if (engine_faulted()) return true;
        if (!r.call(kSearchTick, L.search, {})) {
            std::printf("scenario %u step %d: the original's route search step failed (engine: active %d, open %d, work %d)\n", seed, step,
                        search.active ? search.active->id : -1, search.open_count, search.work);
            return false;
        }
        ++st.steps;
        if (!compare("step " + std::to_string(step) + " route search")) return true;
        for (int i = 1; i < kUnits; ++i) {
            const auto& rg = rgoals[static_cast<std::size_t>(i)];
            if (rg && rg->search_tick == env.game_tick && asked[static_cast<std::size_t>(i)] != env.game_tick) ++st.searches;
        }
        if (search.active) ++st.rounds;
        st.max_open = std::max(st.max_open, static_cast<std::size_t>(search.open_count));
        for (int i = 1; i < kUnits; ++i) {
            const auto& o = orders[static_cast<std::size_t>(i)];
            if (!o) continue;
            const u32 gained = o->events & ~events[static_cast<std::size_t>(i)];
            if (gained & 0x40) ++st.no_routes;
            if (gained & 0x100) ++st.probe_hits;
            if (gained & 0x200) ++st.probe_partial;
        }
        for (int i = 1; i < kUnits; ++i) {
            const auto& rg = rgoals[static_cast<std::size_t>(i)];
            if (rg && (rg->flags & sim::route_flags::kChanged) && rg->count != counts[static_cast<std::size_t>(i)] && rg->count > 0) ++st.routes;
        }
    }
    ++st.scenarios;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: route_harness <TotalA.exe> <game_dir> [scenarios] [steps] [first seed]\n");
        return 2;
    }
    const int scenarios = argc > 3 ? std::atoi(argv[3]) : 10;
    const int steps = argc > 4 ? std::atoi(argv[4]) : 40;
    const int first = argc > 5 ? std::atoi(argv[5]) : 0;
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[2]);
    ta::UnitDefContext ctx;
    ta::load_unit_context(vfs, ctx);
    harness::Remote r;
    Layout L;
    if (!r.start(argv[1]) || !setup(r, L)) {
        std::fprintf(stderr, "cannot start the original\n");
        return 1;
    }
    Stats st;
    for (int s = 0; s < scenarios; ++s) {
        if (!run_scenario(r, L, ctx, steps, static_cast<unsigned>(first + s), st)) {
            ++st.failures;
            std::printf("scenario %d: original call failed - restarting\n", first + s);
            r.stop();
            if (!r.start(argv[1]) || !setup(r, L)) return 1;
        }
    }
    std::printf("scenarios %zu, route search steps %zu, operations %zu\n", st.scenarios, st.steps, st.ops);
    std::printf("route requests taken %zu, steps ending with a search unfinished %zu, largest open set %zu, routes given %zu, no-route answers %zu, "
                "probe reached the goal %zu, probe stopped short %zu\n",
                st.searches, st.rounds, st.max_open, st.routes, st.no_routes, st.probe_hits, st.probe_partial);
    std::printf("placement checks %zu (%zu allowed, %zu yard-map calls)\n", st.places, st.place_true, st.yard_calls);
    std::printf("operations:\n");
    for (const auto& [k, v] : st.by_op) std::printf("  %s: %zu\n", k.c_str(), v);
    std::printf("mismatching scenarios %zu, engine faults %zu, original failures %zu\n", st.mismatches, st.faults, st.failures);
    return st.mismatches || st.failures ? 1 : 0;
}
