// Effect particle lists (sim/effect_lists.h) against the original: the spawners
// (0x471FD0 .. 0x472C50) and the effect sweep (0x471EB0) run in a suspended TotalA.exe on a
// small game state, the engine's model runs beside them, and after every operation the C
// runtime rand() state, the pool's use, every list's emitters (deadline, next emission,
// particle count) and every particle's fields are compared.
//
// Usage: fx_harness <TotalA.exe> [scenarios] [operations] [first seed]

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/effect_lists.h"
#include "sim/heightmap.h"

namespace {

using harness::u32;
using i32 = std::int32_t;

constexpr u32 kGameStatePtr = 0x00511DE8;
constexpr u32 kPool = 0x0051E610, kPoolBlocked = 0x0051E608, kPoolConstruct = 0x00470A90;
constexpr u32 kAllocSlots = 0x00471D90, kSweep = 0x00471EB0, kRand = 0x004E4870;
constexpr u32 kSpawnDots = 0x00471FD0, kSpawnFlame = 0x00472330, kSpawnJitter = 0x00472AB0, kSpawnWakeDown = 0x00472530,
              kSpawnWakeUp = 0x00472430, kSpawnSmokeEmitter = 0x00472630, kSpawnSmoke1 = 0x00472810, kSpawnSmoke2 = 0x004728F0,
              kSpawnMuzzle = 0x004729D0, kSpawnSteam = 0x00472C50;
constexpr u32 kOffTick = 0x38A47, kOffSlots = 0x38D77, kOffSea = 0x1427F, kOffW = 0x14233, kOffH = 0x14237, kOffCells = 0x14287,
              kOffSmoke1 = 0x147CF, kOffSmoke2 = 0x147D3, kOffFlame = 0x147F3;
constexpr int kMap = 32;  // map cells each way

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
};

struct Layout {
    u32 heap = 0, bump = 0, stubs = 0, gs = 0, rand_state = 0, cells = 0, seqs = 0, args = 0;
};

bool setup(harness::Remote& r, Layout& L) {
    L.heap = r.alloc(64u << 20);
    L.bump = r.alloc(0x1000);
    L.stubs = r.alloc(0x1000, PAGE_EXECUTE_READWRITE);
    L.gs = r.alloc(0x40000);
    L.rand_state = r.alloc(0x1000);
    L.cells = r.alloc(kMap * kMap * 13);
    L.seqs = r.alloc(0x1000);
    L.args = r.alloc(0x1000);
    if (!L.heap || !L.stubs || !L.gs || !L.cells) return false;
    Emitter e;
    // Bump allocators (tag, size) / (size) / realloc (ptr, tag, size); frees do nothing.
    // (and the pool's malloc (size) / realloc (ptr, size), always fresh: nothing to copy here).
    const int size_at[5] = {8, 4, 12, 4, 8};
    const u32 allocs[5] = {0x004D83B0, 0x004B4F10, 0x004D84A0, 0x004D8450, 0x004D8580};
    for (int k = 0; k < 5; ++k) {
        const u32 at = L.stubs + static_cast<u32>(e.b.size());
        e.u8(0xA1); e.u32le(L.bump);
        e.u8(0x8B); e.u8(0x4C); e.u8(0x24); e.u8(static_cast<std::uint8_t>(size_at[k]));
        e.u8(0x83); e.u8(0xC1); e.u8(0x03); e.u8(0x83); e.u8(0xE1); e.u8(0xFC);
        e.u8(0x01); e.u8(0x0D); e.u32le(L.bump);
        e.u8(0xC3);
        while (e.b.size() % 16) e.u8(0xCC);
        if (!r.patch_jump(allocs[k], at)) return false;
    }
    {
        const u32 at = L.stubs + static_cast<u32>(e.b.size());
        e.u8(0xC3);
        while (e.b.size() % 16) e.u8(0xCC);
        for (u32 f : {0x004B4F20u, 0x004D85B0u, 0x004D85A0u, 0x00433DA0u, 0x004D8670u})
            if (!r.patch_jump(f, at)) return false;
    }
    {
        // rand() on our own state (the runtime is not initialised).
        const u32 at = L.stubs + static_cast<u32>(e.b.size());
        e.u8(0xA1); e.u32le(L.rand_state);
        e.u8(0x69); e.u8(0xC0); e.u32le(214013u);
        e.u8(0x05); e.u32le(2531011u);
        e.u8(0xA3); e.u32le(L.rand_state);
        e.u8(0xC1); e.u8(0xE8); e.u8(0x10);
        e.u8(0x25); e.u32le(0x7FFF);
        e.u8(0xC3);
        while (e.b.size() % 16) e.u8(0xCC);
        if (!r.patch_jump(kRand, at)) return false;
    }
    if (!r.write(L.stubs, e.b.data(), e.b.size())) return false;
    r.w32(L.bump, L.heap);
    r.w32(kGameStatePtr, L.gs);
    r.w32(L.gs + kOffW, kMap);
    r.w32(L.gs + kOffH, kMap);
    r.w32(L.gs + kOffCells, L.cells);
    r.w32(L.gs + kOffSmoke1, L.seqs);
    r.w32(L.gs + kOffSmoke2, L.seqs + 0x40);
    r.w32(L.gs + kOffFlame, L.seqs + 0x80);
    return true;
}

