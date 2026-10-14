// Differential tester for map features (sim/features.h) and world effects and
// debris (sim/effects.h). Inside a suspended TotalA.exe it builds a small map
// (cells with heights, sea and features), random feature types with synthetic
// frame sequences, the feature animation slot table, the world effect table,
// shard slots, debris slots and their cache, a few weapons and units with real
// models and object states. Each scenario applies random operations with the
// original routines (place, remove, next stage, destroy, ignite, weapon damage,
// damage packets, the feature stage, wrecks of dead units, script explode,
// world effects and impacts, the effects stage) and the engine's, and compares
// after every operation: every map cell, all 2048 animation slots byte for
// byte, list heads, the reproduction sweep, type animation cursors, the world
// effect table, shards, debris pieces and the cache's blocks, piece visibility,
// the battle RNG, the rand() stream and the log of calls to the stubbed
// systems (passability, object states, burn weapons, smoke, steam, sounds,
// fires, network packets).
//
// Usage: feature_harness <TotalA.exe> <game_dir> [scenarios] [operations] [first seed]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/anim.h"
#include "sim/effects.h"
#include "sim/features.h"
#include "sim/footprint.h"
#include "sim/pose.h"
#include "sim/rng.h"
#include "sim/unit.h"
#include "ta/hpi.h"
#include "ta/model3do.h"

namespace {

using harness::u32;
using i16 = std::int16_t;
using i32 = std::int32_t;
using u16 = std::uint16_t;
using u8 = std::uint8_t;

constexpr u32 kGameStatePtr = 0x00511DE8, kBattleRngState = 0x0051FC88;
constexpr u32 kDebrisSlots = 0x00511DF0, kDebrisCache = 0x00511F80;
constexpr u32 kRelocate = 0x004CB4C0, kNegateXZ = 0x004CB590, kBuildState = 0x0045A950, kSync = 0x0045AB10;
// Routines under test (all stdcall; explode is thiscall on a script context).
constexpr u32 kPlace = 0x00423C50, kRemove = 0x004246B0, kReplace = 0x00423710, kKill = 0x00423550, kIgnite = 0x004233A0,
              kDamage = 0x004244B0, kFeatureStep = 0x00424050, kWreck = 0x00486360, kExplode = 0x00481140,
              kEffectsStep = 0x00420F30, kSpawnEffect = 0x00420A30, kGetCell = 0x00481550;
constexpr u32 kCellSize = 13, kSlotBytes = 0x30, kDefSize = 0x100, kWeaponSize = 0x115, kUnitSize = 0x118, kEffectSize = 0x54,
              kShardSize = 0x34, kPlayerSize = 0x14B;
// Game state offsets.
enum : u32 {
    gLocalView = 0x2A43, gPlayers = 0x1B63, gAnimTable = 0x1420B, gActive = 0x14213, gIdle = 0x14217, gFree = 0x1421B,
    gMapW = 0x14233, gMapH = 0x14237, gFeatureCount = 0x14253, gSweep = 0x14257, gGravity = 0x14263, gFeatureDefs = 0x1426F,
    gSea = 0x1427F, gCells = 0x14287, gWaterSplash = 0x147EB, gLavaSplash = 0x147EF, gBitmaps = 0x147F7, gEffectCount = 0x1491B,
    gEffects = 0x1491F, gGlows = 0x1AB8F, gShards = 0x1AB9F, gWind = 0x37ECC, gOptions = 0x37F2F, gTick = 0x38A47,
    gMapInfo = 0x391E9,
};

// ---- stubs ---------------------------------------------------------------------------------
enum StubId : u32 {
    sPassability = 1, sCreateObject, sFreeObject, sSmoke, sSteam, sSound, sFire, sAreaDamage, sNetBroadcast, sNetToHost,
};
struct Stub {
    u32 address;
    int args;
    u32 id;           // 0 = no log
    int deref = -1;   // argument whose 3 dwords are logged
    u32 ret = 0;      // constant return value
};
const Stub kStubs[] = {
    {0x00440A40, 2, sPassability},
    {0x0045A8D0, 1, sCreateObject, -1, 0x7000},
    {0x0045AAA0, 1, sFreeObject},
    {0x00472810, 2, sSmoke, 0},
    {0x00472C50, 2, sSteam, 0},
    {0x0047F610, 3, sSound, 1},
    {0x00472630, 4, sFire, 0},
    {0x0049A0C0, 2, sAreaDamage, 1},
    {0x00451DF0, 3, sNetBroadcast, 1},
    {0x00451BC0, 4, sNetToHost, 2},
    {0x0044FDB0, 0, 0, -1, 0x11},  // first human player's net id
    {0x00450030, 0, 0, -1, 0x22},  // the host's net id
};

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8_(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
};

// Logs {id, args..., deref dwords...} at *log_ptr and returns the constant.
void emit_stub(Emitter& e, const Stub& s, u32 log_ptr) {
    if (s.id) {
        e.u8_(0xA1); e.u32le(log_ptr);
        e.u8_(0xC7); e.u8_(0x00); e.u32le(s.id);
        int at = 4;
        for (int i = 0; i < s.args; ++i) {
            e.u8_(0x8B); e.u8_(0x54); e.u8_(0x24); e.u8_(static_cast<std::uint8_t>(4 + 4 * i));
            e.u8_(0x89); e.u8_(0x50); e.u8_(static_cast<std::uint8_t>(at));
            at += 4;
        }
        if (s.deref >= 0) {
            e.u8_(0x8B); e.u8_(0x54); e.u8_(0x24); e.u8_(static_cast<std::uint8_t>(4 + 4 * s.deref));
            for (int k = 0; k < 3; ++k) {
                e.u8_(0x8B); e.u8_(0x4A); e.u8_(static_cast<std::uint8_t>(4 * k));
                e.u8_(0x89); e.u8_(0x48); e.u8_(static_cast<std::uint8_t>(at));
                at += 4;
            }
        }
        e.u8_(0x83); e.u8_(0x05); e.u32le(log_ptr); e.u8_(static_cast<std::uint8_t>(at));
    }
    e.u8_(0xB8); e.u32le(s.ret);
    if (s.args == 0) {
        e.u8_(0xC3);
    } else {
        e.u8_(0xC2); e.u8_(static_cast<std::uint8_t>(4 * s.args)); e.u8_(0x00);
    }
}
std::size_t entry_words(const Stub& s) { return 1 + static_cast<std::size_t>(s.args) + (s.deref >= 0 ? 3 : 0); }
const Stub* stub_by_id(u32 id) {
    for (const Stub& s : kStubs)
        if (s.id == id) return &s;
    return nullptr;
}

struct Layout {
    u32 heap = 0, bump_ptr = 0, stubs = 0, log_ptr = 0, log_base = 0, rand_state = 0, gs = 0, cells = 0, defs = 0, slots = 0,
        seqs = 0, weapons = 0, units = 0, defs_unit = 0, models = 0, ctx = 0, mapinfo = 0, lobby = 0, cache_buf = 0, shard_buf = 0,
        motion = 0;
};

bool setup(harness::Remote& r, Layout& L) {
    L.heap = r.alloc(64u << 20);
    L.bump_ptr = r.alloc(0x1000);
    L.stubs = r.alloc(0x4000, PAGE_EXECUTE_READWRITE);
    L.log_ptr = r.alloc(0x1000);
    L.log_base = r.alloc(8u << 20);
    L.rand_state = r.alloc(0x1000);
    L.gs = r.alloc(0x40000);
    L.cells = r.alloc(0x40000);
    L.defs = r.alloc(0x10000);
    L.slots = r.alloc(0x18000);
    L.seqs = r.alloc(0x40000);
    L.weapons = r.alloc(0x1000);
    L.units = r.alloc(0x2000);
    L.defs_unit = r.alloc(0x2000);
    L.models = r.alloc(4u << 20);
    L.ctx = r.alloc(0x2000);
    L.mapinfo = r.alloc(0x1000);
    L.lobby = r.alloc(0x1000);
    L.cache_buf = r.alloc(sim::kDebrisCacheSize + 0x100);
    L.shard_buf = r.alloc(0x20000);
    L.motion = r.alloc(0x1000);
    if (!L.heap || !L.stubs || !L.gs || !L.cells || !L.slots || !L.seqs || !L.models || !L.cache_buf || !L.shard_buf) return false;
    Emitter e;
    // Bump allocators: (tag, size), (size), realloc (ptr, tag, size).
    const int size_at[3] = {8, 4, 12};
    const u32 targets[3] = {0x004D83B0, 0x004B4F10, 0x004D84A0};
    for (int k = 0; k < 3; ++k) {
        const u32 at = L.stubs + static_cast<u32>(e.b.size());
        e.u8_(0xA1); e.u32le(L.bump_ptr);
        e.u8_(0x8B); e.u8_(0x4C); e.u8_(0x24); e.u8_(static_cast<std::uint8_t>(size_at[k]));
        e.u8_(0x83); e.u8_(0xC1); e.u8_(0x03); e.u8_(0x83); e.u8_(0xE1); e.u8_(0xFC);
        e.u8_(0x01); e.u8_(0x0D); e.u32le(L.bump_ptr);
        e.u8_(0xC3);
        while (e.b.size() % 16) e.u8_(0xCC);
        if (!r.patch_jump(targets[k], at)) return false;
    }
    // rand(): the C runtime's generator on our own state (the runtime is not initialised).
    {
        const u32 at = L.stubs + static_cast<u32>(e.b.size());
        e.u8_(0xA1); e.u32le(L.rand_state);
        e.u8_(0x69); e.u8_(0xC0); e.u32le(214013u);
        e.u8_(0x05); e.u32le(2531011u);
        e.u8_(0xA3); e.u32le(L.rand_state);
        e.u8_(0xC1); e.u8_(0xE8); e.u8_(0x10);
        e.u8_(0x25); e.u32le(0x7FFF);
        e.u8_(0xC3);
        while (e.b.size() % 16) e.u8_(0xCC);
        if (!r.patch_jump(0x004E4870, at)) return false;
    }
    for (const Stub& s : kStubs) {
        const u32 at = L.stubs + static_cast<u32>(e.b.size());
        emit_stub(e, s, L.log_ptr);
        while (e.b.size() % 16) e.u8_(0xCC);
        if (!r.patch_jump(s.address, at)) return false;
    }
    if (!r.write(L.stubs, e.b.data(), e.b.size())) return false;
    r.w32(L.bump_ptr, L.heap);
    r.w32(kGameStatePtr, L.gs);
    return true;
}

// ---- the engine's view of the stubbed systems ----------------------------------------------
struct Log {
    std::vector<u32> w;
    void rec(u32 id, std::initializer_list<u32> words) {
        w.push_back(id);
        w.insert(w.end(), words.begin(), words.end());
    }
};

struct Hooks final : sim::FeatureSystems, sim::EffectsSystems {
    Log* log = nullptr;
    long area_calls = 0, smoke_calls = 0, packets = 0;
    std::vector<u32> remote_weapon;  // by engine weapon index
    const std::vector<sim::WeaponType>* weapons = nullptr;
    std::vector<sim::Vec3i> unit_vel;   // by unit id (has motion when nonzero entry flag)
    std::vector<bool> unit_moves;
    std::vector<int> unit_owner_slot;
    u32 wtok(const sim::WeaponType& w) const { return remote_weapon[static_cast<std::size_t>(&w - weapons->data())]; }
    void update_passability(i16 x, i16 z, i16 w, i16 h) override {
        log->rec(sPassability, {static_cast<u16>(x) | (static_cast<u32>(static_cast<u16>(z)) << 16),
                                static_cast<u16>(w) | (static_cast<u32>(static_cast<u16>(h)) << 16)});
    }
    u32 create_object(u32 model) override {
        log->rec(sCreateObject, {model});
        return 0x7000;
    }
    void free_object(u32 o) override { log->rec(sFreeObject, {o}); }
    void area_damage(const sim::WeaponType& w, const sim::Vec3i& at) override {
        ++area_calls;
        log->rec(sAreaDamage, {wtok(w), 0, static_cast<u32>(at[0]), static_cast<u32>(at[1]), static_cast<u32>(at[2])});
    }
    void feature_packet(const std::uint8_t* p, bool to_host) override {
        ++packets;
        u32 a = 0, b = 0;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 2);
        if (to_host) log->rec(sNetToHost, {0x11, 0x22, 0, 6, a, b, 0});
        else log->rec(sNetBroadcast, {0x11, 0, 6, a, b, 0});
    }
    void feature_smoke(const sim::Vec3i& p) override {
        ++smoke_calls;
        log->rec(sSmoke, {0, 0, static_cast<u32>(p[0]), static_cast<u32>(p[1]), static_cast<u32>(p[2])});
    }
    void feature_steam(const sim::Vec3i& p) override {
        log->rec(sSteam, {0, 0, static_cast<u32>(p[0]), static_cast<u32>(p[1]), static_cast<u32>(p[2])});
    }
    void feature_fire_sound(i16 x, i16 z) override {
        log->rec(sSound, {0, 0, 0, static_cast<u32>(x) << 20, 0, static_cast<u32>(z) << 20});
    }
    void wreck_fire(const sim::Vec3i& p) override {
        log->rec(sFire, {0, 0xF, 900, 0, static_cast<u32>(p[0]), static_cast<u32>(p[1]), static_cast<u32>(p[2])});
    }
    bool unit_velocity(const sim::Unit& u, sim::Vec3i& v) override {
        if (!unit_moves[u.id]) return false;
        v = unit_vel[u.id];
        return true;
    }
    int unit_owner(const sim::Unit& u) override { return unit_owner_slot[u.id]; }
    void explosion_smoke(const sim::Vec3i& p) override {
        log->rec(sFire, {0, 7, 0xF, 0, static_cast<u32>(p[0]), static_cast<u32>(p[1]), static_cast<u32>(p[2])});
    }
};

