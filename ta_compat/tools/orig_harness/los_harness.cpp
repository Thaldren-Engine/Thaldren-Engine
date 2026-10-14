// Differential tester for line of sight, the explored map, radar, sonar,
// jamming and the cloak hold-off (sim/sight.h). Inside a suspended TotalA.exe it
// builds a random map (heights, sea level), the game's real ring table
// (gamedata\los.tdf) and sight masks (anims\vismasks.gaf), four players (random
// controllers, alliances, radar sharing and watch flags, sight count maps), 32
// units of random synthetic types (sight, radar, sonar, jammers, cloak distance,
// stealth, model boxes) and the players' visible-enemy lists, then runs the
// original height band build. Each step applies random operations through the
// original routines and the engine: unit moves with the sight update, new-unit
// stamps, deaths (lingering sight and removal), frame expiry of lingering sight,
// visibility tests, explored tests, the sensor pass, rebuilds, option toggles,
// viewed-player changes and map sharing; after every operation it compares the
// explored map, every player's sight counts, the units' sight fields, flags and
// cloak hold-offs, the lingering slots and the option flags.
//
// Usage: los_harness <TotalA.exe> <game_dir> [scenarios] [steps] [first seed]

#define NOMINMAX
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/footprint.h"
#include "sim/heightmap.h"
#include "sim/sight.h"
#include "sim/targeting.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"
#include "ta/gaf.h"
#include "ta/hpi.h"
#include "ta/sight_tables.h"

