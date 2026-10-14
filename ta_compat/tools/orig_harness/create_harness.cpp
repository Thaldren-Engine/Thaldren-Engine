// Differential tester for unit creation (sim::Battle::create_unit) and the
// footprint system (sim/footprint.h). Inside a suspended TotalA.exe it builds a
// small battle: three players with unit ranges, unit types made from real
// definitions, models and scripts (with random variations of the values
// creation reads), all 256 weapon definitions, the Moon Quartet terrain with
// random metal, the spatial bucket grid and an off-map bucket. The original
// creation routine runs for real together with everything it calls that
// belongs to the simulation (field set-up, script and object state creation
// with the Create script, weapon slots, metal extraction, ground settle,
// footprint claim, activation); the motion object constructor, line of sight,
// victory conditions, group lists, passability, speech, HUD and network sends
// are logging stubs. Each scenario mixes creations (free, wanted, limited and
// failing ones) with direct footprint calls (release and claim after moves,
// wanted-cell claims, area re-claims, yard checks and yard opening through the
// script value, height ranges, metal extraction), vacated entries with random
// stale contents, and random cell occupants. After every operation it compares
// the stub log (with packet bytes), every unit table entry, the map cells, the
// bucket lists, the players' counters, scripts, poses, motion footprint steps
// and the battle RNG.
//
// Usage: create_harness <TotalA.exe> <game_dir> [scenarios] [operations]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/battle.h"
#include "sim/battle_quiet.h"
#include "sim/footprint.h"
#include "sim/unit_create.h"
#include "ta/battle_types.h"
#include "ta/cob.h"
#include "ta/hpi.h"
#include "ta/model3do.h"
#include "ta/tnt.h"
#include "ta/unitdefs.h"
#include "ta/weapondefs.h"