struct Stats {
    std::size_t scenarios = 0, ops = 0, spawns = 0, refused = 0, sweeps = 0, particles_max = 0, emitters_max = 0, mismatches = 0, faults = 0;
    std::size_t by_kind[10] = {};
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fx_harness <TotalA.exe> [scenarios] [operations] [first seed]\n");
        return 2;
    }
    const int scenarios = argc > 2 ? std::atoi(argv[2]) : 10;
    const int ops = argc > 3 ? std::atoi(argv[3]) : 200;
    const u32 first = argc > 4 ? static_cast<u32>(std::strtoul(argv[4], nullptr, 0)) : 1u;
    Stats st;
    for (int sc = 0; sc < scenarios; ++sc) {
        const u32 seed = first + static_cast<u32>(sc);
        std::mt19937 g(seed);
        harness::Remote r;
        Layout L;
        if (!r.start(argv[1]) || !setup(r, L)) {
            std::printf("cannot start %s\n", argv[1]);
            return 1;
        }
        // Scenario: heights, sea level, sequence frame counts (small ones too), pool and lists.
        sim::HeightMap hm(kMap, kMap);
        std::vector<std::uint8_t> cells(kMap * kMap * 13, 0);
        for (int z = 0; z < kMap; ++z)
            for (int x = 0; x < kMap; ++x) {
                const std::uint8_t h = static_cast<std::uint8_t>(g() % 4 ? 20 + g() % 60 : g() % 256);
                hm.at(x, z) = h;
                cells[static_cast<std::size_t>((z * kMap + x) * 13 + 4)] = h;
            }
        r.write(L.cells, cells.data(), cells.size());
        const std::uint8_t sea = static_cast<std::uint8_t>(g() % 80);
        r.write(L.gs + kOffSea, &sea, 1);
        const i32 s1 = 3 + static_cast<i32>(g() % 20), s2 = 3 + static_cast<i32>(g() % 20), fl = 2 + static_cast<i32>(g() % 12);
        const std::uint16_t f16[3] = {static_cast<std::uint16_t>(s1), static_cast<std::uint16_t>(s2), static_cast<std::uint16_t>(fl)};
        for (int k = 0; k < 3; ++k) r.write(L.seqs + static_cast<u32>(k) * 0x40, &f16[k], 2);
        const u32 pool_size = g() % 3 == 0 ? 20 + g() % 40 : 1000;  // small pools run out
        if (!r.call(kPoolConstruct, kPool, {pool_size, 0x4C}) || !r.call(kAllocSlots, 0, {})) {
            std::printf("scenario %u: set-up call failed\n", seed);
            ++st.mismatches;
            continue;
        }
        r.w32(kPoolBlocked, 0);
        sim::EffectLists fx;
        fx.smoke1_frames = s1;
        fx.smoke2_frames = s2;
        fx.flame_frames = fl;
        sim::Lcg15 rand;
        rand.set_state(g());
        r.w32(L.rand_state, rand.state());
        u32 tick = g() % 1000;
        r.w32(L.gs + kOffTick, tick);
        // The engine's pool is 1000; a smaller original pool is modelled as nano streams holding the rest.
        const i32 held = 1000 - static_cast<i32>(pool_size);
        auto ctx = [&]() { return sim::FxContext{fx, rand, tick, held, sea, &hm, 0, 0, 0}; };
        auto rpos = [&]() {
            std::array<i32, 3> p;
            p[0] = static_cast<i32>(g() % (kMap * 16)) << 16 | static_cast<i32>(g() & 0xFFFF);
            p[1] = static_cast<i32>(g() % 256) << 16;
            p[2] = static_cast<i32>(g() % (kMap * 16)) << 16 | static_cast<i32>(g() & 0xFFFF);
            return p;
        };
        bool bad = false;
        auto check = [&](const char* what) {
            if (!fx.fault.empty()) {
                ++st.faults;
                bad = true;
                std::printf("scenario %u: engine fault (%s) after %s: stopped\n", seed, fx.fault.c_str(), what);
                return;
            }
            bool ok = r.r32(L.rand_state) == rand.state();
            const u32 slots = r.r32(L.gs + kOffSlots);
            std::size_t particles = 0, emitters = 0;
            for (int l = 0; l < 10 && ok; ++l) {
                const u32 begin = r.r32(slots + static_cast<u32>(l) * 16 + 4), end = r.r32(slots + static_cast<u32>(l) * 16 + 8);
                const std::size_t n = begin ? (end - begin) / 4 : 0;
                const auto& mine = fx.lists[static_cast<std::size_t>(l)];
                if (n != mine.size()) {
                    ok = false;
                    break;
                }
                emitters += n;
                for (std::size_t k = 0; k < n && ok; ++k) {
                    const u32 obj = r.r32(begin + static_cast<u32>(k) * 4);
                    const sim::FxEmitter& m = mine[k];
                    ok = r.r32(obj + 4) == m.deadline && r.r32(obj + 8) == m.next_emit;
                    const u32 pb = r.r32(obj + 0x10), pe = r.r32(obj + 0x14);
                    u32 stride = 0x20;
                    switch (m.kind) {
                    case sim::FxKind::Dots: stride = 0x34; break;
                    case sim::FxKind::Flame: stride = 0x3C; break;
                    case sim::FxKind::Wake: stride = 0x44; break;
                    default: break;
                    }
                    const std::size_t pn = pb ? (pe - pb) / stride : 0;
                    ok = ok && pn == m.particles.size();
                    particles += pn;
                    for (std::size_t q = 0; q < pn && ok; ++q) {
                        u32 w[17];
                        r.read(pb + static_cast<u32>(q) * stride, w, stride);
                        const sim::FxParticle& p = m.particles[q];
                        switch (m.kind) {
                        case sim::FxKind::Smoke:
                        case sim::FxKind::Steam:
                            ok = static_cast<i32>(w[4]) == p.frame_max && static_cast<i32>(w[5]) == p.frame && static_cast<i32>(w[6]) == p.hold_period &&
                                 static_cast<i32>(w[7]) == p.hold_left;
                            break;
                        case sim::FxKind::Wake:
                            ok = static_cast<i32>(w[1]) == p.pos[0] && static_cast<i32>(w[2]) == p.pos[1] && static_cast<i32>(w[3]) == p.pos[2] &&
                                 static_cast<i32>(w[7]) == p.vel[0] && static_cast<i32>(w[8]) == p.vel[1] && static_cast<i32>(w[9]) == p.vel[2] &&
                                 static_cast<i32>(w[16]) == p.expire;
                            break;
                        case sim::FxKind::Flame: ok = static_cast<i32>(w[14]) == p.expire; break;
                        case sim::FxKind::Dots: ok = static_cast<i32>(w[12]) == p.expire; break;
                        }
                    }
                }
            }
            ok = ok && static_cast<int>(r.r32(kPool + 0x20)) == fx.live();
            if (particles > st.particles_max) st.particles_max = particles;
            if (emitters > st.emitters_max) st.emitters_max = emitters;
            if (!ok) {
                std::printf("scenario %u: differs after %s (rand orig %08x ours %08x, pool orig %u ours %d)\n", seed, what, r.r32(L.rand_state),
                            rand.state(), r.r32(kPool + 0x20), fx.live());
                ++st.mismatches;
                bad = true;
            }
        };
        for (int op = 0; op < ops && !bad; ++op) {
            ++st.ops;
            const u32 kind = g() % 16;
            const int list = static_cast<int>(g() % 10);
            const std::array<i32, 3> a = rpos(), b = rpos();
            r.write(L.args, a.data(), 12);
            r.write(L.args + 0x10, b.data(), 12);
            const u32 pa = L.args, pb = L.args + 0x10;
            sim::FxContext c = ctx();
            bool spawned = true, called = true;
            const char* what = "?";
            switch (kind) {
            case 0: {
                const i32 dur = 1 + static_cast<i32>(g() % 60);
                spawned = sim::fx_dots(c, a, b, dur, list);
                if (!fx.fault.empty()) break;
                called = r.call(kSpawnDots, 0, {pa, pb, static_cast<u32>(dur), static_cast<u32>(list)});
                what = "dots";
                break;
            }
            case 1: {
                const i32 p = 1 + static_cast<i32>(g() % 3), dur = g() % 25 == 0 ? 0 : 1 + static_cast<i32>(g() % 20);
                spawned = sim::fx_flame(c, a, b, p, dur, list);
                if (!fx.fault.empty()) break;
                called = r.call(kSpawnFlame, 0, {pa, pb, static_cast<u32>(p), static_cast<u32>(dur), static_cast<u32>(list)});
                what = "flame";
                break;
            }
            case 2:
                spawned = sim::fx_flame_jittered(c, a, list);
                called = r.call(kSpawnJitter, 0, {pa, static_cast<u32>(list)});
                what = "jittered flame";
                break;
            case 3:
            case 4: {
                const i32 size = g() % 2 ? 8 : 0x10;
                const bool rising = kind == 4;
                spawned = sim::fx_wake(c, a, b, size, rising, list);
                if (!fx.fault.empty()) break;
                called = r.call(rising ? kSpawnWakeUp : kSpawnWakeDown, 0, {pa, pb, static_cast<u32>(size), static_cast<u32>(list)});
                what = "wake";
                break;
            }
            case 5: {
                const i32 period = 1 + static_cast<i32>(g() % 20), dur = static_cast<i32>(g() % 900);
                spawned = sim::fx_smoke_emitter(c, a, period, dur, list);
                called = r.call(kSpawnSmokeEmitter, 0, {pa, static_cast<u32>(period), static_cast<u32>(dur), static_cast<u32>(list)});
                what = "smoke emitter";
                break;
            }
            case 6:
            case 7:
                spawned = sim::fx_smoke(c, a, list, kind == 7);
                called = r.call(kind == 7 ? kSpawnSmoke2 : kSpawnSmoke1, 0, {pa, static_cast<u32>(list)});
                what = "smoke";
                break;
            case 8:
                spawned = sim::fx_muzzle_smoke(c, a, list);
                called = r.call(kSpawnMuzzle, 0, {pa, static_cast<u32>(list)});
                what = "muzzle smoke";
                break;
            case 9:
                spawned = sim::fx_steam(c, a, list);
                called = r.call(kSpawnSteam, 0, {pa, static_cast<u32>(list)});
                what = "steam";
                break;
            default: {
                // Sweep steps.
                const int n = 1 + static_cast<int>(g() % 8);
                for (int k = 0; k < n && !bad; ++k) {
                    ++tick;
                    r.w32(L.gs + kOffTick, tick);
                    sim::FxContext s = ctx();
                    sim::step_effect_lists(s, 0, 10);
                    if (!fx.fault.empty()) break;
                    if (!r.call(kSweep, 0, {})) {
                        std::printf("scenario %u: the original's sweep failed\n", seed);
                        bad = true;
                        ++st.mismatches;
                        break;
                    }
                    ++st.sweeps;
                    check("sweep");
                }
                continue;
            }
            }
            if (!fx.fault.empty()) {
                check(what);
                break;
            }
            if (!called) {
                std::printf("scenario %u: the original's %s spawn failed\n", seed, what);
                ++st.mismatches;
                break;
            }
            ++st.by_kind[kind < 10 ? kind : 0];
            if (spawned) ++st.spawns;
            else ++st.refused;
            check(what);
        }
        ++st.scenarios;
    }
    std::printf("scenarios %zu, operations %zu, spawns %zu (refused %zu), sweep steps %zu, most emitters %zu, most particles %zu, engine faults %zu\n",
                st.scenarios, st.ops, st.spawns, st.refused, st.sweeps, st.emitters_max, st.particles_max, st.faults);
    std::printf("by kind: dots %zu flame %zu jitter %zu wake %zu smoke-emitter %zu smoke %zu muzzle %zu steam %zu\n", st.by_kind[0], st.by_kind[1],
                st.by_kind[2], st.by_kind[3] + st.by_kind[4], st.by_kind[5], st.by_kind[6] + st.by_kind[7], st.by_kind[8], st.by_kind[9]);
    std::printf("%zu mismatches\n", st.mismatches);
    return st.mismatches ? 1 : 0;
}