namespace {

using harness::u32;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using i16 = std::int16_t;
using i32 = std::int32_t;

constexpr u32 kGameStatePtr = 0x00511DE8, kRingTable = 0x0051E6A0, kUnitTables = 0x005119C0;
constexpr u32 kAlloc = 0x004B4F10, kFree = 0x004B4F20, kMinimapFog = 0x00466C20, kMinimapFinal = 0x00466DC0;
constexpr u32 kBuildLayers = 0x00482C20, kRebuild = 0x004816A0, kUpdate = 0x004827B0, kNewUnit = 0x00482AC0, kLinger = 0x00482910,
              kRemove = 0x00482090, kExpire = 0x00482130, kVisible = 0x00465AC0, kExplored = 0x00408090, kSensors = 0x00467440,
              kShare = 0x00485420;
constexpr u32 kUnitSize = 0x118, kPlayerSize = 0x14B, kDefSize = 0x249, kSideSize = 0xC0, kTblSize = 0x110;
constexpr int kUnits = 33;  // entries 0..32: four players of eight
constexpr int kTypes = 10;  // type 0 is the blank type
constexpr int kPlayers = 4;

enum : u32 {
    gPlayerCount = 0x2A3C, gLocal = 0x2A42, gView = 0x2A43, gPlayers = 0x1B63, gMapW = 0x14233, gMapH = 0x14237, gWorldW = 0x14223,
    gWorldH = 0x14227, gExplored = 0x14273, gLingerCount = 0x14277, gLinger = 0x1427B, gSea = 0x1427F, gFlags = 0x14281,
    gHeights = 0x14287, gBand = 0x1428F, gGrid = 0x1429F, gUnitBase = 0x14357, gUnitEnd = 0x1435B, gMasks = 0x1485B, gTick = 0x38A47,
    pActive = 0x00, pSide = 0x27, pFirst = 0x67, pLast = 0x6B, pFirstId = 0x6F, pLastId = 0x71, pController = 0x73, pCounts = 0x7C,
    pCountW = 0x80, pCountH = 0x84, pCountSize = 0x88, pAllies = 0x108, pState = 0x146,
    uPosX = 0x6A, uPosY = 0x6E, uPosZ = 0x72, uCellX = 0x7A, uCellZ = 0x7C, uCargoNext = 0x8E, uDef = 0x92, uOwner = 0x96,
    uDefIndex = 0xA6, uId = 0xA8, uCloakHold = 0xB0, uFrame = 0xF8, uOwnerSlot = 0xFF, uActivate = 0x10E, uFlags = 0x110,
    sHostFlags = 151, sGameOpt = 155,
};

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8v(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void align() { while (b.size() % 16) b.push_back(0xCC); }
};

struct Layout {
    u32 heap = 0, bump_ptr = 0, stubs = 0, gs = 0, units = 0, defs = 0, sides = 0, tables = 0, lists = 0, linger = 0, gaf = 0, gaf_seq = 0,
        rings = 0, pos = 0;
};

const sim::SightTables* g_tables = nullptr;
std::vector<std::uint8_t> g_gaf;

bool setup(harness::Remote& r, Layout& L) {
    L.heap = r.alloc(32u << 20);
    L.bump_ptr = r.alloc(0x1000);
    L.stubs = r.alloc(0x1000, PAGE_EXECUTE_READWRITE);
    L.gs = r.alloc(0x40000);
    L.units = r.alloc(static_cast<SIZE_T>(kUnits + 1) * kUnitSize);
    L.defs = r.alloc(static_cast<SIZE_T>(kTypes) * kDefSize);
    L.sides = r.alloc(static_cast<SIZE_T>(kPlayers + 1) * kSideSize);
    L.tables = r.alloc(static_cast<SIZE_T>(kPlayers) * kTblSize);
    L.lists = r.alloc(static_cast<SIZE_T>(kPlayers) * kUnits * 4);
    L.linger = r.alloc(20 * 36);
    L.gaf = r.alloc(g_gaf.size() + 16);
    L.pos = r.alloc(0x100);
    if (!L.heap || !L.stubs || !L.gs || !L.units || !L.defs || !L.gaf) return false;
    Emitter e;
    auto here = [&]() { return L.stubs + static_cast<u32>(e.b.size()); };
    {  // malloc: a bump heap (size at [esp+4])
        const u32 at = here();
        e.u8v(0xA1); e.u32le(L.bump_ptr);
        e.u8v(0x8B); e.u8v(0x4C); e.u8v(0x24); e.u8v(0x04);
        e.u8v(0x83); e.u8v(0xC1); e.u8v(0x03); e.u8v(0x83); e.u8v(0xE1); e.u8v(0xFC);
        e.u8v(0x01); e.u8v(0x0D); e.u32le(L.bump_ptr);
        e.u8v(0xC3);
        e.align();
        if (!r.patch_jump(kAlloc, at)) return false;
    }
    {  // free and the minimap redraws: nothing
        const u32 at = here();
        e.u8v(0xC3);
        e.align();
        if (!r.patch_jump(kFree, at) || !r.patch_jump(kMinimapFog, at) || !r.patch_jump(kMinimapFinal, at)) return false;
    }
    if (!r.write(L.stubs, e.b.data(), e.b.size())) return false;
    r.w32(kGameStatePtr, L.gs);
    // The sight masks: the GAF file relocated in place (sequence "vismask").
    if (!r.write(L.gaf, g_gaf.data(), g_gaf.size())) return false;
    ta::GafDirectory dir;
    ta::gaf_read_directory(g_gaf, dir);
    const int seq = ta::gaf_find_sequence(dir, "vismask");
    u32 entry = 0;
    std::memcpy(&entry, &g_gaf[12 + static_cast<std::size_t>(seq) * 4], 4);
    u16 frames = 0;
    std::memcpy(&frames, &g_gaf[entry], 2);
    for (u16 i = 0; i < frames; ++i) {
        u32 fp = 0, data = 0;
        std::memcpy(&fp, &g_gaf[entry + 40 + i * 8u], 4);
        std::memcpy(&data, &g_gaf[fp + 16], 4);
        r.w32(L.gaf + entry + 40 + i * 8u, L.gaf + fp);
        r.w32(L.gaf + fp + 16, L.gaf + data);
    }
    L.gaf_seq = L.gaf + entry;
    // The ring table: a vector of rings, each a vector of slices, each a vector of points. The
    // 16 bytes in front of the rings (read for sight below 32) describe an empty ring, as the
    // engine's default stands in for them.
    const auto& rings = g_tables->rings;
    std::size_t bytes = 16 + rings.size() * 16;
    for (const auto& ring : rings) {
        bytes += ring.size() * 16;
        for (const auto& sl : ring) bytes += sl.size() * 4;
    }
    L.rings = r.alloc(bytes + 64);
    std::vector<std::uint8_t> img(bytes, 0);
    auto put = [&](std::size_t at, u32 v) { std::memcpy(&img[at], &v, 4); };
    const u32 base = L.rings;
    std::size_t ring_at = 16, cursor = 16 + rings.size() * 16;
    for (const auto& ring : rings) {
        const std::size_t slices_at = cursor;
        cursor += ring.size() * 16;
        put(ring_at + 4, ring.empty() ? 0 : base + static_cast<u32>(slices_at));
        put(ring_at + 8, ring.empty() ? 0 : base + static_cast<u32>(cursor));
        put(ring_at + 12, ring.empty() ? 0 : base + static_cast<u32>(cursor));
        for (std::size_t k = 0; k < ring.size(); ++k) {
            const auto& sl = ring[k];
            const std::size_t pts = cursor;
            for (const auto& p : sl) {
                const u32 v = static_cast<u16>(p.x) | static_cast<u32>(static_cast<u16>(p.z)) << 16;
                put(cursor, v);
                cursor += 4;
            }
            put(slices_at + k * 16 + 4, sl.empty() ? 0 : base + static_cast<u32>(pts));
            put(slices_at + k * 16 + 8, sl.empty() ? 0 : base + static_cast<u32>(cursor));
            put(slices_at + k * 16 + 12, sl.empty() ? 0 : base + static_cast<u32>(cursor));
        }
        ring_at += 16;
    }
    r.write(L.rings, img.data(), img.size());
    r.w32(kRingTable + 4, base + 16);
    r.w32(kRingTable + 8, base + 16 + static_cast<u32>(rings.size() * 16));
    r.w32(kRingTable + 12, base + 16 + static_cast<u32>(rings.size() * 16));
    return true;
}

struct Stats {
    std::size_t scenarios = 0, steps = 0, ops = 0, mismatches = 0, failures = 0, faults = 0, visible_true = 0, explored_true = 0,
                radar_marks = 0, sonar_marks = 0, jammed = 0, holds = 0, lingering_max = 0, crashes_confirmed = 0, heap_reads = 0;
    std::map<std::string, std::size_t> by_op;
};

struct TypeData {
    sim::UnitTypeInfo info;
    sim::SightType sight;
};

struct Scenario {
    int W = 0, H = 0;
    std::vector<TypeData> types;
    std::vector<sim::SightType> sight_types;
    sim::HeightMap heights;
    std::vector<sim::Unit> units;
    std::vector<sim::UnitTickState> ticks;
    std::vector<sim::PlayerSlot> players;
    std::vector<std::array<u8, 16>> alliances;
    sim::UnitBuckets buckets;
    sim::TargetingState targeting;
    sim::SightState state;
    std::vector<u16> explored;
    sim::SightEnvironment env;
};

u32 unit_addr(const Layout& L, int id) { return L.units + static_cast<u32>(id) * kUnitSize; }
u32 player_addr(const Layout& L, int p) { return L.gs + gPlayers + static_cast<u32>(p) * kPlayerSize; }

void write_unit(harness::Remote& r, const Layout& L, const Scenario& s, int id) {
    const sim::Unit& u = s.units[static_cast<std::size_t>(id)];
    const sim::UnitTickState& t = s.ticks[static_cast<std::size_t>(id)];
    const u32 a = unit_addr(L, id);
    r.w32(a + uPosX, static_cast<u32>(u.pos[0]));
    r.w32(a + uPosY, static_cast<u32>(u.pos[1]));
    r.w32(a + uPosZ, static_cast<u32>(u.pos[2]));
    r.write(a + uCellX, &u.los_cell_x, 2);
    r.write(a + uCellZ, &u.los_cell_z, 2);
    r.w32(a + uDef, L.defs + t.type_index * kDefSize);
    r.w32(a + uOwner, player_addr(L, t.owner));
    r.write(a + uDefIndex, &t.type_index, 2);
    r.write(a + uId, &u.id, 2);
    r.w32(a + uCloakHold, u.cloak_holdoff_tick);
    r.write(a + uFrame, &u.los_frame, 1);
    const u8 owner = static_cast<u8>(t.owner);
    r.write(a + uOwnerSlot, &owner, 1);
    r.write(a + uActivate, &u.activate_flags, 1);
    r.w32(a + uFlags, u.flags);
}

void write_buckets(harness::Remote& r, const Layout& L, Scenario& s) {
    // Units filed newest first by position (both sides), links through +0x8E.
    s.buckets.resize_for_map(s.W, s.H);
    std::fill(s.buckets.heads.begin(), s.buckets.heads.end(), nullptr);
    for (auto& u : s.units) u.cargo_next = nullptr;
    const u32 grid = r.r32(L.gs + gGrid);
    for (std::size_t k = 0; k < s.buckets.heads.size(); ++k) r.w32(grid + static_cast<u32>(k) * 10 + 6, 0);
    for (int id = 1; id < kUnits; ++id) {
        sim::Unit& u = s.units[static_cast<std::size_t>(id)];
        r.w32(unit_addr(L, id) + uCargoNext, 0);
        if (!(u.flags & sim::sight_unit_flags::kAlive)) continue;
        const i32 bx = u.pos[0] >> 23, bz = u.pos[2] >> 23;
        if (bx < 0 || bz < 0 || bx >= s.buckets.width || bz >= s.buckets.height) continue;
        const std::size_t k = static_cast<std::size_t>(bz) * static_cast<std::size_t>(s.buckets.width) + static_cast<std::size_t>(bx);
        u.cargo_next = s.buckets.heads[k];
        const u32 head = r.r32(grid + static_cast<u32>(k) * 10 + 6);
        r.w32(unit_addr(L, id) + uCargoNext, head);
        s.buckets.heads[k] = &u;
        r.w32(grid + static_cast<u32>(k) * 10 + 6, unit_addr(L, id));
    }
}

void write_lists(harness::Remote& r, const Layout& L, const Scenario& s) {
    for (int p = 0; p < kPlayers; ++p) {
        const u32 tb = L.tables + static_cast<u32>(p) * kTblSize;
        const auto& seen = s.targeting.tables[static_cast<std::size_t>(p)].seen;
        const u32 list = L.lists + static_cast<u32>(p) * kUnits * 4;
        for (std::size_t i = 0; i < seen.size(); ++i) r.w32(list + static_cast<u32>(i) * 4, unit_addr(L, seen[i]->id));
        r.w32(tb + 9, list);
        r.w32(tb + 0xD, list + static_cast<u32>(seen.size()) * 4);
        r.w32(kUnitTables + static_cast<u32>(p) * 4, s.targeting.tables[static_cast<std::size_t>(p)].exists ? tb : 0);
    }
}

void write_players(harness::Remote& r, const Layout& L, const Scenario& s) {
    for (int p = 0; p < kPlayers; ++p) {
        const sim::PlayerSlot& ps = s.players[static_cast<std::size_t>(p)];
        const u32 a = player_addr(L, p);
        r.w32(a + pActive, ps.active ? 1 : 0);
        r.write(a + pController, &ps.controller, 1);
        r.write(a + pState, &ps.state, 1);
        r.write(a + pAllies, s.alliances[static_cast<std::size_t>(p)].data(), 11);
        const u32 side = L.sides + static_cast<u32>(p) * kSideSize;
        r.w32(a + pSide, side);
        const u8 host = s.env.options[static_cast<std::size_t>(p)].shares_radar ? 0x40 : 0;
        const u16 opt = s.env.options[static_cast<std::size_t>(p)].watching ? 0x40 : 0;
        r.write(side + sHostFlags, &host, 1);
        r.write(side + sGameOpt, &opt, 2);
    }
    r.write(L.gs + gView, &s.env.view_player, 1);
    r.write(L.gs + gPlayerCount, &s.env.player_count, 2);
    r.w32(L.gs + gTick, s.env.game_tick);
    r.write(L.gs + gFlags, &s.state.flags, 2);
}

struct Compare {
    harness::Remote& r;
    const Layout& L;
    Scenario& s;
    std::string what;
    bool fail(const std::string& m) {
        if (what.empty()) what = m;
        return false;
    }
    bool all() {
        what.clear();
        const u16 flags = r.rd<u16>(L.gs + gFlags);
        if (flags != s.state.flags) return fail("flags " + std::to_string(flags) + " vs " + std::to_string(s.state.flags));
        const u32 ex = r.r32(L.gs + gExplored);
        std::vector<u16> e(s.explored.size());
        r.read(ex, e.data(), e.size() * 2);
        for (std::size_t i = 0; i < e.size(); ++i)
            if (e[i] != s.explored[i]) return fail("explored[" + std::to_string(i) + "] " + std::to_string(e[i]) + " vs " + std::to_string(s.explored[i]));
        for (int p = 0; p < kPlayers; ++p) {
            const auto& ps = s.state.players[static_cast<std::size_t>(p)];
            if (!ps.allocated) continue;
            std::vector<u8> c(ps.counts.size());
            r.read(r.r32(player_addr(L, p) + pCounts), c.data(), c.size());
            for (std::size_t i = 0; i < c.size(); ++i)
                if (c[i] != ps.counts[i])
                    return fail("counts p" + std::to_string(p) + "[" + std::to_string(i) + "] " + std::to_string(c[i]) + " vs " +
                                std::to_string(ps.counts[i]));
        }
        for (int id = 1; id < kUnits; ++id) {
            const sim::Unit& u = s.units[static_cast<std::size_t>(id)];
            const u32 a = unit_addr(L, id);
            const i16 cx = r.rd<i16>(a + uCellX), cz = r.rd<i16>(a + uCellZ);
            const u8 fr = r.rd<u8>(a + uFrame);
            const u32 fl = r.r32(a + uFlags), hold = r.r32(a + uCloakHold);
            if (cx != u.los_cell_x || cz != u.los_cell_z || fr != u.los_frame)
                return fail("unit " + std::to_string(id) + " sight cell/frame " + std::to_string(cx) + "," + std::to_string(cz) + "," +
                            std::to_string(fr) + " vs " + std::to_string(u.los_cell_x) + "," + std::to_string(u.los_cell_z) + "," +
                            std::to_string(u.los_frame));
            if (fl != u.flags) return fail("unit " + std::to_string(id) + " flags " + std::to_string(fl) + " vs " + std::to_string(u.flags));
            if (hold != u.cloak_holdoff_tick) return fail("unit " + std::to_string(id) + " cloak hold-off");
        }
        const i32 count = static_cast<i32>(r.r32(L.gs + gLingerCount));
        if (count != s.state.lingering_count) return fail("lingering count " + std::to_string(count) + " vs " + std::to_string(s.state.lingering_count));
        std::vector<std::uint8_t> raw(20 * 36);
        r.read(L.linger, raw.data(), raw.size());
        for (int i = 0; i < 20; ++i) {
            const auto& e2 = s.state.lingering[static_cast<std::size_t>(i)];
            const std::uint8_t* b = &raw[static_cast<std::size_t>(i) * 36];
            u32 player = 0, x = 0, y = 0, z = 0, expire = 0;
            i16 sight = 0, cx = 0, cz = 0;
            std::memcpy(&player, b, 4);
            std::memcpy(&sight, b + 8, 2);
            std::memcpy(&x, b + 16, 4);
            std::memcpy(&y, b + 20, 4);
            std::memcpy(&z, b + 24, 4);
            std::memcpy(&expire, b + 28, 4);
            std::memcpy(&cx, b + 32, 2);
            std::memcpy(&cz, b + 34, 2);
            const int player_index = player == 0 ? 0 : static_cast<int>((player - (L.gs + gPlayers)) / kPlayerSize);
            if (player_index != e2.player || sight != e2.sight || b[10] != e2.eye || b[11] != e2.frame || static_cast<i32>(x) != e2.pos[0] ||
                static_cast<i32>(y) != e2.pos[1] || static_cast<i32>(z) != e2.pos[2] || expire != e2.expire_tick || cx != e2.cell_x ||
                cz != e2.cell_z)
                return fail("lingering slot " + std::to_string(i));
        }
        return true;
    }
};

bool run_scenario(harness::Remote& r, const Layout& L, int steps, unsigned seed, Stats& st) {
    std::mt19937 g(seed);
    auto rnd = [&](int n) { return n <= 1 ? 0 : static_cast<int>(g() % static_cast<u32>(n)); };
    std::vector<std::uint8_t> zero(0x40000, 0);
    r.w32(L.bump_ptr, L.heap);
    r.write(L.gs, zero.data(), 0x40000);
    r.write(L.units, zero.data(), (kUnits + 1) * kUnitSize);
    r.write(L.defs, zero.data(), kTypes * kDefSize);
    r.write(L.sides, zero.data(), (kPlayers + 1) * kSideSize);
    r.write(L.tables, zero.data(), kPlayers * kTblSize);
    r.write(L.linger, zero.data(), 20 * 36);

    Scenario s;
    s.W = 16 + 2 * rnd(41);
    s.H = 16 + 2 * rnd(41);
    const int W = s.W, H = s.H;
    const u8 sea = static_cast<u8>(rnd(3) == 0 ? 0 : rnd(90));
    s.env.sea_level = sea;
    // Heights: a base and random hills and pits.
    s.heights = sim::HeightMap(W, H);
    {
        std::vector<int> h(static_cast<std::size_t>(W * H), 20 + rnd(80));
        const int bumps = 2 + rnd(12);
        for (int b = 0; b < bumps; ++b) {
            const int cx = rnd(W), cz = rnd(H), rad = 2 + rnd(14), amp = rnd(260) - 80;
            for (int z = 0; z < H; ++z)
                for (int x = 0; x < W; ++x) {
                    const int d2 = (x - cx) * (x - cx) + (z - cz) * (z - cz);
                    if (d2 < rad * rad) h[static_cast<std::size_t>(z * W + x)] += amp * (rad * rad - d2) / (rad * rad);
                }
        }
        for (int z = 0; z < H; ++z)
            for (int x = 0; x < W; ++x) s.heights.at(x, z) = static_cast<u8>(std::clamp(h[static_cast<std::size_t>(z * W + x)] + rnd(4), 0, 255));
    }
    const u32 cells = r.alloc(static_cast<SIZE_T>(W) * H * 13);
    {
        std::vector<std::uint8_t> c(static_cast<std::size_t>(W) * H * 13, 0);
        for (int z = 0; z < H; ++z)
            for (int x = 0; x < W; ++x) {
                c[static_cast<std::size_t>(z * W + x) * 13 + 4] = s.heights.at(x, z);
                c[static_cast<std::size_t>(z * W + x) * 13 + 5] = s.heights.at(x, z);
            }
        r.write(cells, c.data(), c.size());
    }
    r.w32(L.gs + gMapW, static_cast<u32>(W));
    r.w32(L.gs + gMapH, static_cast<u32>(H));
    r.w32(L.gs + gWorldW, static_cast<u32>(W * 16));
    r.w32(L.gs + gWorldH, static_cast<u32>(H * 16));
    r.w32(L.gs + gHeights, cells);
    r.write(L.gs + gSea, &sea, 1);
    r.w32(L.gs + gMasks, L.gaf_seq);
    r.w32(L.gs + gLinger, L.linger);
    if (!r.call(kBuildLayers, 0, {})) {
        std::printf("scenario %u: original height band build failed\n", seed);
        ++st.failures;
        return false;
    }
    sim::init_sight_map(s.state, s.explored, s.heights, sea);
    {
        // The explored map: the original's is allocated by the map load (zeroed).
        const u32 ex = r.alloc(s.explored.size() * 2 + 16);
        r.w32(L.gs + gExplored, ex);
        // Height band.
        std::vector<std::array<u8, 2>> band(s.state.band.size());
        r.read(r.r32(L.gs + gBand), band.data(), band.size() * 2);
        if (band != s.state.band) {
            std::printf("scenario %u: height band differs\n", seed);
            ++st.mismatches;
            return true;
        }
        if (static_cast<i32>(r.r32(L.gs + gBand + 4)) != s.state.width || static_cast<i32>(r.r32(L.gs + gBand + 8)) != s.state.height) {
            std::printf("scenario %u: band size differs\n", seed);
            ++st.mismatches;
            return true;
        }
    }
    // Types (1..kTypes-1), synthetic.
    s.types.resize(kTypes);
    s.sight_types.resize(kTypes);
    for (int k = 1; k < kTypes; ++k) {
        TypeData& t = s.types[static_cast<std::size_t>(k)];
        const int fx = 1 + rnd(6), fz = 1 + rnd(6);
        t.info.model_min_x = (fx * -0x100000) / 2;
        t.info.model_min_z = (fz * -0x100000) / 2;
        t.info.model_max_x = (fx << 20) / 2;
        t.info.model_max_z = (fz << 20) / 2;
        t.info.model_min_y = 0;
        t.info.model_max_y = rnd(70) << 16 | rnd(0x10000);
        sim::SightType& st2 = t.sight;
        const int pick = rnd(8);
        st2.sight = static_cast<i16>(pick == 0 ? 0 : pick == 1 ? rnd(32) : pick == 2 ? 32 * rnd(12) : rnd(800));
        st2.radar = static_cast<i16>(rnd(3) == 0 ? 50 + rnd(2500) : 0);
        st2.sonar = static_cast<i16>(rnd(3) == 0 ? 50 + rnd(1500) : 0);
        st2.radar_jam = static_cast<i16>(rnd(5) == 0 ? 50 + rnd(600) : 0);
        st2.sonar_jam = static_cast<i16>(rnd(6) == 0 ? 50 + rnd(500) : 0);
        st2.min_cloak_distance = static_cast<i16>(rnd(300));
        st2.can_cloak = rnd(3) == 0;
        if (std::getenv("LOS_NOCLOAK")) st2.can_cloak = false;
        if (std::getenv("LOS_NORADAR")) st2.radar = st2.sonar = 0;
        if (std::getenv("LOS_NOJAM")) st2.radar_jam = st2.sonar_jam = 0;
        st2.stealth = rnd(5) == 0;
        st2.model_size_x = t.info.model_max_x - t.info.model_min_x;
        st2.model_size_y = t.info.model_max_y - t.info.model_min_y;
        st2.model_size_z = t.info.model_max_z - t.info.model_min_z;
        s.sight_types[static_cast<std::size_t>(k)] = st2;
        std::vector<std::uint8_t> d(kDefSize, 0);
        auto put32 = [&](int at, i32 v) { std::memcpy(&d[static_cast<std::size_t>(at)], &v, 4); };
        auto put16 = [&](int at, i16 v) { std::memcpy(&d[static_cast<std::size_t>(at)], &v, 2); };
        put32(350, t.info.model_min_x);
        put32(354, t.info.model_min_y);
        put32(358, t.info.model_min_z);
        put32(362, t.info.model_max_x);
        put32(366, t.info.model_max_y);
        put32(370, t.info.model_max_z);
        put32(374, st2.model_size_x);
        put32(378, st2.model_size_y);
        put32(382, st2.model_size_z);
        put16(514, st2.sight);
        put16(516, st2.radar);
        put16(518, st2.sonar);
        put16(520, st2.min_cloak_distance);
        put16(522, st2.radar_jam);
        put16(524, st2.sonar_jam);
        put32(577, st2.stealth ? 0x100 : 0);
        put32(581, st2.can_cloak ? 0x2000 : 0);
        r.write(L.defs + static_cast<u32>(k) * kDefSize, d.data(), d.size());
    }
    // Players.
    s.players.assign(11, {});
    s.alliances.assign(11, {});
    for (int p = 0; p < kPlayers; ++p) {
        sim::PlayerSlot& ps = s.players[static_cast<std::size_t>(p)];
        ps.active = rnd(6) != 0;
        ps.controller = static_cast<u8>(rnd(25) == 0 ? 0 : 1 + rnd(3));
        ps.state = static_cast<u8>(rnd(12) == 0 ? 10 : p);
        ps.first_unit = p * 8 + 1;
        ps.last_unit = p * 8 + 8;
        s.alliances[static_cast<std::size_t>(p)][static_cast<std::size_t>(p)] = 1;
        for (int q = 0; q < kPlayers; ++q)
            if (q != p && rnd(4) == 0) s.alliances[static_cast<std::size_t>(p)][static_cast<std::size_t>(q)] = 1;
        s.env.options[static_cast<std::size_t>(p)].shares_radar = rnd(2) == 0;
        s.env.options[static_cast<std::size_t>(p)].watching = rnd(6) == 0;
        const u32 a = player_addr(L, p);
        r.w32(a + pFirst, unit_addr(L, ps.first_unit));
        r.w32(a + pLast, unit_addr(L, ps.last_unit));
        const u16 fi = static_cast<u16>(ps.first_unit), la = static_cast<u16>(ps.last_unit);
        r.write(a + pFirstId, &fi, 2);
        r.write(a + pLastId, &la, 2);
    }
    sim::init_player_sight(s.state, s.players);
    for (int p = 0; p < kPlayers; ++p) {
        const auto& ps = s.state.players[static_cast<std::size_t>(p)];
        if (!ps.allocated) continue;
        const u32 a = player_addr(L, p);
        const u32 buf = r.alloc(ps.counts.size() + 16);
        r.w32(a + pCounts, buf);
        r.w32(a + pCountW, static_cast<u32>(ps.width));
        r.w32(a + pCountH, static_cast<u32>(ps.height));
        r.w32(a + pCountSize, static_cast<u32>(ps.counts.size()));
    }
    s.env.view_player = static_cast<u8>(rnd(kPlayers));
    s.env.player_count = static_cast<u16>(rnd(5) == 0 ? 1 : 2 + rnd(3));
    s.env.game_tick = static_cast<u32>(rnd(1000));
    s.state.flags = static_cast<u16>(rnd(8));
    // Units.
    s.units.clear();
    s.units.resize(kUnits);
    s.ticks.assign(kUnits, {});
    auto place = [&](sim::Unit& u) {
        const int wx = rnd(W * 16 + 64) - 32, wz = rnd(H * 16 + 64) - 32;
        u.pos[0] = wx << 16 | rnd(0x10000);
        u.pos[2] = wz << 16 | rnd(0x10000);
        const int pick = rnd(6);
        int y = s.heights.cell_height(std::clamp(wx / 16, 0, W - 1), std::clamp(wz / 16, 0, H - 1));
        if (pick == 0) y = rnd(300);
        if (pick == 1) y = sea - rnd(40);
        u.pos[1] = y << 16 | rnd(0x10000);
    };
    for (int id = 1; id < kUnits; ++id) {
        sim::Unit& u = s.units[static_cast<std::size_t>(id)];
        sim::UnitTickState& t = s.ticks[static_cast<std::size_t>(id)];
        u.id = static_cast<u16>(id);
        t.owner = (id - 1) / 8;
        t.type_index = static_cast<u16>(rnd(10) == 0 ? 0 : 1 + rnd(kTypes - 1));
        u.type = &s.types[t.type_index].info;
        place(u);
        u.flags = (rnd(8) != 0 ? sim::sight_unit_flags::kAlive : 0) | (rnd(10) == 0 ? sim::sight_unit_flags::kDying : 0) |
                  static_cast<u32>(rnd(8)) << 8 | (rnd(4) == 0 ? sim::sight_unit_flags::kCloakHold : 0);
        u.activate_flags = static_cast<u8>((rnd(3) != 0 ? 1 : 0) | (rnd(5) == 0 ? 4 : 0));
        u.cloak_holdoff_tick = static_cast<u32>(rnd(2000));
        write_unit(r, L, s, id);
    }
    r.w32(L.gs + gUnitBase, unit_addr(L, 0));
    r.w32(L.gs + gUnitEnd, unit_addr(L, kUnits - 1));
    // Unit tables of the players with a controller (some missing).
    for (int p = 0; p < kPlayers; ++p) {
        auto& tb = s.targeting.tables[static_cast<std::size_t>(p)];
        tb.exists = rnd(30) != 0;
        tb.seen.clear();
        for (int id = 1; id < kUnits; ++id)
            if (rnd(3) == 0) tb.seen.push_back(&s.units[static_cast<std::size_t>(id)]);
    }
    write_players(r, L, s);
    write_lists(r, L, s);
    write_buckets(r, L, s);

    sim::SightWorld w{s.state, *g_tables, s.explored, s.units, s.ticks, s.players, s.alliances, s.sight_types, s.buckets, &s.targeting, s.env, 0, 0};
    Compare cmp{r, L, s, {}};
    auto sync_back = [&](int id) { write_unit(r, L, s, id); };

    // Battle start: a rebuild clearing the explored map.
    sim::rebuild_sight(w, true);
    if (w.faults) {
        ++st.faults;
        ++st.by_op["start rebuild (predicted fault)"];
        if (!r.call(kRebuild, 0, {1})) {
            ++st.crashes_confirmed;
            return false;
        }
        ++st.scenarios;
        return true;
    }
    if (!r.call(kRebuild, 0, {1})) {
        std::printf("scenario %u: original first rebuild failed\n", seed);
        ++st.failures;
        return false;
    }
    ++st.ops;
    ++st.by_op["rebuild"];
    if (!cmp.all()) {
        std::printf("scenario %u start: %s\n", seed, cmp.what.c_str());
        ++st.mismatches;
        return true;
    }
    for (int step = 0; step < steps; ++step) {
        const int nops = 1 + rnd(8);
        for (int k = 0; k < nops; ++k) {
            const int op = rnd(100);
            const int id = 1 + rnd(kUnits - 1);
            sim::Unit& u = s.units[static_cast<std::size_t>(id)];
            const std::uint32_t faults_before = w.faults;
            std::string name;
            bool ok = true;
            if (op < 30) {  // a move and the sight update
                name = "move+update";
                if (rnd(3) == 0) place(u);
                else {
                    u.pos[0] += (rnd(64) - 32) << 16;
                    u.pos[2] += (rnd(64) - 32) << 16;
                    u.pos[1] += (rnd(16) - 8) << 16;
                }
                sync_back(id);
                sim::update_unit_sight(w, u);
                ok = r.call(kUpdate, 0, {unit_addr(L, id)});
            } else if (op < 36) {
                name = "new unit";
                sim::stamp_new_unit(w, u);
                ok = r.call(kNewUnit, 0, {unit_addr(L, id)});
            } else if (op < 42) {  // the kill handler's sight
                name = "death";
                const sim::UnitTickState& t = s.ticks[static_cast<std::size_t>(id)];
                if (s.players[static_cast<std::size_t>(t.owner)].state == s.env.view_player) {
                    const i32 y = u.pos[1];
                    r.write(L.pos, u.pos.data(), 12);
                    const u32 eye = (static_cast<u32>(u.type->model_max_y) >> 16) & 0xFF;
                    sim::linger_dead_unit(w, u);
                    ok = r.call(kLinger, 0, {L.pos, static_cast<u32>(static_cast<u16>(s.sight_types[t.type_index].sight)), eye, 0x3C});
                    (void)y;
                    ++st.by_op["death: lingering"];
                }
                if (ok && (s.state.flags & sim::sight_flags::kFog)) {
                    sim::remove_unit_sight(w, u);
                    ok = r.call(kRemove, 0, {unit_addr(L, id)});
                }
            } else if (op < 50) {
                name = "frame expiry";
                s.env.game_tick += static_cast<u32>(rnd(40));
                r.w32(L.gs + gTick, s.env.game_tick);
                sim::expire_lingering_sight(w);
                ok = r.call(kExpire, 0, {});
            } else if (op < 62) {
                name = "visible";
                const int viewer = rnd(kPlayers);
                u32 res = 0;
                const bool mine = sim::unit_visible(w, viewer, u);
                ok = r.call(kVisible, 0, {player_addr(L, viewer), unit_addr(L, id)}, &res);
                if (ok && ((res & 0xFF) != 0) != mine) {
                    std::printf("scenario %u step %d: visible(%d, unit %d) %u vs %d\n", seed, step, viewer, id, res & 0xFF, mine);
                    ++st.mismatches;
                    return true;
                }
                if (mine) ++st.visible_true;
            } else if (op < 68) {
                name = "explored";
                const int owner = rnd(kPlayers);
                std::array<i32, 3> pos{(rnd(W * 16 + 64) - 32) << 16, rnd(200) << 16, (rnd(H * 16 + 64) - 32) << 16};
                r.write(L.pos, pos.data(), 12);
                u32 res = 0;
                const bool mine = sim::position_explored(w, owner, pos);
                ok = r.call(kExplored, 0, {player_addr(L, owner), L.pos}, &res);
                if (ok && ((res & 0xFF) != 0) != mine) {
                    std::printf("scenario %u step %d: explored(%d) %u vs %d\n", seed, step, owner, res & 0xFF, mine);
                    ++st.mismatches;
                    return true;
                }
                if (mine) ++st.explored_true;
            } else if (op < 78) {
                name = "sensors";
                for (int j = 1; j < kUnits; ++j)
                    if (rnd(4) == 0) {
                        sim::Unit& v = s.units[static_cast<std::size_t>(j)];
                        v.activate_flags = static_cast<u8>(v.activate_flags ^ (rnd(2) ? 1 : 4));
                        if (rnd(6) == 0) v.flags ^= sim::sight_unit_flags::kAlive;
                        sync_back(j);
                    }
                for (int p = 0; p < kPlayers; ++p) {
                    auto& tb = s.targeting.tables[static_cast<std::size_t>(p)];
                    if (rnd(3) == 0) {
                        tb.seen.clear();
                        for (int j = 1; j < kUnits; ++j)
                            if (rnd(3) == 0) tb.seen.push_back(&s.units[static_cast<std::size_t>(j)]);
                    }
                }
                write_lists(r, L, s);
                write_buckets(r, L, s);
                sim::update_sensors(w);
                ok = r.call(kSensors, 0, {});
                for (int j = 1; j < kUnits; ++j) {
                    const u32 f = s.units[static_cast<std::size_t>(j)].flags;
                    st.radar_marks += (f & 0x100) ? 1 : 0;
                    st.sonar_marks += (f & 0x200) ? 1 : 0;
                    st.jammed += (f & 0x400) ? 1 : 0;
                    st.holds += (f & 0x1000) ? 1 : 0;
                }
            } else if (op < 82) {
                name = "rebuild";
                const bool clear = rnd(2) == 0;
                sim::rebuild_sight(w, clear);
                ok = r.call(kRebuild, 0, {clear ? 1u : 0u});
            } else if (op < 86) {  // an option toggle and its rebuild (the console commands)
                name = "option toggle";
                const int bit = rnd(3);
                s.state.flags = static_cast<u16>(s.state.flags ^ (1u << bit));
                write_players(r, L, s);
                sim::rebuild_sight(w, bit == 0);
                ok = r.call(kRebuild, 0, {bit == 0 ? 1u : 0u});
            } else if (op < 90) {
                name = "view change";
                s.env.view_player = static_cast<u8>(rnd(kPlayers));
                write_players(r, L, s);
            } else if (op < 93) {
                name = "share";
                const u8 from = static_cast<u8>(rnd(kPlayers)), to = static_cast<u8>(rnd(kPlayers));
                sim::share_explored(w, from, to);
                ok = r.call(kShare, 0, {from, to});
            } else {
                name = "tick";
                s.env.game_tick += static_cast<u32>(1 + rnd(30));
                r.w32(L.gs + gTick, s.env.game_tick);
            }
            const bool predicted = w.faults != faults_before;
            if (predicted) {
                // The engine says the original reads or writes outside its data here.
                ++st.faults;
                if (!ok) {
                    ++st.crashes_confirmed;
                    ++st.by_op[name + " (predicted crash, confirmed)"];
                    return false;
                }
                ++st.by_op[name + " (predicted fault, the original survived)"];
                ++st.scenarios;
                return true;
            }
            if (!ok) {
                std::printf("scenario %u step %d: original %s (unit %d) failed\n", seed, step, name.c_str(), id);
                ++st.failures;
                return false;
            }
            ++st.ops;
            ++st.by_op[name];
            st.lingering_max = std::max<std::size_t>(st.lingering_max, static_cast<std::size_t>(s.state.lingering_count));
            if (!cmp.all()) {
                std::printf("scenario %u step %d after %s (unit %d): %s\n", seed, step, name.c_str(), id, cmp.what.c_str());
                ++st.mismatches;
                return true;
            }
        }
        ++st.steps;
    }
    st.faults += w.faults;
    st.heap_reads += w.heap_reads;
    ++st.scenarios;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: los_harness <TotalA.exe> <game_dir> [scenarios] [steps] [first seed]\n");
        return 2;
    }
    const int scenarios = argc > 3 ? std::atoi(argv[3]) : 10;
    const int steps = argc > 4 ? std::atoi(argv[4]) : 40;
    const int first = argc > 5 ? std::atoi(argv[5]) : 0;
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[2]);
    sim::SightTables tables;
    std::string error;
    if (!ta::load_sight_tables(vfs, tables, error) || !vfs.read("anims\\vismasks.gaf", g_gaf)) {
        std::fprintf(stderr, "sight tables: %s\n", error.c_str());
        return 1;
    }
    g_tables = &tables;
    {
        std::size_t slices = 0, points = 0;
        for (const auto& ring : tables.rings)
            for (const auto& sl : ring) {
                ++slices;
                points += sl.size();
            }
        std::printf("ring tables %zu (%zu slices, %zu points), sight masks %zu", tables.rings.size(), slices, points, tables.masks.size());
        if (!tables.masks.empty())
            std::printf(" (first %dx%d, last %dx%d)", tables.masks.front().width, tables.masks.front().height, tables.masks.back().width,
                        tables.masks.back().height);
        std::printf("\n");
    }
    harness::Remote r;
    Layout L;
    if (!r.start(argv[1]) || !setup(r, L)) {
        std::fprintf(stderr, "cannot start the original\n");
        return 1;
    }
    Stats st;
    for (int s = 0; s < scenarios; ++s) {
        if (!run_scenario(r, L, steps, static_cast<unsigned>(first + s), st)) {
            r.stop();
            if (!r.start(argv[1]) || !setup(r, L)) return 1;
        }
    }
    std::printf("scenarios %zu, steps %zu, operations %zu\n", st.scenarios, st.steps, st.ops);
    std::printf("visible answers %zu, explored answers %zu; after sensor passes: radar marks %zu, sonar marks %zu, jammed %zu, cloak holds %zu; "
                "most lingering %zu\n",
                st.visible_true, st.explored_true, st.radar_marks, st.sonar_marks, st.jammed, st.holds, st.lingering_max);
    std::printf("operations:\n");
    for (const auto& [k, v] : st.by_op) std::printf("  %s: %zu\n", k.c_str(), v);
    std::printf("rings read in front of the table (sight below 32, true sight): %zu\n", st.heap_reads);
    std::printf("mismatching scenarios %zu, engine faults %zu (original crashes predicted and confirmed %zu), unexpected original failures %zu\n",
                st.mismatches, st.faults, st.crashes_confirmed, st.failures);
    return st.mismatches || st.failures ? 1 : 0;
}