// Masks the log words the original leaves undefined (stub slot numbers, uninitialised stack).
void normalise(std::vector<u32>& w, bool remote) {
    for (std::size_t i = 0; i < w.size();) {
        const Stub* s = stub_by_id(w[i]);
        if (!s) return;
        const std::size_t n = entry_words(*s);
        if (i + n > w.size()) return;
        u32* e = &w[i];
        switch (s->id) {
        case sSmoke: case sSteam: e[1] = e[2] = 0; break;     // pos pointer, effect slot
        case sSound: e[1] = e[2] = e[3] = 0; e[5] = 0; break;  // name, pos pointer, flag, y (uninitialised)
        case sFire: e[1] = 0; e[4] = 0; break;                 // pos pointer, effect slot (a register)
        case sAreaDamage: e[2] = 0; break;
        case sNetBroadcast: e[2] = 0; e[5] &= 0xFFFF; e[6] = 0; break;
        case sNetToHost: e[3] = 0; e[6] &= 0xFFFF; e[7] = 0; break;
        default: break;
        }
        (void)remote;
        i += n;
    }
}

// ---- remote structures -----------------------------------------------------------------------
struct Seq {
    u32 remote = 0;
};

// Writes a frame sequence (the in-memory GAF layout the cursors read) and registers it.
u32 make_sequence(harness::Remote& r, Layout& L, u32& at, sim::AnimLibrary& lib, std::mt19937& g, int frames, bool loop) {
    const u32 seq = at;
    std::vector<std::uint8_t> b(0x28 + static_cast<std::size_t>(frames) * 8 + static_cast<std::size_t>(frames) * 16, 0);
    const u16 nf = static_cast<u16>(frames);
    std::memcpy(&b[0], &nf, 2);
    b[2] = loop ? 1 : 0;
    sim::AnimSequence s;
    s.address = seq;
    s.loop = b[2];
    for (int f = 0; f < frames; ++f) {
        const u32 frame_at = seq + 0x28 + static_cast<u32>(frames) * 8 + static_cast<u32>(f) * 16;
        const u32 dur = g() % 5;  // includes 0 and 1
        std::memcpy(&b[0x28 + f * 8], &frame_at, 4);
        std::memcpy(&b[0x2C + f * 8], &dur, 4);
        sim::AnimFrame fr;
        fr.duration = static_cast<u16>(dur);
        fr.width = static_cast<u16>(g() % 80);
        fr.height = static_cast<u16>(g() % 80);
        fr.x_offset = static_cast<i16>(static_cast<int>(g() % 60) - 10);
        fr.y_offset = static_cast<i16>(static_cast<int>(g() % 60) - 10);
        std::uint8_t* h = &b[0x28 + static_cast<std::size_t>(frames) * 8 + static_cast<std::size_t>(f) * 16];
        std::memcpy(h, &fr.width, 2);
        std::memcpy(h + 2, &fr.height, 2);
        std::memcpy(h + 4, &fr.x_offset, 2);
        std::memcpy(h + 6, &fr.y_offset, 2);
        s.frames.push_back(fr);
    }
    r.write(seq, b.data(), b.size());
    at += (static_cast<u32>(b.size()) + 15) & ~15u;
    (void)L;
    lib.add(std::move(s));
    return seq;
}