namespace {

using harness::u32;
using i32 = std::int32_t;
using i16 = std::int16_t;
using u16 = std::uint16_t;
using u8 = std::uint8_t;

constexpr u32 kGameStatePtr = 0x00511DE8, kBattleRngState = 0x0051FC88, kDisplayPtr = 0x0051FBD0;
constexpr u32 kRelocate = 0x004CB4C0, kNegateXZ = 0x004CB590, kAlloc = 0x004B4F10, kClock = 0x004B6340;
constexpr u32 kCreate = 0x00485F50, kClaim = 0x0047CC30, kRelease = 0x0047D0E0, kClearFootprint = 0x0047CBD0,
              kClaimWanted = 0x0047C790, kYardFree = 0x0047D970, kReclaimArea = 0x0047CAC0, kForceReclaim = 0x0047DB20,
              kHeights = 0x00483210, kMetal = 0x00437840, kWeaponSetup = 0x0049E070, kSetValue = 0x00480B20;
constexpr u32 kUnitSize = 0x118, kPlayerSize = 0x14B, kCellSize = 13, kDefSize = 0x249, kWeaponSize = 0x115;
constexpr int kTable = 27;  // entries 0..26; players own 1..8, 9..16, 17..24
constexpr int kTypes = 4;

enum class StubRet { Zero, Silent, This };
struct Stub {
    u32 address;
    int args;
    int id;
    int extra = 0;
    StubRet ret = StubRet::Zero;
};
const Stub kStubs[] = {
    {0x0043DC00, 1, 1, 0, StubRet::This},  // motion object constructor(unit) [thiscall on the new block]
    {0x00482AC0, 1, 2},                    // line of sight(unit)
    {0x00490580, 1, 3},                    // victory: unit created(unit) [thiscall]
    {0x00480250, 2, 4},                    // group list(unit, group)
    {0x00440A40, 2, 5},                    // passability(origin, size)
    {0x00440A70, 1, 6},                    // passability after release(unit)
    {0x00480EB0, 2, 10},                   // script emit sfx(piece, type) [thiscall]
    {0x00481140, 2, 11},                   // script explode(piece, type)
    {0x00481340, 3, 12},                   // script attach(unit, piece, mode)
    {0x004813B0, 1, 13},                   // script drop(unit)
    {0x0047F780, 3, 14},                   // speech(unit, event, 0)
    {0x0041C110, 1, 15},                   // HUD dirty(unit)
    {0x00451DF0, 3, 16, 6},                // network send(net id, packet, size) + the first 24 packet bytes
    {0x00456190, 2, 17},                   // broadcast script call(unit, function)
    {0x004B26F0, 1, 50, 0, StubRet::Silent},  // script checksum cache (not simulated)
};

std::size_t record_size(u32 id) {
    for (const Stub& s : kStubs)
        if (static_cast<u32>(s.id) == id) return 2 + static_cast<std::size_t>(s.args + s.extra);
    return 0;
}

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8v(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void bytes(std::initializer_list<std::uint8_t> l) {
        for (auto v : l) b.push_back(v);
    }
    void align() {
        while (b.size() % 16) b.push_back(0xCC);
    }
};

void emit_stub(Emitter& e, const Stub& s, u32 log_ptr_addr) {
    if (s.ret != StubRet::Silent) {
        e.u8v(0xA1); e.u32le(log_ptr_addr);                       // mov eax,[log_ptr]
        e.u8v(0xC7); e.u8v(0x00); e.u32le(static_cast<u32>(s.id)); // mov [eax],id
        e.bytes({0x89, 0x48, 0x04});                              // mov [eax+4],ecx
        for (int i = 0; i < s.args; ++i) {
            e.bytes({0x8B, 0x54, 0x24, static_cast<std::uint8_t>(4 + 4 * i)});
            e.bytes({0x89, 0x50, static_cast<std::uint8_t>(8 + 4 * i)});
        }
        if (s.extra) {
            e.bytes({0x8B, 0x54, 0x24, 0x08});  // the second argument points at the bytes
            for (int k = 0; k < s.extra; ++k) {
                e.bytes({0x8B, 0x4A, static_cast<std::uint8_t>(4 * k)});
                e.bytes({0x89, 0x48, static_cast<std::uint8_t>(8 + 4 * (s.args + k))});
            }
            e.bytes({0x8B, 0x48, 0x04});  // restore ecx from the record
        }
        e.u8v(0x83); e.u8v(0x05); e.u32le(log_ptr_addr); e.u8v(static_cast<std::uint8_t>(4 * (s.args + s.extra + 2)));
    }
    if (s.ret == StubRet::This) e.bytes({0x8B, 0xC1});  // mov eax,ecx
    else e.bytes({0x33, 0xC0});                         // xor eax,eax
    if (s.args == 0) e.u8v(0xC3);
    else {
        e.u8v(0xC2); e.u8v(static_cast<std::uint8_t>(4 * s.args)); e.u8v(0x00);
    }
}

struct Layout {
    u32 heap = 0, scenario_heap = 0, bump_ptr = 0, stubs = 0, log_ptr = 0, log_base = 0, ret_val = 0, clock = 0, gs = 0, units = 0,
        defs = 0, models = 0, cells = 0, buckets = 0, off_map = 0, files = 0, empty_def = 0, weapons = 0, yards = 0, display = 0,
        victory = 0;
    i32 bw = 0, bh = 0;
};

bool setup(harness::Remote& r, Layout& L, const sim::HeightMap& hm) {
    L.heap = r.alloc(128u << 20);
    L.bump_ptr = r.alloc(0x1000);
    L.stubs = r.alloc(0x4000, PAGE_EXECUTE_READWRITE);
    L.log_ptr = r.alloc(0x1000);
    L.log_base = r.alloc(4u << 20);
    L.ret_val = r.alloc(0x1000);
    L.gs = r.alloc(0x40000);
    L.units = r.alloc(static_cast<SIZE_T>(kTable + 2) * kUnitSize);
    L.defs = r.alloc(0x2000);
    L.models = r.alloc(0x1000);
    L.cells = r.alloc(static_cast<SIZE_T>(hm.width()) * hm.height() * kCellSize);
    L.bw = (hm.width() + 7) >> 3;
    L.bh = (hm.height() + 7) >> 3;
    L.buckets = r.alloc(static_cast<SIZE_T>(L.bw) * L.bh * 10 + 16);
    L.off_map = r.alloc(0x1000);
    L.files = r.alloc(32u << 20);
    L.empty_def = r.alloc(0x1000);
    L.weapons = r.alloc(256 * kWeaponSize + 16);
    L.yards = r.alloc(0x4000);
    L.display = r.alloc(0x1000);
    L.victory = r.alloc(0x1000);
    if (!L.heap || !L.stubs || !L.log_base || !L.gs || !L.units || !L.cells || !L.buckets || !L.files || !L.weapons || !L.yards ||
        !L.display)
        return false;
    L.clock = L.ret_val + 0x40;
    Emitter e;
    auto here = [&]() { return L.stubs + static_cast<u32>(e.b.size()); };
    // Bump allocators for malloc-with-name (0x4D83B0), the game allocator and realloc (0x4D84A0).
    const int size_at[3] = {8, 4, 12};
    const u32 targets[3] = {0x004D83B0, kAlloc, 0x004D84A0};
    for (int k = 0; k < 3; ++k) {
        const u32 at = here();
        e.u8v(0xA1); e.u32le(L.bump_ptr);
        e.bytes({0x8B, 0x4C, 0x24, static_cast<std::uint8_t>(size_at[k])});
        e.bytes({0x83, 0xC1, 0x03, 0x83, 0xE1, 0xFC});
        e.u8v(0x01); e.u8v(0x0D); e.u32le(L.bump_ptr);
        e.u8v(0xC3);
        e.align();
        if (!r.patch_jump(targets[k], at)) return false;
    }
    for (const Stub& s : kStubs) {
        const u32 at = here();
        emit_stub(e, s, L.log_ptr);
        e.align();
        if (!r.patch_jump(s.address, at)) return false;
    }
    {  // real-time clock: returns the counter, then advances it by 7
        const u32 at = here();
        e.u8v(0xA1); e.u32le(L.clock);
        e.u8v(0x83); e.u8v(0x05); e.u32le(L.clock); e.u8v(0x07);
        e.u8v(0xC3);
        e.align();
        if (!r.patch_jump(kClock, at)) return false;
    }
    if (e.b.size() > 0x4000 || !r.write(L.stubs, e.b.data(), e.b.size())) return false;
    r.w32(L.bump_ptr, L.heap);
    r.w32(L.log_ptr, L.log_base);
    r.w32(kGameStatePtr, L.gs);
    r.w32(kDisplayPtr, L.display);
    r.w32(L.display + 0xE8, 30);  // tick rate the script objects read
    r.w32(L.gs + 0x14357, L.units);
    r.w32(L.gs + 0x14287, L.cells);
    r.w32(L.gs + 0x14233, static_cast<u32>(hm.width()));
    r.w32(L.gs + 0x14237, static_cast<u32>(hm.height()));
    r.w32(L.gs + 0x14377, L.models);
    r.w32(L.gs + 0x1439B, L.defs);  // the definition table; record 0 is the blank one
    r.w32(L.gs + 0x1429F, L.buckets);
    r.w32(L.gs + 0x142A3, static_cast<u32>(L.bw));
    r.w32(L.gs + 0x142A7, static_cast<u32>(L.bh));
    r.w32(L.gs + 0x142B7, L.off_map);
    r.w32(L.gs + 0x391ED, L.victory);
    L.scenario_heap = (r.r32(L.bump_ptr) + 0xFFF) & ~0xFFFu;
    return true;
}

// Remote offsets.
enum : u32 {
    uMotion = 0x00, uExtract = 0x58, uPitch = 0x64, uHeading = 0x66, uBank = 0x68, uPosX = 0x6A, uPosY = 0x6E, uPosZ = 0x72,
    uCellX = 0x76, uCellZ = 0x78, uLosX = 0x7A, uLosZ = 0x7C, uFootX = 0x7E, uFootZ = 0x80, uBucket = 0x82, uCarrier = 0x86,
    uCargo = 0x8A, uNext = 0x8E, uDef = 0x92, uOwner = 0x96, uScript = 0x9A, uState = 0x9E, uDefIndex = 0xA6, uId = 0xA8,
    uHover = 0xAA, uCloak = 0xB0, uKills = 0xB8, uNetDirty = 0xBA, uResource = 0xBC, uEnergyUse = 0xC0, uMetalMake = 0xD4,
    uMetalUse = 0xD8, uResourcePlayer = 0xEC, uLastAttacker = 0xF0, uLastSlot = 0xF4, uLastKind = 0xF5, uHealth = 0xF6,
    uPrevHealth = 0xF7, uLosFrame = 0xF8, uTransportPiece = 0xF9, uDamagedTimer = 0xFA, uHoldoff = 0xFB, uOwnerSlot = 0xFF,
    uUnbuilt = 0x100, uBuildLeft = 0x104, uHp = 0x108, uActivate = 0x10E, uScriptFlags = 0x10F, uFlags = 0x110, uDraw = 0x114,
    pActive = 0x00, pNetId = 0x04, pFirst = 0x67, pLast = 0x6B, pController = 0x73, pCreated = 0x140, pAlive = 0x144,
    pState = 0x146,
    dName = 0x20, dFootprint = 0x14A, dYard = 0x14E, dLimit = 0x15A, dModelMaxY = 0x16E, dCob = 0x18E, dMaxVelocity = 0x192,
    dExtract = 0x1CE, dWeapons = 0x1EE, dMaxDamage = 0x1FA, dBuildAngle = 0x210, dWaterline = 0x22C, dPages = 0x22E,
    dMovement = 0x22F, dTypeFlags = 0x241,
    wReload = 0xE4, wIndex = 0x10A,
    gLocalView = 0x2A43, gSeaLevel = 0x1427F, gTick = 0x38A47,
};

struct LoadedType {
    const ta::UnitDefRecord* def = nullptr;
    ta::UnitTypeAssets assets;
};

u32 fbits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

// Engine side of everything the stubs stand for.
struct Outside : sim::QuietBattleOutside {
    std::vector<u32> log;
    std::vector<sim::PlayerSlot>* players = nullptr;
    std::vector<sim::UnitTickState>* ticks = nullptr;
    std::map<u16, u32> motion_stamp;  // footprint step written into each unit's motion object
    u32 game_tick = 0, clock = 0;
    static u32 tok(const sim::Unit& u) { return 0x40000000u | u.id; }
    void rec(int id, std::initializer_list<u32> args) {
        log.push_back(static_cast<u32>(id));
        log.push_back(0);
        for (u32 a : args) log.push_back(a);
    }
    void join_group(sim::Unit& u, i32 group) override { rec(4, {tok(u), static_cast<u32>(group)}); }
    void stamp_line_of_sight(sim::Unit& u) override { rec(2, {tok(u)}); }
    void victory_unit_created(sim::Unit& u) override { rec(3, {tok(u)}); }
    std::int32_t run_mission(sim::Unit&, sim::MissionOrder&, std::uint32_t&) override { return 2; }
    void attach_unit(sim::Unit& u, i32 a, i32 b, i32 c) override { rec(12, {static_cast<u32>(a), static_cast<u32>(b), static_cast<u32>(c)}), (void)u; }
    void drop_unit(sim::Unit&, i32 a) override { rec(13, {static_cast<u32>(a)}); }
    void detach_from_carrier(sim::Unit&) override {}
    void release_unit(sim::Unit&) override {}
    bool pay_energy(sim::Unit&, float) override { return true; }
    void damage_feature(sim::MapCell&, i32, i32, const sim::WeaponType&) override {}
    sim::Vec3i feature_anim_pos(std::uint16_t) override { return {}; }
    void spawn_wreck(sim::Unit&, i32, bool) override {}
    void explode_piece(sim::Unit&, i32 piece, i32 type) override { rec(11, {static_cast<u32>(piece), static_cast<u32>(type)}); }
    void emit_sfx(sim::Unit&, i32 piece, i32 type) override { rec(10, {static_cast<u32>(piece), static_cast<u32>(type)}); }
    void reveal_dead_unit(sim::Unit&) override {}
    void forget_fog(sim::Unit&) override {}
    void victory_unit_destroyed(sim::Unit&) override {}
    void player_out_of_units(int) override {}
    void commander_died(int) override {}
    std::uint32_t clock_ticks() override {
        const u32 v = clock;
        clock += 7;
        return v;
    }
    void network_send(i32 net, const std::uint8_t* p, i32 size) override {
        u32 d[6] = {};
        std::memcpy(d, p, static_cast<std::size_t>((std::min)(size, 24)));
        rec(16, {static_cast<u32>(net), 0, static_cast<u32>(size), d[0], d[1], d[2], d[3], d[4], d[5]});
    }
    void broadcast_player_units(int) override {}
    void broadcast_script_call(sim::Unit& u, std::int16_t fn) override { rec(17, {tok(u), static_cast<u32>(static_cast<i32>(fn))}); }
    void on_activate_flags_changed(sim::Unit& u, std::uint8_t gained, std::uint8_t lost) override {
        if (gained & sim::unit_flags::kActivated) rec(14, {tok(u), 3, 0});
        if (lost & sim::unit_flags::kActivated) rec(14, {tok(u), 4, 0});
        if (gained & sim::unit_flags::kCloaked) rec(14, {tok(u), 0xE, 0});
        if (lost & sim::unit_flags::kCloaked) rec(14, {tok(u), 0xF, 0});
        rec(15, {tok(u)});
        const auto& owner = (*players)[static_cast<std::size_t>((*ticks)[u.id].owner)];
        if (owner.active && (owner.controller == 1 || owner.controller == 2)) rec(16, {0, 0, 4, 0, 0, 0, 0, 0, 0});
    }
    void forget_speech(sim::Unit&) override {}
    void leave_group(sim::Unit&) override {}
    void stats_activity(i32) override {}
    void kill_counted(std::uint8_t, sim::Unit&) override {}
    void stockpile_used(sim::Unit&) override {}
    void camera_follow_step() override {}
    void play_sound(std::uint16_t, const sim::Vec3i&) override {}
    void spawn_smoke(const sim::Vec3i&) override {}
    void spawn_muzzle_smoke(const sim::Vec3i&) override {}
    void spawn_explosion(const sim::Vec3i&, const sim::WeaponType&, bool, bool) override {}
    void camera_shake(i32, i32, i32) override {}
};

struct Stats {
    std::size_t scenarios = 0, ops = 0, created = 0, refused = 0, structures = 0, mobiles = 0, scripted = 0, activated = 0,
                contests = 0, mismatches = 0, failures = 0, faults = 0, predicted = 0, undefined = 0;
    std::map<std::string, std::size_t> by_op;
};

bool run_scenario(harness::Remote& r, const Layout& L, std::vector<LoadedType>& types, const ta::WeaponDefTable& wdefs,
                  sim::HeightMap& hm, int ops_to_run, unsigned seed, Stats& st, bool& predicted) {
    std::mt19937 g(seed);
    // Fresh heap contents (script statics come from it uninitialised in the original).
    {
        const u32 used = r.r32(L.bump_ptr);
        if (used > L.scenario_heap) {
            std::vector<std::uint8_t> zero(used - L.scenario_heap, 0);
            r.write(L.scenario_heap, zero.data(), zero.size());
        }
        r.w32(L.bump_ptr, L.scenario_heap);
    }
    std::vector<std::uint8_t> zero(0x40000, 0);
    r.write(L.gs + 0x1B63, zero.data(), 11 * kPlayerSize);
    r.write(L.units, zero.data(), (kTable + 1) * kUnitSize);
    r.write(L.buckets, zero.data(), static_cast<std::size_t>(L.bw) * L.bh * 10);
    r.write(L.off_map, zero.data(), 16);
    r.w32(L.log_ptr, L.log_base);

    Outside out;
    sim::BattleRng rng;
    sim::Battle b(out, hm, rng);
    b.init_unit_table(kTable);
    out.players = &b.players;
    out.ticks = &b.ticks;
    const i32 W = hm.width(), H = hm.height();

    // Weapons: the real table, some indexes and reload times varied.
    std::vector<sim::WeaponType> weapons = ta::to_weapon_types(wdefs);
    {
        std::vector<std::uint8_t> wb(256 * kWeaponSize, 0);
        for (int s = 0; s < 256; ++s) {
            sim::WeaponType& w = weapons[static_cast<std::size_t>(s)];
            if (g() % 8 == 0) w.index = static_cast<u8>(g() % 3 == 0 ? 0 : g());
            if (g() % 8 == 0) w.reload_time = static_cast<u16>(g() % 3 == 0 ? g() : g() % 400);
            std::memcpy(&wb[static_cast<std::size_t>(s) * kWeaponSize + wReload], &w.reload_time, 2);
            wb[static_cast<std::size_t>(s) * kWeaponSize + wIndex] = w.index;
        }
        r.write(L.weapons, wb.data(), wb.size());
    }
    auto weapon_addr = [&](const sim::WeaponType* w) -> u32 {
        return w ? L.weapons + static_cast<u32>(w - weapons.data()) * kWeaponSize : 0u;
    };

    // Unit types.
    b.types.resize(kTypes + 1);
    std::vector<int> pick;
    u32 file_at = L.files, yard_at = L.yards;
    std::vector<std::uint8_t> empty_def(kDefSize, 0);
    r.write(L.defs, empty_def.data(), empty_def.size());
    for (int k = 1; k <= kTypes; ++k) {
        std::size_t ti = g() % types.size();
        while (std::find(pick.begin(), pick.end(), static_cast<int>(ti)) != pick.end()) ti = g() % types.size();
        pick.push_back(static_cast<int>(ti));
        LoadedType& t = types[ti];
        sim::BattleUnitType& bt = b.types[static_cast<std::size_t>(k)];
        ta::to_battle_unit_type(*t.def, t.assets, weapons, bt);
        sim::UnitCreateType& c = bt.create;
        // Random variations of what creation reads.
        if (g() % 3 == 0) c.max_per_player = g() % 4 == 0 ? -1 : static_cast<i32>(g() % 4);
        if (g() % 4 == 0) c.movement = static_cast<u8>(g() % 5 == 0 ? 2 : g() % 2);
        if (g() % 3 == 0) c.build_menu_pages = static_cast<u8>(g() % 4);
        if (g() % 3 == 0) c.build_angle = static_cast<u16>(g() % 3 == 0 ? g() : g() % 0x4000);
        if (g() % 3 == 0) c.extracts_metal = g() % 4 == 0 ? -0.5f : static_cast<float>(g() % 1000) / 997.0f;
        if (g() % 4 == 0) {
            c.footprint_x = static_cast<i16>(1 + g() % 6);
            c.footprint_z = static_cast<i16>(1 + g() % 6);
        }
        const u32 flip[] = {1u << 18, 1u << 7, 1u << 9, 1u << 16, 1u << 4, 3u, 0xCu, 1u << 24, 1u << 23};
        for (int n = static_cast<int>(g() % 3); n > 0; --n) {
            const u32 bit = flip[g() % 9];
            if (bit == (1u << 24) && g() % 3) continue;  // immediate death: sparingly
            c.type_flags ^= bit;
        }
        if (g() % 12 == 0) c.type_flags &= ~sim::create_flags::kLoadable;
        else c.type_flags |= sim::create_flags::kLoadable;
        const std::size_t cells = static_cast<std::size_t>((std::max)(0, c.footprint_x * c.footprint_z));
        if (bt.info.yard_map.size() != cells || g() % 3 == 0) {
            bt.info.yard_map.resize(cells);
            const u8 pool[] = {0x00, 0x35, 0x8F, 0x2B, 0x31, 0x2D, 0x6F, 0x2F, 0x37, 0x29, 0x01, 0x02, 0x04, 0x07};
            for (auto& y : bt.info.yard_map) y = pool[g() % sizeof pool];
        }
        if (g() % 4 == 0) {  // ground settle variants
            const u32 variants[] = {0, 0x1000, 0x80000, 0x100000};
            bt.settle.type_flags = (bt.settle.type_flags & ~0x181000u) | variants[g() % 4];
            bt.tick.type_flags = bt.settle.type_flags;
        }
        if (bt.settle.max_velocity >= -1 && bt.settle.max_velocity <= 1) bt.settle.max_velocity = 0x20000;  // the original divides by it
        for (auto& w : c.weapons)
            if (g() % 6 == 0) w = &weapons[g() % 256];
        if (g() % 3 == 0) bt.info.max_damage = static_cast<u32>(g() % 3 == 0 ? g() : g() % 3000);
        // Settle and creation share the type flags of the definition in the original.
        bt.settle.type_flags = (c.type_flags & ~0x181000u) | (bt.settle.type_flags & 0x181000u);
        c.type_flags = bt.settle.type_flags;
        bt.tick.type_flags = c.type_flags;
        if (c.script) ++st.scripted;

        // Remote: model, script and definition.
        const u32 remote_model = file_at;
        file_at += (static_cast<u32>(t.assets.model_file.size()) + 15) & ~15u;
        r.write(remote_model, t.assets.model_file.data(), t.assets.model_file.size());
        if (!r.call(kRelocate, 0, {remote_model, remote_model}) || !r.call(kNegateXZ, remote_model, {remote_model})) return false;
        u32 remote_cob = 0;
        if (c.script) {
            remote_cob = file_at;
            file_at += (static_cast<u32>(t.assets.cob_file.size()) + 15) & ~15u;
            std::vector<std::uint8_t> img = t.assets.cob_file;
            auto rel = [&](std::size_t off) {
                u32 v;
                std::memcpy(&v, &img[off], 4);
                v += remote_cob;
                std::memcpy(&img[off], &v, 4);
            };
            auto hdr = [&](int i) {
                u32 v;
                std::memcpy(&v, &img[static_cast<std::size_t>(i) * 4], 4);
                return v;
            };
            for (u32 i = 0; i < hdr(1); ++i) rel(hdr(7) + i * 4);
            for (u32 i = 0; i < hdr(2); ++i) rel(hdr(8) + i * 4);
            for (u32 i = 0; i < hdr(5); ++i) rel(hdr(10) + i * 8 + 4);
            for (int i : {6, 7, 8, 9, 10}) rel(static_cast<std::size_t>(i) * 4);
            r.write(remote_cob, img.data(), img.size());
        }
        const u32 yard = yard_at;
        if (!bt.info.yard_map.empty()) r.write(yard, bt.info.yard_map.data(), bt.info.yard_map.size());
        yard_at += static_cast<u32>((bt.info.yard_map.size() + 16) & ~15u);
        std::vector<std::uint8_t> d(kDefSize, 0);
        auto put = [&](u32 off, const void* p, std::size_t n) { std::memcpy(&d[off], p, n); };
        put(dName, bt.tick.name.c_str(), (std::min)(bt.tick.name.size() + 1, std::size_t{32}));
        put(dFootprint, &c.footprint_x, 2);
        put(dFootprint + 2, &c.footprint_z, 2);
        put(dYard, &yard, 4);
        put(dLimit, &c.max_per_player, 4);
        put(dModelMaxY, &bt.info.model_max_y, 4);
        put(dCob, &remote_cob, 4);
        put(dMaxVelocity, &bt.settle.max_velocity, 4);
        put(dExtract, &c.extracts_metal, 4);
        for (int s = 0; s < 3; ++s) {
            const u32 wa = weapon_addr(c.weapons[static_cast<std::size_t>(s)]);
            put(dWeapons + 4u * static_cast<u32>(s), &wa, 4);
        }
        put(dMaxDamage, &bt.info.max_damage, 4);
        put(dBuildAngle, &c.build_angle, 2);
        put(dWaterline, &bt.settle.waterline, 1);
        put(dPages, &c.build_menu_pages, 1);
        put(dMovement, &c.movement, 1);
        put(dTypeFlags, &c.type_flags, 4);
        r.write(L.defs + static_cast<u32>(k) * kDefSize, d.data(), d.size());
        r.w32(L.models + static_cast<u32>(k) * 4, remote_model);
    }
    auto def_index = [&](u32 addr) -> int {
        if (addr >= L.defs && addr < L.defs + kDefSize * (kTypes + 1) && (addr - L.defs) % kDefSize == 0)
            return static_cast<int>((addr - L.defs) / kDefSize);
        return -1;
    };
    auto engine_def_index = [&](const sim::Unit& u) -> int {
        if (!u.type) return 0;
        for (int k = 1; k <= kTypes; ++k)
            if (u.type == &b.types[static_cast<std::size_t>(k)].info) return k;
        return u.type == &b.types[0].info ? 0 : -1;
    };

    // Players: ranges 1..8, 9..16, 17..24; slot 3 has an empty range; others none.
    const int ranges[4][2] = {{1, 8}, {9, 16}, {17, 24}, {25, 24}};
    for (int p = 0; p < 10; ++p) {
        sim::PlayerSlot& s = b.players[static_cast<std::size_t>(p)];
        s.active = p < 3 || g() % 3 == 0;
        s.controller = static_cast<u8>(p < 3 ? (g() % 3 == 0 ? 3 : 1 + g() % 2) : g() % 4);
        s.state = static_cast<u8>(g() % 4);
        s.net_id = static_cast<i32>(g() % 6) - 1;
        s.alive_count = 0;
        s.units_created = static_cast<u32>(g() % 5);
        const int first = p < 4 ? ranges[p][0] : 1, last = p < 4 ? ranges[p][1] : 0;
        b.assign_unit_range(p, first, last);
        const u32 at = L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize;
        r.w32(at + pActive, s.active ? 1u : 0u);
        r.w32(at + pNetId, static_cast<u32>(s.net_id));
        r.write(at + pController, &s.controller, 1);
        r.write(at + pState, &s.state, 1);
        r.w32(at + pCreated, s.units_created);
        r.w32(at + pFirst, L.units + static_cast<u32>(first) * kUnitSize);
        r.w32(at + pLast, L.units + static_cast<u32>(last) * kUnitSize);
    }
    // Table entries: id, owner record and slot are fixed.
    for (int i = 0; i < kTable; ++i) {
        sim::UnitTickState& t = b.ticks[static_cast<std::size_t>(i)];
        if (i == 0 || i > 24) t.owner = 0;
        const u32 at = L.units + static_cast<u32>(i) * kUnitSize;
        const u16 id = static_cast<u16>(i);
        const u8 slot = static_cast<u8>(t.owner);
        r.write(at + uId, &id, 2);
        r.w32(at + uOwner, L.gs + 0x1B63 + static_cast<u32>(t.owner) * kPlayerSize);
        r.write(at + uOwnerSlot, &slot, 1);
        r.w32(at + uDef, L.defs);
        b.units[static_cast<std::size_t>(i)].type = &b.types[0].info;
        for (auto& ws : b.units[static_cast<std::size_t>(i)].weapons) ws.target_z = 0;  // the original's table starts zeroed
        t.last_attacker_slot = 0;
    }

    // Map cells: real heights, random metal, height ranges, a few stale marks.
    b.map.width = W;
    b.map.height = H;
    b.map.cells.assign(static_cast<std::size_t>(W) * H, sim::MapCell{});
    for (i32 z = 0; z < H; ++z)
        for (i32 x = 0; x < W; ++x) {
            sim::MapCell& c = b.map.cells[static_cast<std::size_t>(z * W + x)];
            c.height = hm.at(x, z);
            c.metal = static_cast<u8>(g() % 3 == 0 ? 0 : g() % 4 == 0 ? g() : g() % 40);
            c.flags = static_cast<u8>(g() % 50 == 0 ? 2 : 0);
        }
    sim::refresh_height_range(b.map, 0, 0, static_cast<i16>(W), static_cast<i16>(H));
    b.init_buckets();
    auto put_cell = [&](i32 index) {
        const sim::MapCell& c = b.map.cells[static_cast<std::size_t>(index)];
        std::uint8_t d[kCellSize] = {};
        std::memcpy(d, &c.unit, 2);
        std::memcpy(d + 2, &c.unit2, 2);
        d[4] = c.height;
        d[5] = c.height_max;
        d[6] = c.height_min;
        d[7] = c.metal;
        std::memcpy(d + 8, &c.feature, 2);
        std::memcpy(d + 10, &c.feature_aux, 2);
        d[12] = c.flags;
        r.write(L.cells + static_cast<u32>(index) * kCellSize, d, kCellSize);
    };
    {
        std::vector<std::uint8_t> cells(b.map.cells.size() * kCellSize, 0);
        for (std::size_t i = 0; i < b.map.cells.size(); ++i) {
            const sim::MapCell& c = b.map.cells[i];
            std::uint8_t* d = &cells[i * kCellSize];
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
        r.write(L.cells, cells.data(), cells.size());
    }

    // Environment.
    b.settings.local_view = static_cast<u8>(g() % 4);
    b.settings.sea_level = static_cast<u8>(g() % 3 == 0 ? g() : g() % 40);
    b.unit_env().game_tick = 1000 + g() % 5000;
    out.game_tick = b.unit_env().game_tick;
    r.write(L.gs + gLocalView, &b.settings.local_view, 1);
    r.write(L.gs + gSeaLevel, &b.settings.sea_level, 1);
    r.w32(L.gs + gTick, b.unit_env().game_tick);
    const u32 rng0 = static_cast<u32>(g()) | 1u;
    rng.set_state(rng0);
    r.w32(kBattleRngState, rng0);
    out.clock = static_cast<u32>(g());
    r.w32(L.clock, out.clock);
    ++st.scenarios;

    auto unit_addr = [&](int i) { return L.units + static_cast<u32>(i) * kUnitSize; };
    auto token = [&](u32 w) -> u32 {
        if (w >= L.units && w < L.units + kTable * kUnitSize) {
            const u32 off = (w - L.units) % kUnitSize, idx = (w - L.units) / kUnitSize;
            if (off == 0) return 0x40000000u | idx;
            if (off == uResource) return 0x41000000u | idx;
        }
        if (w >= L.gs + 0x1B63 && w < L.gs + 0x1B63 + 11 * kPlayerSize && (w - L.gs - 0x1B63) % kPlayerSize == 0)
            return 0x20000000u | ((w - L.gs - 0x1B63) / kPlayerSize);
        return w;
    };
    auto bucket_index = [&](u32 w) -> i32 {
        if (w == 0) return sim::kNoBucket;
        if (w == L.off_map) return sim::kOffMapBucket;
        if (w >= L.buckets && (w - L.buckets) % 10 == 0) return static_cast<i32>((w - L.buckets) / 10);
        return -100;
    };
    auto normalise = [&](const std::vector<u32>& in) {
        std::vector<u32> o;
        for (std::size_t i = 0; i < in.size();) {
            const std::size_t n = record_size(in[i]);
            if (!n || i + n > in.size()) {
                o.push_back(0xDEADBEEFu);
                break;
            }
            std::vector<u32> rec(in.begin() + static_cast<std::ptrdiff_t>(i), in.begin() + static_cast<std::ptrdiff_t>(i + n));
            i += n;
            rec[1] = 0;
            // The engine builds a real motion object where the original's constructor is a stub here.
            if (rec[0] == 1) continue;
            // Passability updates (5, 6) are engine work now, verified by route_harness.
            if (rec[0] == 5 || rec[0] == 6) continue;
            for (std::size_t k = 2; k < rec.size(); ++k) rec[k] = token(rec[k]);
            if (rec[0] >= 10 && rec[0] <= 13) {  // script requests: the unit is in ECX, not compared
            }
            if (rec[0] == 16) {
                rec[3] = 0;  // packet address
                const u32 size = rec[4];
                if (size == 4) rec[2] = 0;
                std::uint8_t bytes[24];
                std::memcpy(bytes, &rec[5], 24);
                for (u32 k = size < 24 ? size : 24; k < 24; ++k) bytes[k] = 0;
                if (size == 4) std::memset(bytes, 0, 24);  // activation packet: compared by size
                std::memcpy(&rec[5], bytes, 24);
            }
            o.insert(o.end(), rec.begin(), rec.end());
        }
        return o;
    };

    std::string failure;
    auto compare = [&](const std::string& what) -> bool {
        bool ok = true;
        auto bad = [&](const std::string& m) {
            if (ok) std::printf("scenario %u %s: %s\n", seed, what.c_str(), m.c_str());
            ok = false;
        };
        const u32 end = r.r32(L.log_ptr);
        std::vector<u32> log((end - L.log_base) / 4);
        if (!log.empty()) r.read(L.log_base, log.data(), log.size() * 4);
        r.w32(L.log_ptr, L.log_base);
        const std::vector<u32> theirs = normalise(log), mine = normalise(out.log);
        out.log.clear();
        if (theirs != mine) {
            bad("subsystem call log differs");
            std::size_t first = 0;
            while (first < theirs.size() && first < mine.size() && theirs[first] == mine[first]) ++first;
            const std::size_t from = first > 12 ? first - 12 : 0;
            for (std::size_t i = from; i < first + 12 && i < (std::max)(theirs.size(), mine.size()); ++i) {
                const long long a = i < theirs.size() ? static_cast<long long>(theirs[i]) : -1;
                const long long c = i < mine.size() ? static_cast<long long>(mine[i]) : -1;
                std::printf("    log[%zu] orig %llx ours %llx%s\n", i, a, c, a != c ? "   <--" : "");
            }
        }
        if (r.r32(kBattleRngState) != rng.state()) bad("battle RNG state");
        if (r.r32(L.clock) != out.clock) bad("clock reads");
        for (int p = 0; p < 10; ++p) {
            const sim::PlayerSlot& s = b.players[static_cast<std::size_t>(p)];
            const u32 at = L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize;
            if (r.rd<u16>(at + pAlive) != s.alive_count || r.r32(at + pCreated) != s.units_created) bad("player " + std::to_string(p) + " counters");
        }
        // Map cells.
        {
            std::vector<std::uint8_t> rc(b.map.cells.size() * kCellSize);
            r.read(L.cells, rc.data(), rc.size());
            for (std::size_t i = 0; i < b.map.cells.size(); ++i) {
                const sim::MapCell& c = b.map.cells[i];
                const std::uint8_t* d = &rc[i * kCellSize];
                u16 u1, u2;
                std::memcpy(&u1, d, 2);
                std::memcpy(&u2, d + 2, 2);
                if (u1 != c.unit || u2 != c.unit2 || d[4] != c.height || d[5] != c.height_max || d[6] != c.height_min || d[7] != c.metal ||
                    d[12] != c.flags) {
                    bad("map cell " + std::to_string(i % static_cast<std::size_t>(W)) + "," + std::to_string(i / static_cast<std::size_t>(W)) +
                        " orig unit " + std::to_string(u1) + "/" + std::to_string(u2) + " flags " + std::to_string(d[12]) + " max/min " +
                        std::to_string(d[5]) + "/" + std::to_string(d[6]) + " ours " + std::to_string(c.unit) + "/" + std::to_string(c.unit2) +
                        " flags " + std::to_string(c.flags) + " max/min " + std::to_string(c.height_max) + "/" + std::to_string(c.height_min));
                    break;
                }
            }
        }
        // Buckets.
        {
            std::vector<std::uint8_t> rb(static_cast<std::size_t>(L.bw) * L.bh * 10);
            r.read(L.buckets, rb.data(), rb.size());
            for (std::size_t k = 0; k < b.buckets.heads.size(); ++k) {
                u32 head;
                std::memcpy(&head, &rb[k * 10 + 6], 4);
                const u32 mine_head = b.buckets.heads[k] ? 0x40000000u | b.buckets.heads[k]->id : 0u;
                if (token(head) != mine_head) {
                    bad("bucket " + std::to_string(k) + " head");
                    break;
                }
            }
            const u32 off = r.r32(L.off_map + 6);
            if (token(off) != (b.buckets.off_map ? 0x40000000u | b.buckets.off_map->id : 0u)) bad("off-map bucket head");
        }
        // Units.
        for (int i = 1; i < kTable; ++i) {
            sim::Unit& u = b.units[static_cast<std::size_t>(i)];
            const sim::UnitTickState& t = b.ticks[static_cast<std::size_t>(i)];
            std::vector<std::uint8_t> ru(kUnitSize);
            r.read(unit_addr(i), ru.data(), ru.size());
            auto f8 = [&](u32 off) { return ru[off]; };
            auto f16 = [&](u32 off) {
                u16 v;
                std::memcpy(&v, &ru[off], 2);
                return v;
            };
            auto f32 = [&](u32 off) {
                u32 v;
                std::memcpy(&v, &ru[off], 4);
                return v;
            };
            const std::string un = "unit " + std::to_string(i) + " ";
            if ((f32(uMotion) != 0) != t.has_motion) bad(un + "motion presence");
            for (int s = 0; s < 3; ++s) {
                const sim::WeaponSlot& ws = u.weapons[static_cast<std::size_t>(s)];
                const u32 so = 4 + static_cast<u32>(s) * 0x1C;
                if (static_cast<i16>(f16(so)) != ws.target_x || static_cast<i16>(f16(so + 2)) != ws.target_z || f32(so + 8) != static_cast<u32>(ws.aim.result) ||
                    f32(so + 0xC) != weapon_addr(ws.type) || static_cast<i32>(f32(so + 0x10)) != ws.aux || f16(so + 0x14) != ws.reload ||
                    f16(so + 0x16) != ws.aim_heading || f16(so + 0x18) != ws.aim_pitch || f8(so + 0x1A) != ws.stockpile || f8(so + 0x1B) != ws.flags)
                    bad(un + "weapon slot " + std::to_string(s) + " (aux orig " + std::to_string(static_cast<i32>(f32(so + 0x10))) + " ours " +
                        std::to_string(ws.aux) + ", flags orig " + std::to_string(f8(so + 0x1B)) + " ours " + std::to_string(ws.flags) + ")");
            }
            if (f32(uExtract) != fbits(u.metal_extraction)) bad(un + "metal extraction");
            if (static_cast<i16>(f16(uPitch)) != u.pitch || f16(uHeading) != u.heading || static_cast<i16>(f16(uBank)) != u.bank)
                bad(un + "pitch/heading/bank orig " + std::to_string(f16(uHeading)) + " ours " + std::to_string(u.heading));
            if (static_cast<i32>(f32(uPosX)) != u.pos[0] || static_cast<i32>(f32(uPosY)) != u.pos[1] || static_cast<i32>(f32(uPosZ)) != u.pos[2])
                bad(un + "position");
            if (static_cast<i16>(f16(uCellX)) != u.cell_x || static_cast<i16>(f16(uCellZ)) != u.cell_z || static_cast<i16>(f16(uFootX)) != u.footprint_x ||
                static_cast<i16>(f16(uFootZ)) != u.footprint_z)
                bad(un + "cell origin / footprint");
            if (static_cast<i16>(f16(uLosX)) != u.los_cell_x || static_cast<i16>(f16(uLosZ)) != u.los_cell_z || f8(uLosFrame) != u.los_frame)
                bad(un + "line-of-sight fields");
            if (bucket_index(f32(uBucket)) != u.bucket) bad(un + "bucket");
            if (token(f32(uNext)) != (u.cargo_next ? 0x40000000u | u.cargo_next->id : 0u)) bad(un + "bucket link");
            if (def_index(f32(uDef)) != engine_def_index(u)) bad(un + "type record");
            if (f16(uDefIndex) != t.type_index) bad(un + "type index");
            if (f16(uHover) != u.hover_phase) bad(un + "hover phase");
            if (f32(uCloak) != u.cloak_holdoff_tick) bad(un + "cloak hold-off");
            if (f16(uKills) != t.kill_count || f16(uNetDirty) != u.net_dirty) bad(un + "kills / net dirty");
            if (f32(uEnergyUse) != fbits(t.energy_use) || f32(uMetalMake) != fbits(t.metal_make) || f32(uMetalUse) != fbits(t.metal_use))
                bad(un + "resource slot");
            if (t.type_index && token(f32(uResourcePlayer)) != (0x20000000u | static_cast<u32>(t.owner))) bad(un + "resource slot player");
            if (token(f32(uLastAttacker)) != (t.last_attacker ? 0x40000000u | t.last_attacker->id : 0u) || f8(uLastSlot) != t.last_attacker_slot ||
                f8(uLastKind) != t.last_damage_kind)
                bad(un + "last attacker");
            if (f8(uHealth) != t.health_percent || f8(uPrevHealth) != t.prev_health_percent) bad(un + "health percent");
            if (f8(uTransportPiece) != u.transport_piece) bad(un + "transport piece");
            if (f8(uDamagedTimer) != t.damaged_timer || f32(uHoldoff) != t.transfer_holdoff) bad(un + "timers");
            if (f32(uUnbuilt) != u.unbuilt_word || f32(uBuildLeft) != fbits(u.build_fraction_left) || static_cast<i16>(f16(uHp)) != u.hit_points)
                bad(un + "build state / hit points");
            if (f8(uActivate) != u.activate_flags || f8(uScriptFlags) != u.script_flags) bad(un + "activate/script flags");
            if (f32(uFlags) != u.flags) {
                char m[96];
                std::snprintf(m, sizeof m, "flags orig %08x ours %08x", f32(uFlags), u.flags);
                bad(un + m);
            }
            if (f32(uDraw) != u.draw_flags) bad(un + "draw flags");
            if (t.has_motion && f32(uMotion)) {
                if (r.r32(f32(uMotion) + 0x26) != t.motion.claim_tick) bad(un + "motion footprint step");
            }
            if (t.type_index == 0) continue;
            const u32 so = f32(uScript);
            if ((so != 0) != (u.script != nullptr)) {
                bad(un + "script presence");
                continue;
            }
            if (u.script) {
                std::vector<u32> blk(sim::ScriptInstance::kThreads * sim::ScriptInstance::kThreadWords + 1);
                r.read(so + 0x1C, blk.data(), blk.size() * 4);
                const auto& mem = u.script->thread_memory();
                for (std::size_t w = 0; w < blk.size(); ++w)
                    if (blk[w] != mem[w]) {
                        bad(un + "script thread word " + std::to_string(w));
                        break;
                    }
                std::vector<i32> an(u.script->piece_anims().size());
                if (!an.empty()) r.read(r.r32(so + 0x14), an.data(), an.size() * 4);
                if (an != u.script->piece_anims()) bad(un + "script piece animations");
                std::vector<i32> sv(u.script->statics().size());
                if (!sv.empty()) r.read(r.r32(so + 0x10), sv.data(), sv.size() * 4);
                if (sv != u.script->statics()) bad(un + "script statics");
                if (!u.script->fault().empty()) bad(un + "script fault: " + u.script->fault());
            }
            // Pose: piece count, dirty flag, and each piece's move, turn and flags.
            const u32 state = f32(uState);
            if (state) {
                if (static_cast<i32>(r.r32(state)) != static_cast<i32>(u.pose.pieces.size()) || static_cast<i32>(r.r32(state + 8)) != u.pose.anim_dirty)
                    bad(un + "object state header");
                for (std::size_t pi = 0; pi < u.pose.pieces.size(); ++pi) {
                    std::uint8_t p[54];
                    r.read(state + 0x22 + static_cast<u32>(pi) * 54, p, 54);
                    i32 mv[3];
                    i16 tu[3];
                    u16 fl;
                    std::memcpy(mv, p + 4, 12);
                    std::memcpy(tu, p + 16, 6);
                    std::memcpy(&fl, p + 40, 2);
                    const sim::PosePiece& o = u.pose.pieces[pi];
                    if (mv[0] != o.move[0] || mv[1] != o.move[1] || mv[2] != o.move[2] || tu[0] != o.turn[0] || tu[1] != o.turn[1] || tu[2] != o.turn[2] ||
                        fl != o.flags) {
                        bad(un + "piece " + std::to_string(pi));
                        break;
                    }
                }
            }
        }
        if (!ok) ++st.mismatches;
        if (ok && !b.footprint_world().fault.empty()) {
            // The original went outside its tables without crashing (undefined behaviour); the scenario stops here.
            ++st.undefined;
            std::printf("scenario %u %s: stopped at undefined behaviour of the original (%s)\n", seed, what.c_str(),
                        b.footprint_world().fault.c_str());
            return false;
        }
        return ok;
    };
    // A failed call of the original ends the scenario (the process restarts). When the engine
    // recorded a fault there (an access outside the original's tables), the crash was predicted.
    auto fail = [&](const std::string& what) {
        if (!b.footprint_world().fault.empty()) {
            ++st.predicted;
            predicted = true;
            std::printf("scenario %u: %s crashed the original as predicted (%s)\n", seed, what.c_str(), b.footprint_world().fault.c_str());
        } else {
            std::printf("scenario %u: the original failed during %s\n", seed, what.c_str());
        }
        return false;
    };
    auto live = [&]() {
        std::vector<int> v;
        for (int i = 1; i < kTable; ++i)
            if (b.ticks[static_cast<std::size_t>(i)].type_index != 0) v.push_back(i);
        return v;
    };
    auto pack = [](i32 lo, i32 hi) { return static_cast<u32>(static_cast<u16>(lo)) | static_cast<u32>(static_cast<u16>(hi)) << 16; };
    auto write_unit_footprint_state = [&](int i) {
        const sim::Unit& u = b.units[static_cast<std::size_t>(i)];
        const u32 at = unit_addr(i);
        r.write(at + uPosX, &u.pos[0], 4);
        r.write(at + uPosY, &u.pos[1], 4);
        r.write(at + uPosZ, &u.pos[2], 4);
        r.write(at + uCellX, &u.cell_x, 2);
        r.write(at + uCellZ, &u.cell_z, 2);
        r.w32(at + uFlags, u.flags);
        r.write(at + uScriptFlags, &u.script_flags, 1);
    };
    // Most positions fall in a small area so that footprints meet.
    const i32 centre[2] = {static_cast<i32>(64 + g() % static_cast<u32>(W * 16 - 128)), static_cast<i32>(64 + g() % static_cast<u32>(H * 16 - 128))};
    int axis = 0;
    auto random_pos = [&](i32 size) -> i32 {
        const int m = static_cast<int>(g() % 12);
        const i32 c = centre[axis];
        axis ^= 1;
        if (m >= 4) return static_cast<i32>(static_cast<u32>(c - 48 + static_cast<i32>(g() % 96)) << 16 | (g() & 0xFFFF));
        if (m == 0) return static_cast<i32>(g());
        if (m == 1) return static_cast<i32>(static_cast<u32>(g() % 160) << 16) - (80 << 16);
        if (m == 2) return static_cast<i32>(static_cast<u32>(size - 80 + static_cast<i32>(g() % 160)) << 16);
        return static_cast<i32>((static_cast<u32>(g() % static_cast<u32>(size - 64)) + 32) << 16 | (g() & 0xFFFF));
    };
    // Random stale contents in a vacated entry (both sides).
    auto stale_fill = [&](int i) {
        sim::Unit& u = b.units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = b.ticks[static_cast<std::size_t>(i)];
        const u32 at = unit_addr(i);
        for (int s = 0; s < 3; ++s) {
            sim::WeaponSlot& ws = u.weapons[static_cast<std::size_t>(s)];
            ws.target_x = static_cast<i16>(g() % 30);
            ws.target_z = g() % 2 ? sim::kUnitTarget : static_cast<i16>(g() % 500);
            ws.aim.result = static_cast<i32>(g() % 2);
            ws.type = g() % 3 ? &weapons[g() % 256] : nullptr;
            ws.aux = static_cast<i32>(g());
            ws.reload = static_cast<u16>(g());
            ws.aim_heading = static_cast<u16>(g());
            ws.aim_pitch = static_cast<u16>(g());
            ws.stockpile = static_cast<u8>(g());
            ws.flags = static_cast<u8>(g());
            const u32 so = at + 4 + static_cast<u32>(s) * 0x1C;
            r.write(so, &ws.target_x, 2);
            r.write(so + 2, &ws.target_z, 2);
            r.w32(so + 8, static_cast<u32>(ws.aim.result));
            r.w32(so + 0xC, weapon_addr(ws.type));
            r.w32(so + 0x10, static_cast<u32>(ws.aux));
            r.write(so + 0x14, &ws.reload, 2);
            r.write(so + 0x16, &ws.aim_heading, 2);
            r.write(so + 0x18, &ws.aim_pitch, 2);
            r.write(so + 0x1A, &ws.stockpile, 1);
            r.write(so + 0x1B, &ws.flags, 1);
        }
        u.activate_flags = static_cast<u8>(g());
        u.script_flags = static_cast<u8>(g());
        u.flags = static_cast<u32>(g()) & ~(sim::unit_flags::kAlive | 0x30u);
        t.last_damage_kind = static_cast<u8>(g() % 12);
        u.net_dirty = static_cast<u16>(g());
        u.hit_points = static_cast<i16>(g());
        u.build_fraction_left = static_cast<float>(g() % 100) / 100.0f;
        u.unbuilt_word = static_cast<u32>(g());
        u.draw_flags = static_cast<u32>(g());
        u.transport_piece = static_cast<u8>(g());
        u.los_frame = static_cast<u8>(g());
        u.metal_extraction = static_cast<float>(g() % 50);
        t.kill_count = static_cast<u16>(g() % 40);
        r.write(at + uActivate, &u.activate_flags, 1);
        r.write(at + uScriptFlags, &u.script_flags, 1);
        r.w32(at + uFlags, u.flags);
        r.write(at + uLastKind, &t.last_damage_kind, 1);
        r.write(at + uNetDirty, &u.net_dirty, 2);
        r.write(at + uHp, &u.hit_points, 2);
        r.w32(at + uBuildLeft, fbits(u.build_fraction_left));
        r.w32(at + uUnbuilt, u.unbuilt_word);
        r.w32(at + uDraw, u.draw_flags);
        r.write(at + uTransportPiece, &u.transport_piece, 1);
        r.write(at + uLosFrame, &u.los_frame, 1);
        r.w32(at + uExtract, fbits(u.metal_extraction));
        r.write(at + uKills, &t.kill_count, 2);
    };
    for (int i = 1; i < kTable; ++i)
        if (g() % 3 == 0) stale_fill(i);
    if (!compare("set-up")) return true;

    for (int op = 0; op < ops_to_run; ++op) {
        b.unit_env().game_tick += g() % 3;
        out.game_tick = b.unit_env().game_tick;
        r.w32(L.gs + gTick, b.unit_env().game_tick);
        const std::vector<int> alive = live();
        const int kind = alive.empty() ? 0 : static_cast<int>(g() % 20);
        std::string what;
        if (kind < 8) {  // create
            const int player = static_cast<int>(g() % 10 == 0 ? 3 + g() % 3 : g() % 3);
            const u16 type = static_cast<u16>(g() % 15 == 0 ? 0 : 1 + g() % kTypes);
            const i32 x = random_pos(W * 16), z = random_pos(H * 16);
            const i32 y = static_cast<i32>(g() % 3 == 0 ? g() : (g() % 255) << 16);
            const bool built = g() % 3 != 0;
            const u32 layer = g() % 4 ? 1u : static_cast<u32>(g() % 4);
            const u16 wanted = static_cast<u16>(g() % 5 ? 0 : 1 + g() % (kTable - 1));
            sim::Unit* made = b.create_unit(player, type, x, y, z, built, layer, wanted);
            u32 res = 0;
            if (!r.call(kCreate, 0, {static_cast<u32>(player), type, static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(z), built ? 1u : 0u,
                                     layer, wanted},
                        &res))
                return fail("create");
            const u32 mine = made ? unit_addr(made->id) : 0u;
            if (res != mine) {
                std::printf("scenario %u create: original returned %08x, ours %08x\n", seed, res, mine);
                ++st.mismatches;
                return true;
            }
            if (made) {
                ++st.created;
                const sim::UnitCreateType& c = b.types[type].create;
                if (c.movement == 0) ++st.structures;
                if (c.movement == 1) ++st.mobiles;
                if (built && (c.type_flags & sim::create_flags::kActivateWhenBuilt)) ++st.activated;
                if (made->flags & (sim::footprint_flags::kWantsCells | sim::footprint_flags::kHoldsWanted)) ++st.contests;
            } else {
                ++st.refused;
            }
            what = made ? "create" : "create (refused)";
        } else if (kind < 10) {  // vacate an entry (the footprint and table part of a death), then stale contents
            const int i = alive[g() % alive.size()];
            sim::Unit& u = b.units[static_cast<std::size_t>(i)];
            sim::UnitTickState& t = b.ticks[static_cast<std::size_t>(i)];
            sim::clear_footprint(b.footprint_world(), u);
            if (!r.call(kClearFootprint, 0, {unit_addr(i)})) return fail("clear footprint");
            t.type_index = 0;
            t.has_motion = false;
            u.script.reset();
            u.type = &b.types[0].info;
            --b.players[static_cast<std::size_t>(t.owner)].alive_count;
            const u32 at = unit_addr(i);
            const u16 zero16 = 0;
            r.write(at + uDefIndex, &zero16, 2);
            r.w32(at + uMotion, 0);
            r.w32(at + uScript, 0);
            r.w32(at + uDef, L.defs);
            const u32 pa = L.gs + 0x1B63 + static_cast<u32>(t.owner) * kPlayerSize + pAlive;
            r.write(pa, &b.players[static_cast<std::size_t>(t.owner)].alive_count, 2);
            stale_fill(i);
            what = "vacate";
        } else if (kind < 12) {  // move: release, new position, claim
            const int i = alive[g() % alive.size()];
            sim::Unit& u = b.units[static_cast<std::size_t>(i)];
            sim::release_cells(b.footprint_world(), u);
            if (!r.call(kRelease, 0, {unit_addr(i)})) return fail("release");
            if (g() % 2) {
                u.pos[0] = g() % 4 ? u.pos[0] + static_cast<i32>(g() % 0x400000) - 0x200000 : random_pos(W * 16);
                u.pos[2] = g() % 4 ? u.pos[2] + static_cast<i32>(g() % 0x400000) - 0x200000 : random_pos(H * 16);
                sim::footprint_origin(u.pos[0], u.pos[2], u.footprint_x, u.footprint_z, u.cell_x, u.cell_z);
            }
            if (g() % 5 == 0) u.flags = (u.flags & ~3u) | static_cast<u32>(g() % 4);
            if (g() % 5 == 0) u.script_flags ^= sim::unit_flags::kYardOpen;
            write_unit_footprint_state(i);
            sim::claim_footprint(b.footprint_world(), u);
            if (!r.call(kClaim, 0, {unit_addr(i)})) return fail("claim");
            what = "move";
        } else if (kind == 12) {  // wanted-cell claim (rarely with the first cell off the map: the original crashes)
            int i = alive[g() % alive.size()];
            for (int tries = 0; tries < 8 && g() % 10; ++tries) {
                const sim::Unit& c = b.units[static_cast<std::size_t>(i)];
                if (c.cell_x >= 0 && c.cell_x < W && c.cell_z >= 0 && c.cell_z < H) break;
                i = alive[g() % alive.size()];
            }
            sim::Unit& u = b.units[static_cast<std::size_t>(i)];
            if (g() % 2) u.flags |= sim::footprint_flags::kWantsCells;
            if (g() % 4 == 0) u.script_flags ^= sim::unit_flags::kYardOpen;
            write_unit_footprint_state(i);
            sim::claim_wanted_cells(b.footprint_world(), u);
            if (!r.call(kClaimWanted, 0, {unit_addr(i)})) return fail("claim wanted cells");
            what = "claim wanted cells";
        } else if (kind == 13) {  // area re-claim, with random units wanting cells
            for (int i : alive)
                if (g() % 3 == 0) {
                    b.units[static_cast<std::size_t>(i)].flags |= sim::footprint_flags::kWantsCells;
                    r.w32(unit_addr(i) + uFlags, b.units[static_cast<std::size_t>(i)].flags);
                }
            const i16 x = static_cast<i16>(static_cast<i32>(g() % static_cast<u32>(W + 10)) - 5), z = static_cast<i16>(static_cast<i32>(g() % static_cast<u32>(H + 10)) - 5);
            const i16 w = static_cast<i16>(g() % 12), h = static_cast<i16>(g() % 12);
            sim::reclaim_area(b.footprint_world(), x, z, w, h);
            if (!r.call(kReclaimArea, 0, {pack(x, z), pack(w, h)})) return fail("reclaim area");
            what = "reclaim area";
        } else if (kind == 14) {  // force re-claim around a unit
            const int i = alive[g() % alive.size()];
            sim::force_reclaim_around(b.footprint_world(), b.units[static_cast<std::size_t>(i)]);
            if (!r.call(kForceReclaim, 0, {unit_addr(i)})) return fail("force reclaim");
            what = "force reclaim";
        } else if (kind == 15) {  // yard check, or yard opening through the script value
            const int i = alive[g() % alive.size()];
            sim::Unit& u = b.units[static_cast<std::size_t>(i)];
            const i32 value = g() % 4 == 0 ? static_cast<i32>(g()) : static_cast<i32>(g() % 3);
            const u32 so = r.r32(unit_addr(i) + uScript);
            if (so && u.script && g() % 2) {
                sim::unit_set_value(u, b.unit_world(), 18, value);
                if (!r.call(kSetValue, so, {18u, static_cast<u32>(value)})) return fail("set yard value");
                what = "yard open (script value)";
            } else {
                const bool mine = sim::yard_cells_free(b.footprint_world(), u, value);
                u32 res = 0;
                if (!r.call(kYardFree, 0, {unit_addr(i), static_cast<u32>(value)}, &res)) return fail("yard check");
                if ((res != 0) != mine) {
                    std::printf("scenario %u yard check unit %d: original %u ours %d\n", seed, i, res, mine ? 1 : 0);
                    ++st.mismatches;
                    return true;
                }
                what = "yard check";
            }
        } else if (kind == 16) {  // height ranges after terrain changes
            for (int n = static_cast<int>(g() % 20); n > 0; --n) {
                const i32 x = static_cast<i32>(g() % static_cast<u32>(W)), z = static_cast<i32>(g() % static_cast<u32>(H));
                const u8 h = static_cast<u8>(g());
                hm.at(x, z) = h;
                b.map.cells[static_cast<std::size_t>(z * W + x)].height = h;
                put_cell(z * W + x);
            }
            const i16 x = static_cast<i16>(static_cast<i32>(g() % static_cast<u32>(W + 10)) - 5), z = static_cast<i16>(static_cast<i32>(g() % static_cast<u32>(H + 10)) - 5);
            const i16 w = static_cast<i16>(g() % 30), h = static_cast<i16>(g() % 30);
            sim::refresh_height_range(b.map, x, z, w, h);
            if (!r.call(kHeights, 0, {pack(x, z), pack(w, h)})) return fail("height ranges");
            what = "height ranges";
        } else if (kind == 17) {  // metal extraction and weapon slots again
            const int i = alive[g() % alive.size()];
            sim::Unit& u = b.units[static_cast<std::size_t>(i)];
            const sim::UnitCreateType& c = b.types[b.ticks[static_cast<std::size_t>(i)].type_index].create;
            if (g() % 2) {
                sim::update_metal_extraction(u, b.map, c.extracts_metal);
                if (!r.call(kMetal, 0, {unit_addr(i)})) return fail("metal extraction");
                what = "metal extraction";
            } else {
                sim::setup_weapon_slots(u, c.weapons);
                if (!r.call(kWeaponSetup, 0, {unit_addr(i)})) return fail("weapon slots");
                what = "weapon slots";
            }
        } else {  // random cell occupants and marks
            for (int n = static_cast<int>(1 + g() % 12); n > 0; --n) {
                const i32 index = static_cast<i32>(g() % b.map.cells.size());
                sim::MapCell& c = b.map.cells[static_cast<std::size_t>(index)];
                if (g() % 2) c.unit = static_cast<u16>(g() % 3 == 0 ? 0 : 1 + g() % 24);
                if (g() % 2) c.unit2 = static_cast<u16>(g() % 3 == 0 ? 0 : 1 + g() % 24);
                if (g() % 4 == 0) c.flags ^= 2;
                put_cell(index);
            }
            what = "cell occupants";
        }
        ++st.ops;
        ++st.by_op[what];
        for (const auto& u : b.units)
            if (u.script && !u.script->fault().empty()) {
                ++st.faults;
                std::printf("scenario %u: stopped, engine script fault: %s\n", seed, u.script->fault().c_str());
                return true;
            }
        if (!compare(what + " (op " + std::to_string(op) + ")")) return true;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: create_harness <TotalA.exe> <game_dir> [scenarios] [operations]\n");
        return 2;
    }
    const int scenarios = argc > 3 ? std::atoi(argv[3]) : 20;
    const int ops = argc > 4 ? std::atoi(argv[4]) : 60;
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[2]);
    ta::UnitDefContext ctx;
    ta::load_unit_context(vfs, ctx);
    const ta::UnitDefTable defs = ta::load_unit_defs(vfs, ctx);
    ta::WeaponDefOptions wopt;
    const ta::WeaponDefTable wdefs = ta::load_weapon_defs(vfs, wopt);
    std::vector<std::uint8_t> tnt;
    sim::HeightMap hm;
    if (!vfs.read("maps\\Moon Quartet.tnt", tnt) || !ta::tnt_load_heights(tnt, hm)) {
        std::fprintf(stderr, "cannot load the Moon Quartet heights\n");
        return 1;
    }
    const sim::HeightMap pristine = hm;
    std::vector<LoadedType> types;
    types.reserve(defs.units.size());
    for (std::size_t i = 1; i < defs.units.size(); ++i) {
        LoadedType t;
        t.def = &defs.units[i];
        std::string err;
        if (!ta::load_unit_assets(vfs, *t.def, t.assets, err)) continue;
        types.push_back(std::move(t));
    }
    harness::Remote r;
    Layout L;
    if (!r.start(argv[1]) || !setup(r, L, hm)) {
        std::fprintf(stderr, "cannot start the original\n");
        return 1;
    }
    Stats st;
    for (int s = 0; s < scenarios; ++s) {
        hm = pristine;
        bool predicted = false;
        if (!run_scenario(r, L, types, wdefs, hm, ops, static_cast<unsigned>(s * 13 + 5), st, predicted)) {
            if (!predicted) ++st.failures;
            std::printf("scenario %d: restarting the original\n", s);
            r.stop();
            if (!r.start(argv[1]) || !setup(r, L, hm)) return 1;
        }
    }
    std::printf("unit types %zu, map %dx%d cells\n", types.size(), hm.width(), hm.height());
    std::printf("scenarios %zu, operations %zu, units created %zu (structures %zu, mobile %zu, activated %zu, contested %zu), refused %zu, "
                "scripted types %zu\n",
                st.scenarios, st.ops, st.created, st.structures, st.mobiles, st.activated, st.contests, st.refused, st.scripted);
    std::printf("operations:\n");
    for (const auto& [k, v] : st.by_op) std::printf("  %s: %zu\n", k.c_str(), v);
    std::printf("stopped at undefined behaviour of the original %zu\n", st.undefined);
    std::printf("mismatching scenarios %zu, engine faults %zu, predicted original crashes %zu, original failures %zu\n", st.mismatches,
                st.faults, st.predicted, st.failures);
    return st.mismatches || st.failures ? 1 : 0;
}