void write_cell(std::uint8_t* d, const sim::MapCell& c) {
    std::memcpy(d, &c.unit, 2);
    std::memcpy(d + 2, &c.unit2, 2);
    d[4] = c.height;
    d[5] = c.height_max;
    d[6] = c.height_min;
    d[7] = c.metal;
    std::memcpy(d + 8, &c.feature, 2);
    std::memcpy(d + 10, &c.feature_aux, 2);
    d[12] = c.flags;
}

struct Model {
    std::string name;
    std::vector<std::uint8_t> file;
    std::unique_ptr<ta::Model3doPiece> root;
    sim::PoseModel pose;
    u32 remote = 0;
};

struct Stats {
    long scenarios = 0, ops = 0, mismatches = 0, places = 0, removes = 0, replaces = 0, kills = 0, ignites = 0, damages = 0,
         packets = 0, feature_steps = 0, wrecks = 0, explodes = 0, effect_steps = 0, effects_spawned = 0, faults = 0,
         slots_max = 0, effects_max = 0, debris_max = 0, burning_max = 0, rng_draws_explode = 0, moving_slot_steps = 0,
         predicted_crashes = 0, burn_weapons = 0, smoke_puffs = 0, sent_packets = 0;
};

constexpr int kMapW = 40, kMapH = 36, kTypes = 18, kWeapons = 5, kUnits = 6, kPool = 24;

bool run_scenario(harness::Remote& r, Layout& L, const std::vector<Model>& models, std::uint32_t seed, int ops, Stats& st) {
    std::mt19937 g(seed);
    Log elog;
    Hooks hooks;
    hooks.log = &elog;

    // Map: heights, sea, cells.
    sim::HeightMap hm(kMapW, kMapH);
    sim::MapGrid map;
    map.width = kMapW;
    map.height = kMapH;
    map.cells.assign(static_cast<std::size_t>(kMapW * kMapH), sim::MapCell{});
    const int base = static_cast<int>(g() % 60);
    for (int z = 0; z < kMapH; ++z)
        for (int x = 0; x < kMapW; ++x) {
            const int h = std::clamp(base + static_cast<int>(g() % 40) + (x + z) / 2 - 10, 0, 255);
            hm.at(x, z) = static_cast<u8>(h);
            sim::MapCell& c = map.cells[static_cast<std::size_t>(z * kMapW + x)];
            c.height = static_cast<u8>(h);
            c.flags = static_cast<u8>((g() & 0x86) | 0x50);
            c.unit = static_cast<u16>(g() % 5 == 0 ? 1 + g() % 6 : 0);
            c.feature_aux = static_cast<u16>(g());
        }
    sim::refresh_height_range(map, 0, 0, kMapW, kMapH);
    const u8 sea = static_cast<u8>(base + 20 + g() % 30);

    // Sequences.
    sim::AnimLibrary lib;
    u32 seq_at = L.seqs;
    std::vector<u32> pool;
    for (int k = 0; k < kPool; ++k) pool.push_back(make_sequence(r, L, seq_at, lib, g, 1 + static_cast<int>(g() % 7), g() % 3 == 0));
    auto any_seq = [&](int none_one_in) -> u32 { return g() % none_one_in == 0 ? 0 : pool[g() % pool.size()]; };
    sim::EffectsEnvironment fxenv;
    static const int kGlowFrames[3] = {12, 15, 15};
    for (int k = 0; k < 3; ++k) {
        const u32 s = make_sequence(r, L, seq_at, lib, g, kGlowFrames[k], false);
        fxenv.glow[static_cast<std::size_t>(k)] = s;
    }
    for (int k = 0; k < 6; ++k) fxenv.bitmap[static_cast<std::size_t>(k)] = make_sequence(r, L, seq_at, lib, g, 2 + static_cast<int>(g() % 8), false);
    fxenv.water_splash = make_sequence(r, L, seq_at, lib, g, 3 + static_cast<int>(g() % 5), false);
    fxenv.lava_splash = make_sequence(r, L, seq_at, lib, g, 3 + static_cast<int>(g() % 5), false);
    fxenv.gravity = static_cast<i32>(g() % 3 ? 112 + g() % 200 : g() % 4000);
    fxenv.sea_level = sea;
    fxenv.no_sea_level_trigger = g() % 4 == 0;
    fxenv.lava_world = g() % 4 == 0;

    // Weapons.
    std::vector<sim::WeaponType> weapons(kWeapons);
    for (int k = 0; k < kWeapons; ++k) {
        sim::WeaponType& w = weapons[static_cast<std::size_t>(k)];
        w.index = static_cast<u8>(10 + k);
        w.default_damage = static_cast<u16>(g() % 4 ? g() % 400 : g());
        w.fire_starter = static_cast<u8>(g() % 2 ? g() % 100 : 0);
        w.explosion_seq = any_seq(4);
        w.water_explosion_seq = any_seq(4);
        const u32 at = L.weapons + static_cast<u32>(k) * kWeaponSize;
        std::vector<std::uint8_t> b(kWeaponSize, 0);
        std::memcpy(&b[0x78], &w.explosion_seq, 4);
        std::memcpy(&b[0x7C], &w.water_explosion_seq, 4);
        std::memcpy(&b[0xD4], &w.default_damage, 2);
        b[0x10A] = w.index;
        b[0x10B] = w.fire_starter;
        r.write(at, b.data(), b.size());
        hooks.remote_weapon.push_back(at);
    }
    hooks.weapons = &weapons;
    std::vector<const sim::WeaponType*> by_index(256, nullptr);
    for (auto& w : weapons) by_index[w.index] = &w;

    // Feature types.
    std::vector<sim::FeatureTypeInfo> types(kTypes);
    {
        std::vector<std::uint8_t> d(static_cast<std::size_t>(kTypes) * kDefSize, 0);
        for (int k = 0; k < kTypes; ++k) {
            sim::FeatureTypeInfo& t = types[static_cast<std::size_t>(k)];
            t.footprint_x = static_cast<i16>(1 + g() % 3);
            t.footprint_z = static_cast<i16>(1 + g() % 3);
            t.height = static_cast<u8>(g() % 30);
            const bool flat = g() % 3 != 0;
            t.flags = static_cast<u16>((flat ? 1 : 0) | (g() % 3 == 0 ? 2 : 0) | (g() % 2 ? 0x10 : 0) | (g() % 10 == 0 ? 0x20 : 0) |
                                       (g() % 8 == 0 ? 0x200 : 0) | 0xB000);
            t.damage = static_cast<u16>(g() % 4 ? g() % 1500 : g());
            t.spark_time = static_cast<u16>(g() % 4 ? g() % 40 : g() % 400);
            t.spread_chance = static_cast<u8>(g() % 3 ? g() % 100 : g() % 256);
            t.reproduce = static_cast<u8>(g() % 3 == 0 ? g() % 100 : 0);
            t.reproduce_area = static_cast<u8>(g() % 12);
            t.burn_weapon = g() % 3 == 0 ? &weapons[g() % kWeapons] : nullptr;
            auto link = [&]() -> u16 { return g() % 3 == 0 ? u16{0xFFFF} : static_cast<u16>(g() % kTypes); };
            t.dead = link();
            t.burnt = link();
            t.reclamate = link();
            if (flat)
                for (int s = 0; s < 8; ++s) t.sequences[static_cast<std::size_t>(s)] = any_seq(s < 2 ? 4 : 2);
            t.model = flat ? 0 : 0x00450000u + static_cast<u32>(k);  // object state stub only logs it
            std::uint8_t* p = &d[static_cast<std::size_t>(k) * kDefSize];
            std::memcpy(p + 0x94, &t.footprint_x, 2);
            std::memcpy(p + 0x96, &t.footprint_z, 2);
            if (!flat) std::memcpy(p + 0x98, &t.model, 4);
            for (int s = 0; s < 8; ++s) std::memcpy(p + 0xAC + 4 * s, &t.sequences[static_cast<std::size_t>(s)], 4);
            const u32 bw = t.burn_weapon ? hooks.wtok(*t.burn_weapon) : 0;
            std::memcpy(p + 0xE4, &bw, 4);
            std::memcpy(p + 0xE8, &t.spark_time, 2);
            std::memcpy(p + 0xEA, &t.damage, 2);
            std::memcpy(p + 0xF4, &t.dead, 2);
            std::memcpy(p + 0xF6, &t.burnt, 2);
            std::memcpy(p + 0xF8, &t.reclamate, 2);
            p[0xFB] = t.spread_chance;
            p[0xFC] = t.reproduce;
            p[0xFD] = t.reproduce_area;
            std::memcpy(p + 0xFE, &t.flags, 2);
        }
        r.write(L.defs, d.data(), d.size());
    }

    // Engine state.
    sim::FeatureState fs;
    fs.reset();
    sim::FeatureEnvironment fenv;
    fenv.sea_level = sea;
    fenv.gravity = fxenv.gravity;
    fenv.wind = {static_cast<i32>(g() % 0x40000) - 0x20000, 0, static_cast<i32>(g() % 0x40000) - 0x20000};
    const u16 options = static_cast<u16>(g() % 5 ? 8 : 0);
    fenv.destructible = (options & 8) != 0;
    const u32 provider = g() % 5 == 0 ? 3 : 1;
    const bool host = g() % 2 == 0;
    fenv.network_role = static_cast<u8>(provider == 3 ? (host ? 1 : 2) : 0);
    fenv.game_tick = static_cast<u32>(g() % 1000);
    sim::BattleRng rng;
    rng.set_state(g() | 1);
    sim::Lcg15 crt;
    crt.set_state(g());
    sim::FeatureWorld fw{fs, map, types, hm, lib, fenv, hooks, rng, crt};
    sim::init_feature_type_cursors(fw);
    sim::EffectsState es;
    es.reset();
    sim::EffectsWorld ew{es, map, lib, fxenv, hooks, rng};

    // Remote game state.
    {
        std::vector<std::uint8_t> zero(0x40000, 0);
        r.write(L.gs, zero.data(), zero.size());
        std::vector<std::uint8_t> cb(map.cells.size() * kCellSize, 0);
        for (std::size_t i = 0; i < map.cells.size(); ++i) write_cell(&cb[i * kCellSize], map.cells[i]);
        r.write(L.cells, cb.data(), cb.size());
        std::vector<std::uint8_t> sb(0x18000);
        for (int i = 0; i < sim::kFeatureSlots; ++i) std::memcpy(&sb[static_cast<std::size_t>(i) * kSlotBytes], fs.slots[static_cast<std::size_t>(i)].b.data(), kSlotBytes);
        r.write(L.slots, sb.data(), sb.size());
        // Type cursors.
        for (int k = 0; k < kTypes; ++k)
            for (int c = 0; c < 2; ++c) {
                const sim::AnimCursor& a = fs.type_cursors[static_cast<std::size_t>(k)][static_cast<std::size_t>(c)];
                std::uint8_t b[12];
                std::memcpy(b, &a.frame, 2);
                std::memcpy(b + 2, &a.countdown, 2);
                b[4] = a.loop;
                b[5] = a.pad[0];
                b[6] = a.pad[1];
                b[7] = a.pad[2];
                std::memcpy(b + 8, &a.seq, 4);
                r.write(L.defs + static_cast<u32>(k) * kDefSize + 0xCC + static_cast<u32>(c) * 12, b, 12);
            }
        const u32 gs = L.gs;
        r.w32(gs + gCells, L.cells);
        r.w32(gs + gMapW, kMapW);
        r.w32(gs + gMapH, kMapH);
        r.w32(gs + gFeatureDefs, L.defs);
        r.w32(gs + gFeatureCount, kTypes);
        r.w32(gs + gAnimTable, L.slots);
        r.w32(gs + gActive, static_cast<u32>(-1));
        r.w32(gs + gIdle, static_cast<u32>(-1));
        r.w32(gs + gFree, 0);
        r.w32(gs + gSweep, 0);
        r.w32(gs + gGravity, static_cast<u32>(fxenv.gravity));
        r.write(gs + gSea, &sea, 1);
        r.w32(gs + gWind, static_cast<u32>(fenv.wind[0]));
        r.w32(gs + gWind + 8, static_cast<u32>(fenv.wind[2]));
        r.write(gs + gOptions, &options, 2);
        r.w32(gs + gTick, fenv.game_tick);
        r.w32(gs + gMapInfo, L.mapinfo);
        r.w32(L.mapinfo, provider);
        r.w32(L.mapinfo + 0xD44, fxenv.lava_world ? 1 : 0);
        r.w32(L.mapinfo + 0xD48, fxenv.no_sea_level_trigger ? 1 : 0);
        const u8 view = static_cast<u8>(g() % 4);
        r.write(gs + gLocalView, &view, 1);
        r.w32(gs + gPlayers + view * kPlayerSize + 0x27, L.lobby);
        const u8 lobby = host ? 1 : 0;
        r.write(L.lobby + 0x97, &lobby, 1);
        r.w32(gs + gWaterSplash, fxenv.water_splash);
        r.w32(gs + gLavaSplash, fxenv.lava_splash);
        for (int k = 0; k < 6; ++k) r.w32(gs + gBitmaps + 4 * k, fxenv.bitmap[static_cast<std::size_t>(k)]);
        for (int k = 0; k < 3; ++k) r.w32(gs + gGlows + 4 * k, fxenv.glow[static_cast<std::size_t>(k)]);
        // Shard slots: piece headers with their own point and primitive buffers.
        for (int k = 0; k < sim::kMaxShards; ++k) {
            const u32 at = gs + gShards + static_cast<u32>(k) * kShardSize;
            const u8 freeb = 0xFF;
            r.write(at, &freeb, 1);
            r.w32(at + 4, 8);
            r.w32(at + 8, 6);
            r.w32(at + 0xC, static_cast<u32>(-1));
            r.w32(at + 0x24, L.shard_buf + static_cast<u32>(k) * 0x60);
            r.w32(at + 0x28, L.shard_buf + 0x8000 + static_cast<u32>(k) * 0xC0);
        }
        std::vector<std::uint8_t> z(0x190, 0);
        r.write(kDebrisSlots, z.data(), z.size());
        r.w32(kDebrisCache, sim::kDebrisCacheSize);
        r.w32(kDebrisCache + 4, L.cache_buf);
        r.w32(kDebrisCache + 8, L.cache_buf);
        r.w32(L.cache_buf, 0);
        r.w32(L.cache_buf + 4, sim::kDebrisCacheSize);
        r.w32(kBattleRngState, rng.state());
        r.w32(L.rand_state, crt.state());
        r.w32(L.log_ptr, L.log_base);
        r.w32(L.bump_ptr, L.heap);
    }

    // Units with real models: dead units leave wrecks, scripts blow pieces off.
    std::vector<sim::Unit> units(kUnits + 1);
    hooks.unit_vel.assign(kUnits + 1, {});
    hooks.unit_moves.assign(kUnits + 1, false);
    hooks.unit_owner_slot.assign(kUnits + 1, 0);
    std::vector<sim::WreckSource> wreck(kUnits + 1);
    std::vector<u32> remote_unit(kUnits + 1), remote_state(kUnits + 1);
    for (int i = 1; i <= kUnits; ++i) {
        const Model& m = models[g() % models.size()];
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        u.id = static_cast<u16>(i);
        const u32 at = L.units + static_cast<u32>(i) * kUnitSize;
        const u32 def = L.defs_unit + static_cast<u32>(i) * 0x249 % 0x1E00;
        std::vector<std::uint8_t> ub(kUnitSize, 0), db(0x249, 0);
        u.cell_x = static_cast<i16>(static_cast<int>(g() % (kMapW + 4)) - 2);
        u.cell_z = static_cast<i16>(static_cast<int>(g() % (kMapH + 4)) - 2);
        u.pos = {static_cast<i32>((u.cell_x * 16 + static_cast<int>(g() % 16)) << 16) + static_cast<i32>(g() & 0xFFFF),
                 static_cast<i32>((g() % 200) << 16), static_cast<i32>((u.cell_z * 16 + static_cast<int>(g() % 16)) << 16)};
        u.pitch = static_cast<i16>(g());
        u.heading = static_cast<u16>(g());
        u.bank = static_cast<i16>(g());
        wreck[static_cast<std::size_t>(i)].corpse = static_cast<u16>(g() % 6 == 0 ? 0xFFFF : g() % kTypes);
        wreck[static_cast<std::size_t>(i)].feature_unit = g() % 5 == 0;
        wreck[static_cast<std::size_t>(i)].owner = static_cast<u8>(g() % 11);
        hooks.unit_owner_slot[static_cast<std::size_t>(i)] = wreck[static_cast<std::size_t>(i)].owner;
        const bool moves = g() % 2 == 0;
        hooks.unit_moves[static_cast<std::size_t>(i)] = moves;
        hooks.unit_vel[static_cast<std::size_t>(i)] = {static_cast<i32>(g() % 0x60000) - 0x30000, static_cast<i32>(g() % 0x60000) - 0x30000,
                                                      static_cast<i32>(g() % 0x60000) - 0x30000};
        if (moves) {
            const u32 mo = L.motion + static_cast<u32>(i) * 0x40;
            for (int k = 0; k < 3; ++k) r.w32(mo + 8 + 4 * k, static_cast<u32>(hooks.unit_vel[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)]));
            std::memcpy(&ub[0], &mo, 4);
        }
        std::memcpy(&ub[0x64], &u.pitch, 2);
        std::memcpy(&ub[0x66], &u.heading, 2);
        std::memcpy(&ub[0x68], &u.bank, 2);
        std::memcpy(&ub[0x6A], u.pos.data(), 12);
        std::memcpy(&ub[0x76], &u.cell_x, 2);
        std::memcpy(&ub[0x78], &u.cell_z, 2);
        std::memcpy(&ub[0x92], &def, 4);
        ub[0xFF] = wreck[static_cast<std::size_t>(i)].owner;
        std::memcpy(&db[0x1BC], &wreck[static_cast<std::size_t>(i)].corpse, 2);
        const u32 tf = wreck[static_cast<std::size_t>(i)].feature_unit ? 0x1000000u : 0u;
        std::memcpy(&db[0x241], &tf, 4);
        r.write(at, ub.data(), ub.size());
        r.write(def, db.data(), db.size());
        u32 state = 0;
        if (!r.call(kBuildState, 0, {m.remote, L.ctx + 0x1800, at}, &state) || !state) {  // an empty script: no reordering
            std::printf("build state failed\n");
            return false;
        }
        r.w32(at + 0x9E, state);
        u.pose.build(m.pose, {});
        remote_unit[static_cast<std::size_t>(i)] = at;
        remote_state[static_cast<std::size_t>(i)] = state;
    }
    auto sync_unit = [&](int i) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        r.call(kSync, 0, {remote_unit[static_cast<std::size_t>(i)]});
        u.pose.sync(u.pitch, u.heading, u.bank);
    };
    for (int i = 1; i <= kUnits; ++i) sync_unit(i);

    // Original routines; a failed call is a crash of the original.
    bool orig_failed = false;
    auto call = [&](u32 fn, u32 self, const std::vector<u32>& args) {
        if (!r.call(fn, self, args)) orig_failed = true;
    };
    // ---- comparison ----
    auto compare = [&](const std::string& what) -> bool {
        bool ok = true;
        st.burn_weapons += hooks.area_calls;
        st.smoke_puffs += hooks.smoke_calls;
        st.sent_packets += hooks.packets;
        hooks.area_calls = hooks.smoke_calls = hooks.packets = 0;
        if (orig_failed && (!fs.fault.empty() || !es.fault.empty())) {
            ++st.predicted_crashes;
            std::printf("seed %u: after %s: the original crashed where the engine predicted it (%s%s)\n", seed, what.c_str(),
                        fs.fault.c_str(), es.fault.c_str());
            return false;
        }
        auto bad = [&](const std::string& m) {
            if (ok) std::printf("seed %u: after %s: %s\n", seed, what.c_str(), m.c_str());
            ok = false;
        };
        std::vector<std::uint8_t> cb(map.cells.size() * kCellSize);
        r.read(L.cells, cb.data(), cb.size());
        for (std::size_t i = 0; i < map.cells.size() && ok; ++i) {
            std::uint8_t e[kCellSize] = {};
            write_cell(e, map.cells[i]);
            if (std::memcmp(e, &cb[i * kCellSize], kCellSize) != 0) {
                char buf[160];
                std::snprintf(buf, sizeof buf, "cell %zu (%zu,%zu): feature orig %04x ours %04x, aux %04x/%04x, flags %02x/%02x", i, i % kMapW,
                              i / kMapW, cb[i * kCellSize + 8] | cb[i * kCellSize + 9] << 8, map.cells[i].feature,
                              cb[i * kCellSize + 10] | cb[i * kCellSize + 11] << 8, map.cells[i].feature_aux, cb[i * kCellSize + 12],
                              map.cells[i].flags);
                bad(buf);
            }
        }
        std::vector<std::uint8_t> sb(0x18000);
        r.read(L.slots, sb.data(), sb.size());
        for (int i = 0; i < sim::kFeatureSlots && ok; ++i)
            if (std::memcmp(&sb[static_cast<std::size_t>(i) * kSlotBytes], fs.slots[static_cast<std::size_t>(i)].b.data(), kSlotBytes) != 0) {
                std::string m = "slot " + std::to_string(i) + " bytes differ at";
                for (u32 k = 0; k < kSlotBytes; ++k)
                    if (sb[i * kSlotBytes + k] != fs.slots[static_cast<std::size_t>(i)].b[k]) m += " +" + std::to_string(k);
                bad(m);
            }
        if (static_cast<i32>(r.r32(L.gs + gActive)) != fs.active || static_cast<i32>(r.r32(L.gs + gIdle)) != fs.idle ||
            static_cast<i32>(r.r32(L.gs + gFree)) != fs.free)
            bad("list heads");
        if (static_cast<i32>(r.r32(L.gs + gSweep)) != fs.sweep) bad("reproduction sweep");
        for (int k = 0; k < kTypes; ++k)
            for (int c = 0; c < 2; ++c) {
                std::uint8_t b[12];
                r.read(L.defs + static_cast<u32>(k) * kDefSize + 0xCC + static_cast<u32>(c) * 12, b, 12);
                const sim::AnimCursor& a = fs.type_cursors[static_cast<std::size_t>(k)][static_cast<std::size_t>(c)];
                u16 f, cd;
                u32 s;
                std::memcpy(&f, b, 2);
                std::memcpy(&cd, b + 2, 2);
                std::memcpy(&s, b + 8, 4);
                if (f != a.frame || cd != a.countdown || s != a.seq || b[4] != a.loop) bad("type cursor");
            }
        if (r.r32(kBattleRngState) != rng.state()) bad("battle RNG");
        if (r.r32(L.rand_state) != crt.state()) bad("rand() state");
        // Effects.
        const i32 count = static_cast<i32>(r.r32(L.gs + gEffectCount));
        if (count != es.count) bad("effect count orig " + std::to_string(count) + " ours " + std::to_string(es.count));
        for (i32 k = 0; k < (std::min)(count, es.count) && ok; ++k) {
            std::uint8_t b[kEffectSize];
            r.read(L.gs + gEffects + static_cast<u32>(k) * kEffectSize, b, kEffectSize);
            const sim::WorldEffect& e = es.effects[static_cast<std::size_t>(k)];
            u32 shard;
            std::memcpy(&shard, b, 4);
            const i32 si = shard ? static_cast<i32>((shard - (L.gs + gShards)) / kShardSize) : -1;
            if (si != e.shard) bad("effect " + std::to_string(k) + " shard");
            auto cur = [&](int off, const sim::AnimCursor& c, const char* n) {
                u16 f, cd;
                u32 s;
                std::memcpy(&f, b + off, 2);
                std::memcpy(&cd, b + off + 2, 2);
                std::memcpy(&s, b + off + 8, 4);
                if (s != c.seq || (s && (f != c.frame || cd != c.countdown || b[off + 4] != c.loop)))
                    bad("effect " + std::to_string(k) + " " + n);
            };
            cur(4, e.art, "art");
            cur(0x10, e.glow, "glow");
            i32 v[9];
            std::memcpy(v, b + 0x1C, 36);
            for (int a = 0; a < 3; ++a)
                if (v[a] != e.pos[static_cast<std::size_t>(a)]) bad("effect " + std::to_string(k) + " pos");
            if (si >= 0) {
                for (int a = 0; a < 3; ++a) {
                    if (v[3 + a] != e.carry[static_cast<std::size_t>(a)]) bad("effect " + std::to_string(k) + " carry");
                    if (v[6 + a] != e.vel[static_cast<std::size_t>(a)]) bad("effect " + std::to_string(k) + " vel");
                }
                i32 spin[3];
                i16 ang[3];
                u16 fl;
                std::memcpy(spin, b + 0x40, 12);
                std::memcpy(ang, b + 0x4C, 6);
                std::memcpy(&fl, b + 0x52, 2);
                for (int a = 0; a < 3; ++a)
                    if (spin[a] != e.spin[static_cast<std::size_t>(a)] || ang[a] != e.angle[static_cast<std::size_t>(a)]) bad("effect spin/angle");
                if ((fl & 1) != (e.flags & 1)) bad("effect flags");
            }
        }
        for (int k = 0; k < sim::kMaxShards && ok; ++k) {
            u8 fb = 0;
            r.read(L.gs + gShards + static_cast<u32>(k) * kShardSize, &fb, 1);
            if (fb != es.shards[static_cast<std::size_t>(k)].free) bad("shard " + std::to_string(k) + " free byte");
            if (fb == 0) {
                i32 pts[24];
                r.read(L.shard_buf + static_cast<u32>(k) * 0x60, pts, sizeof pts);
                for (int p = 0; p < 8; ++p)
                    for (int a = 0; a < 3; ++a)
                        if (pts[p * 3 + a] != es.shards[static_cast<std::size_t>(k)].points[static_cast<std::size_t>(p)][static_cast<std::size_t>(a)])
                            bad("shard " + std::to_string(k) + " points");
            }
        }
        // Debris.
        u32 slots[sim::kMaxDebris];
        r.read(kDebrisSlots, slots, sizeof slots);
        for (int k = 0; k < sim::kMaxDebris && ok; ++k) {
            const i32 off = slots[k] ? static_cast<i32>(slots[k] - L.cache_buf) : -1;
            if (off != es.debris_block[static_cast<std::size_t>(k)]) {
                bad("debris slot " + std::to_string(k) + " orig " + std::to_string(off) + " ours " + std::to_string(es.debris_block[static_cast<std::size_t>(k)]));
                break;
            }
            if (off < 0) continue;
            std::uint8_t b[0x66];
            r.read(slots[k], b, sizeof b);
            const sim::DebrisPiece& d = es.debris[static_cast<std::size_t>(k)];
            i32 v[12];
            std::memcpy(v, b + 8, 36);
            for (int a = 0; a < 3; ++a) {
                if (v[a] != d.spin[static_cast<std::size_t>(a)]) bad("debris spin");
                if (v[3 + a] != d.vel[static_cast<std::size_t>(a)]) bad("debris vel");
            }
            if (v[6] != d.ttl) bad("debris ttl");
            u32 fl;
            std::memcpy(&fl, b + 0x28, 4);
            if ((fl & 0x3F) != (d.flags & 0x3F)) bad("debris flags");
            i32 pos[3];
            i16 turn[3];
            std::memcpy(turn, b + 0x40, 6);
            std::memcpy(pos, b + 0x46, 12);
            for (int a = 0; a < 3; ++a) {
                if (pos[a] != d.pos[static_cast<std::size_t>(a)]) bad("debris pos");
                if (turn[a] != d.turn[static_cast<std::size_t>(a)]) bad("debris turn");
            }
        }
        {
            const u32 cursor = r.r32(kDebrisCache + 8) - L.cache_buf;
            if (static_cast<i32>(cursor) != es.cache.cursor) bad("cache cursor");
            for (i32 q = 0; q < sim::kDebrisCacheSize && ok;) {
                const u32 owner = r.r32(L.cache_buf + static_cast<u32>(q));
                const i32 size = static_cast<i32>(r.r32(L.cache_buf + static_cast<u32>(q) + 4));
                auto it = es.cache.blocks.find(q);
                const i32 oslot = owner ? static_cast<i32>((owner - kDebrisSlots) / 4) : -1;
                if (it == es.cache.blocks.end() || it->second.size != size || it->second.owner != oslot) {
                    bad("cache block at " + std::to_string(q));
                    break;
                }
                if (size <= 0) break;
                q += size;
            }
        }
        // Piece visibility.
        for (int i = 1; i <= kUnits && ok; ++i) {
            const sim::Unit& u = units[static_cast<std::size_t>(i)];
            for (std::size_t p = 0; p < u.pose.pieces.size(); ++p) {
                u16 f = 0;
                r.read(remote_state[static_cast<std::size_t>(i)] + 0x22 + static_cast<u32>(p) * 0x36 + 0x28, &f, 2);
                if (f != u.pose.pieces[p].flags) bad("piece flags");
            }
        }
        // Stub log.
        const u32 end = r.r32(L.log_ptr);
        std::vector<u32> got((end - L.log_base) / 4);
        if (!got.empty()) r.read(L.log_base, got.data(), got.size() * 4);
        std::vector<u32> want = elog.w;
        normalise(got, true);
        normalise(want, false);
        if (got != want) {
            std::string m = "call log: orig";
            for (std::size_t k = 0; k < got.size() && k < 24; ++k) m += " " + std::to_string(got[k]);
            m += " | ours";
            for (std::size_t k = 0; k < want.size() && k < 24; ++k) m += " " + std::to_string(want[k]);
            bad(m);
        }
        r.w32(L.log_ptr, L.log_base);
        elog.w.clear();
        const bool fault = !fs.fault.empty() || !es.fault.empty();
        if (fault || orig_failed) {
            if (fault && orig_failed) {
                ++st.predicted_crashes;
                std::printf("seed %u: after %s: the original crashed where the engine predicted it (%s%s)\n", seed, what.c_str(),
                            fs.fault.c_str(), es.fault.c_str());
                return false;
            }
            ++st.faults;
            std::printf("seed %u: after %s: %s %s%s\n", seed, what.c_str(), orig_failed ? "original crashed" : "engine fault:",
                        fs.fault.c_str(), es.fault.c_str());
            ++st.mismatches;
            return false;
        }
        if (!ok) ++st.mismatches;
        return ok;
    };
    if (!compare("setup")) return true;

    auto cell_index = [&](int x, int z) { return z * kMapW + x; };
    auto random_cell = [&]() { return static_cast<int>(g() % static_cast<u32>(kMapW * kMapH)); };
    // Mostly a cell holding a feature (its own cell), else any cell.
    auto feature_cell = [&]() {
        if (g() % 4 != 0) {
            std::vector<int> with;
            for (int i = 0; i < kMapW * kMapH; ++i)
                if (map.cells[static_cast<std::size_t>(i)].feature < 0xFFFB) with.push_back(i);
            if (!with.empty()) return with[g() % with.size()];
        }
        return random_cell();
    };
    auto stats_tick = [&]() {
        long active = 0, burning = 0;
        for (i32 i = fs.active; i != -1 && active < 5000; i = fs.slots[static_cast<std::size_t>(i)].get<i16>(0)) {
            ++active;
            const sim::FeatureSlot& fsl = fs.slots[static_cast<std::size_t>(i)];
            const u16 ty = fsl.get<u16>(0x2C);
            const bool flat = ty < types.size() && (types[ty].flags & 1);
            if (flat && (fsl.get<u8>(0x2F) & 1)) ++burning;
            if (!flat && (fsl.get<i32>(0x14) || fsl.get<i32>(0x18) || fsl.get<i32>(0x1C))) ++st.moving_slot_steps;
        }
        st.slots_max = (std::max)(st.slots_max, active);
        st.burning_max = (std::max)(st.burning_max, burning);
        st.effects_max = (std::max)(st.effects_max, static_cast<long>(es.count));
        long dn = 0;
        for (i32 b : es.debris_block) dn += b != -1;
        st.debris_max = (std::max)(st.debris_max, dn);
    };
    // Initial features.
    for (int k = 0; k < 120; ++k) {
        const int c = random_cell();
        const u16 type = static_cast<u16>(g() % 12 == 0 ? 0xFFFC : g() % kTypes);
        u32 res = 0;
        call(kPlace, 0, {L.cells + static_cast<u32>(c) * kCellSize, type, 0, 0, 10});
        (void)res;
        sim::place_feature(fw, c, type, nullptr, nullptr, 10);
    }
    if (!compare("initial features")) return true;

    for (int op = 0; op < ops; ++op) {
        const int pick = static_cast<int>(g() % 100);
        std::string what;
        if (pick < 30) {
            ++fenv.game_tick;
            r.w32(L.gs + gTick, fenv.game_tick);
            call(kFeatureStep, 0, {});
            sim::step_features(fw);
            what = "feature step";
            ++st.feature_steps;
        } else if (pick < 45) {
            call(kEffectsStep, 0, {});
            sim::step_effects(ew);
            what = "effects step";
            ++st.effect_steps;
        } else if (pick < 50) {
            const int c = random_cell();
            const u16 type = static_cast<u16>(g() % 15 == 0 ? 0xFFFC : g() % kTypes);
            const bool with_pos = g() % 3 == 0;
            const sim::Vec3i pos = {static_cast<i32>(g()), static_cast<i32>(g()), static_cast<i32>(g())};
            const std::array<i16, 3> ang = {static_cast<i16>(g()), static_cast<i16>(g()), static_cast<i16>(g())};
            const u8 owner = static_cast<u8>(g() % 16);
            const u32 args = L.ctx + 0x1000;
            r.write(args, pos.data(), 12);
            r.write(args + 0x10, ang.data(), 6);
            call(kPlace, 0, {L.cells + static_cast<u32>(c) * kCellSize, type, with_pos ? args : 0, with_pos ? args + 0x10 : 0, owner});
            sim::place_feature(fw, c, type, with_pos ? &pos : nullptr, with_pos ? &ang : nullptr, owner);
            what = "place";
            ++st.places;
        } else if (pick < 53) {
            const int c = feature_cell();
            const bool force = g() % 4 == 0;
            call(kRemove, 0, {L.cells + static_cast<u32>(c) * kCellSize, force ? 1u : 0u});
            sim::remove_feature(fw, c, force);
            what = "remove";
            ++st.removes;
        } else if (pick < 56) {
            const int fc = feature_cell();
            const int x = fc % kMapW, z = fc / kMapW;
            const bool rec = g() % 3 == 0;
            call(kReplace, 0, {static_cast<u32>(x), static_cast<u32>(z), rec ? 1u : 0u});
            sim::replace_feature(fw, x, z, rec);
            what = "next stage";
            ++st.replaces;
        } else if (pick < 61) {
            const int fc = feature_cell();
            const int x = fc % kMapW, z = fc / kMapW;
            const bool rec = g() % 3 == 0;
            call(kKill, 0, {static_cast<u32>(x), static_cast<u32>(z), rec ? 1u : 0u});
            sim::kill_feature(fw, x, z, rec);
            what = "destroy";
            ++st.kills;
        } else if (pick < 66) {
            int x = static_cast<int>(g() % (kMapW + 2)) - 1, z = static_cast<int>(g() % (kMapH + 2)) - 1;
            if (g() % 4 != 0) {
                const int fc = feature_cell();
                x = fc % kMapW;
                z = fc / kMapW;
            }
            const bool remote = g() % 4 == 0;
            call(kIgnite, 0, {static_cast<u32>(x), static_cast<u32>(z), remote ? 1u : 0u});
            sim::ignite_feature(fw, x, z, remote);
            what = "ignite";
            ++st.ignites;
        } else if (pick < 76) {
            // Weapon damage at a feature's own cell, as area damage hands it over.
            int c = feature_cell();
            const sim::MapCell& mc = map.cells[static_cast<std::size_t>(c)];
            if (mc.feature == 0xFFFE) c -= (mc.feature_aux & 0xFF) * kMapW + (mc.feature_aux >> 8);
            if (c < 0) continue;
            const int w = static_cast<int>(g() % kWeapons);
            call(kDamage, 0, {L.cells + static_cast<u32>(c) * kCellSize, static_cast<u32>(c % kMapW), static_cast<u32>(c / kMapW),
                                hooks.remote_weapon[static_cast<std::size_t>(w)]});
            sim::damage_feature(fw, map.cells[static_cast<std::size_t>(c)], c % kMapW, c / kMapW, weapons[static_cast<std::size_t>(w)]);
            what = "damage";
            ++st.damages;
        } else if (pick < 80) {
            const int i = 1 + static_cast<int>(g() % kUnits);
            const i32 level = g() % 4 == 0 ? static_cast<i32>(g() % 6) - 1 : 1 + static_cast<i32>(g() % 3);
            const bool burn = g() % 2 == 0;
            if (level == 0) continue;
            call(kWreck, 0, {remote_unit[static_cast<std::size_t>(i)], static_cast<u32>(level), burn ? 1u : 0u});
            sim::spawn_wreck(fw, units[static_cast<std::size_t>(i)], wreck[static_cast<std::size_t>(i)], level, burn);
            what = "wreck";
            ++st.wrecks;
        } else if (pick < 92) {
            const int i = 1 + static_cast<int>(g() % kUnits);
            sim::Unit& u = units[static_cast<std::size_t>(i)];
            if (u.pose.pieces.empty()) continue;
            if (g() % 3 == 0) {
                u.pitch = static_cast<i16>(g());
                u.heading = static_cast<u16>(g());
                u.bank = static_cast<i16>(g());
                const u32 at = remote_unit[static_cast<std::size_t>(i)];
                r.write(at + 0x64, &u.pitch, 2);
                r.write(at + 0x66, &u.heading, 2);
                r.write(at + 0x68, &u.bank, 2);
                sync_unit(i);
            }
            const i32 piece = static_cast<i32>(g() % u.pose.pieces.size());
            u32 type = static_cast<u32>(g() & 0x3F);
            if (g() % 3 == 0) type |= 0x100u << (g() % 6);
            if (g() % 5 == 0) type |= 1;  // shatter more often
            const u32 ctx = L.ctx;
            r.w32(ctx + 0x540, remote_state[static_cast<std::size_t>(i)]);
            const u32 before = rng.state();
            call(kExplode, ctx, {static_cast<u32>(piece), type});
            sim::explode_piece(ew, u, piece, type);
            st.rng_draws_explode += before != rng.state();
            what = "explode " + std::to_string(type);
            ++st.explodes;
        } else if (pick < 96) {
            const sim::Vec3i pos = {static_cast<i32>((g() % (kMapW * 16)) << 16), static_cast<i32>((g() % 250) << 16),
                                    static_cast<i32>((g() % (kMapH * 16)) << 16)};
            const u32 seq = g() % 4 == 0 ? 0 : pool[g() % pool.size()];
            const i32 glow = static_cast<i32>(g() % 4) - 1;
            const i32 flags = static_cast<i32>(g() % 2);
            const u32 args = L.ctx + 0x1000;
            r.write(args, pos.data(), 12);
            call(kSpawnEffect, 0, {args, seq, static_cast<u32>(glow), static_cast<u32>(flags)});
            sim::spawn_world_effect(ew, pos, seq, glow, flags);
            what = "world effect";
            ++st.effects_spawned;
        } else {
            // A received feature packet.
            std::uint8_t pkt[8] = {0x0F, 0, 0, 0, 0, 0, 0, 0};
            const int kind = static_cast<int>(g() % 4);
            pkt[1] = kind == 0 ? 0xFD : kind == 1 ? 0xFE : kind == 2 ? 0xFF : weapons[g() % kWeapons].index;
            const u16 x = static_cast<u16>(g() % kMapW), z = static_cast<u16>(g() % kMapH);
            std::memcpy(pkt + 2, &x, 2);
            std::memcpy(pkt + 4, &z, 2);
            // The packet handler is a branch of the network routine; call the same routines it does.
            if (pkt[1] == 0xFD) call(kKill, 0, {x, z, 0});
            else if (pkt[1] == 0xFE) call(kIgnite, 0, {x, z, 1});
            else if (pkt[1] == 0xFF) call(kKill, 0, {x, z, 1});
            else {
                const int w = static_cast<int>(pkt[1]) - 10;
                call(kDamage, 0, {L.cells + static_cast<u32>(cell_index(x, z)) * kCellSize, x, z, hooks.remote_weapon[static_cast<std::size_t>(w)]});
            }
            sim::apply_feature_packet(fw, pkt, by_index);
            what = "feature packet";
            ++st.packets;
        }
        ++st.ops;
        stats_tick();
        if (!compare(what + " (op " + std::to_string(op) + ")")) return true;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: feature_harness <TotalA.exe> <game_dir> [scenarios] [operations] [first seed]\n");
        return 2;
    }
    const int scenarios = argc > 3 ? std::atoi(argv[3]) : 10;
    const int ops = argc > 4 ? std::atoi(argv[4]) : 200;
    const std::uint32_t first = argc > 5 ? static_cast<std::uint32_t>(std::strtoul(argv[5], nullptr, 0)) : 1;
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[2]);
    std::vector<Model> models;
    for (const char* name : {"armcom", "corak", "armllt", "armstump", "corraid", "armsolar", "corllt", "armpw", "corgator", "armham"}) {
        Model m;
        m.name = name;
        if (!vfs.read(std::string("objects3d\\") + name + ".3DO", m.file)) continue;
        auto loaded = ta::load_3do(m.file, true);
        if (!loaded.root) continue;
        m.root = std::move(loaded.root);
        m.pose = ta::to_pose_model(m.root.get());
        models.push_back(std::move(m));
    }
    if (models.empty()) {
        std::fprintf(stderr, "no models\n");
        return 1;
    }
    Stats st;
    for (int s = 0; s < scenarios; ++s) {
        harness::Remote r;
        Layout L;
        if (!r.start(argv[1]) || !setup(r, L)) {
            std::fprintf(stderr, "cannot start the original\n");
            return 1;
        }
        u32 at = L.models;
        for (Model& m : models) {
            m.remote = at;
            r.write(at, m.file.data(), m.file.size());
            at += (static_cast<u32>(m.file.size()) + 15) & ~15u;
            if (!r.call(kRelocate, 0, {m.remote, m.remote}) || !r.call(kNegateXZ, m.remote, {m.remote})) {
                std::fprintf(stderr, "model relocation failed\n");
                return 1;
            }
        }
        ++st.scenarios;
        if (!run_scenario(r, L, models, first + static_cast<std::uint32_t>(s), ops, st)) return 1;
        r.stop();
    }
    std::printf("feature_harness: %ld scenarios, %ld operations, %ld mismatches, %ld unpredicted faults/crashes, %ld predicted original crashes\n",
                st.scenarios, st.ops, st.mismatches, st.faults, st.predicted_crashes);
    std::printf("  places %ld, removes %ld, next stages %ld, destroys %ld, ignites %ld, damages %ld, packets %ld, wrecks %ld\n",
                st.places, st.removes, st.replaces, st.kills, st.ignites, st.damages, st.packets, st.wrecks);
    std::printf("  fire spreads with burn weapons %ld, smoke puffs (rand) %ld, packets sent %ld\n", st.burn_weapons, st.smoke_puffs,
                st.sent_packets);
    std::printf("  feature steps %ld, effect steps %ld, explodes %ld (%ld drew), world effects %ld\n", st.feature_steps,
                st.effect_steps, st.explodes, st.rng_draws_explode, st.effects_spawned);
    std::printf("  most active slots %ld, most burning %ld, most effects %ld, most debris %ld, moving 3D slot-steps %ld\n",
                st.slots_max, st.burning_max, st.effects_max, st.debris_max, st.moving_slot_steps);
    return st.mismatches ? 1 : 0;
}
