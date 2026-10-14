// Differential tester for the per-step unit pass (sim::run_unit_pass) with real
// unit scripts. Inside a suspended TotalA.exe it builds a small battle: three
// players, units of random real types (their models, scripts, object states
// and the original unit script callback table), a fake game state, and logging
// stubs in place of every subsystem the engine does not model yet (weapons,
// orders, motion, damage, healing, speech, network...). It then runs the
// original unit pass and the engine's tick by tick, with random environment
// changes and random script calls, comparing after every tick: the stub call
// log, every unit field the pass touches, each unit's script threads,
// animations and statics, piece poses, the battle RNG and the live count.
//
// Usage: unitloop_harness <TotalA.exe> <game_dir> [scenarios] [ticks]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/pose.h"
#include "sim/rng.h"
#include "sim/script_vm.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"
#include "ta/cob.h"
#include "ta/hpi.h"
#include "ta/model3do.h"
#include "ta/tnt.h"
#include "ta/unitdefs.h"

namespace {

using harness::u32;
using i32 = std::int32_t;

constexpr u32 kGameStatePtr = 0x00511DE8, kBattleRngState = 0x0051FC88;
constexpr u32 kRelocate = 0x004CB4C0, kNegateXZ = 0x004CB590, kBuildState = 0x0045A950, kBindState = 0x00480D40;
constexpr u32 kCallNamed = 0x004B0940, kScriptCall = 0x004B0A70, kUnitPass = 0x0048AD30;
constexpr u32 kUnitVtable = 0x004FD698;
constexpr u32 kUnitSize = 0x118, kPlayerSize = 0x14B, kScriptSize = 0x544;
constexpr int kMaxUnits = 16;

// Subsystems replaced by logging stubs: address, argument count, log id.
struct Stub {
    u32 address;
    int args;
    int id;
};
const Stub kStubs[] = {
    {0x0049E1A0, 1, 1},   // weapons(unit)
    {0x00489BB0, 5, 2},   // damage(attacker, target, amount, kind, heading)
    {0x0041BD10, 3, 3},   // repair_heal(worker, target, float rate)
    {0x0043B7C0, 1, 4},   // primary orders(unit)
    {0x0043BAD0, 1, 5},   // secondary orders(unit)
    {0x0043DD20, 1, 6},   // motion(unit) [thiscall on the motion object]
    {0x0048A870, 1, 7},   // settle height(unit)
    {0x004864B0, 2, 8},   // killed(unit, kind)
    {0x0048B710, 1, 9},   // broadcast player units(player)
    {0x00480EB0, 2, 10},  // script emit sfx(piece, type) [thiscall]
    {0x00481140, 2, 11},  // script explode(piece, type)
    {0x00481340, 3, 12},  // script attach(unit, piece, mode)
    {0x004813B0, 1, 13},  // script drop(unit)
    {0x0047F780, 3, 14},  // speech(unit, event, 0)
    {0x0041C110, 1, 15},  // HUD dirty(unit)
    {0x00451DF0, 3, 16},  // network broadcast(player net id, packet, size)
    {0x0047DAC0, 2, 17},  // yard open(unit, open): stubbed, so it never succeeds
};

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
};

// Logs {id, ECX, args...} at *log_ptr, then returns with `ret 4*args`.
void emit_stub(Emitter& e, int id, int nargs, u32 log_ptr_addr) {
    e.u8(0xA1); e.u32le(log_ptr_addr);                         // mov eax,[log_ptr]
    e.u8(0xC7); e.u8(0x00); e.u32le(static_cast<u32>(id));     // mov dword [eax],id
    e.u8(0x89); e.u8(0x48); e.u8(0x04);                         // mov [eax+4],ecx
    for (int i = 0; i < nargs; ++i) {
        e.u8(0x8B); e.u8(0x54); e.u8(0x24); e.u8(static_cast<std::uint8_t>(4 + 4 * i));  // mov edx,[esp+4+4i]
        e.u8(0x89); e.u8(0x50); e.u8(static_cast<std::uint8_t>(8 + 4 * i));              // mov [eax+8+4i],edx
    }
    e.u8(0x83); e.u8(0x05); e.u32le(log_ptr_addr); e.u8(static_cast<std::uint8_t>(4 * (nargs + 2)));
    e.u8(0x33); e.u8(0xC0);                                     // xor eax,eax
    e.u8(0xC2); e.u8(static_cast<std::uint8_t>(4 * nargs)); e.u8(0x00);
}

struct Layout {
    u32 heap = 0, bump_ptr = 0, stubs = 0, log_ptr = 0, log_base = 0, gs = 0, units = 0, defs = 0, models = 0, cells = 0,
        schema = 0, names = 0, files = 0;
};

bool setup(harness::Remote& r, Layout& L, const sim::HeightMap& hm) {
    L.heap = r.alloc(128u << 20);
    L.bump_ptr = r.alloc(0x1000);
    L.stubs = r.alloc(0x4000, PAGE_EXECUTE_READWRITE);
    L.log_ptr = r.alloc(0x1000);
    L.log_base = r.alloc(4u << 20);
    L.gs = r.alloc(0x40000);
    L.units = r.alloc(static_cast<SIZE_T>(65536) * kUnitSize);
    L.defs = r.alloc(0x10000);
    L.models = r.alloc(0x1000);
    L.cells = r.alloc(static_cast<SIZE_T>(hm.width()) * hm.height() * 13);
    L.schema = r.alloc(0x1000);
    L.names = r.alloc(0x1000);
    L.files = r.alloc(32u << 20);
    if (!L.heap || !L.stubs || !L.log_base || !L.gs || !L.units || !L.cells || !L.files) return false;
    Emitter e;
    // Bump allocators: (tag, size), (size), and realloc (ptr, tag, size) for fresh blocks.
    const int size_at[3] = {8, 4, 12};
    const u32 targets[3] = {0x004D83B0, 0x004B4F10, 0x004D84A0};
    for (int k = 0; k < 3; ++k) {
        const u32 at = L.stubs + static_cast<u32>(e.b.size());
        e.u8(0xA1); e.u32le(L.bump_ptr);
        e.u8(0x8B); e.u8(0x4C); e.u8(0x24); e.u8(static_cast<std::uint8_t>(size_at[k]));
        e.u8(0x83); e.u8(0xC1); e.u8(0x03); e.u8(0x83); e.u8(0xE1); e.u8(0xFC);
        e.u8(0x01); e.u8(0x0D); e.u32le(L.bump_ptr);
        e.u8(0xC3);
        while (e.b.size() % 16) e.u8(0xCC);
        if (!r.patch_jump(targets[k], at)) return false;
    }
    for (const Stub& s : kStubs) {
        const u32 at = L.stubs + static_cast<u32>(e.b.size());
        emit_stub(e, s.id, s.args, L.log_ptr);
        while (e.b.size() % 16) e.u8(0xCC);
        if (!r.patch_jump(s.address, at)) return false;
    }
    if (!r.write(L.stubs, e.b.data(), e.b.size())) return false;
    r.w32(L.bump_ptr, L.heap);
    r.w32(L.log_ptr, L.log_base);
    r.w32(kGameStatePtr, L.gs);
    r.w32(L.gs + 0x14357, L.units);
    r.w32(L.gs + 0x14287, L.cells);
    r.w32(L.gs + 0x14233, static_cast<u32>(hm.width()));
    r.w32(L.gs + 0x14237, static_cast<u32>(hm.height()));
    r.w32(L.gs + 0x14377, L.models);
    r.w32(L.gs + 0x391E9, L.schema);
    std::vector<std::uint8_t> cells(static_cast<std::size_t>(hm.width()) * hm.height() * 13, 0);
    for (i32 z = 0; z < hm.height(); ++z)
        for (i32 x = 0; x < hm.width(); ++x) cells[(static_cast<std::size_t>(z) * hm.width() + x) * 13 + 4] = hm.at(x, z);
    return r.write(L.cells, cells.data(), cells.size());
}

// Remote unit / player / type offsets.
enum : u32 {
    uMotion = 0x00, uPitch = 0x64, uHeading = 0x66, uBank = 0x68, uPosX = 0x6A, uPosY = 0x6E, uPosZ = 0x72, uCarrier = 0x86,
    uCargoHead = 0x8A, uCargoNext = 0x8E, uDef = 0x92, uOwner = 0x96, uScript = 0x9A, uState = 0x9E, uPathAttach = 0xA2,
    uDefIndex = 0xA6, uId = 0xA8, uNetDirty = 0xBA, uLastDamageKind = 0xF5, uHealth = 0xF6, uPrevHealth = 0xF7,
    uDamagedTimer = 0xFA, uHoldoff = 0xFB, uBuildLeft = 0x104, uHp = 0x108, uActivate = 0x10E, uScriptFlags = 0x10F, uFlags = 0x110,
    pActive = 0x00, pController = 0x73, pFirst = 0x67, pLast = 0x6B, pState = 0x146,
    dCob = 0x18E, dModelMaxY = 0x16E, dWindGen = 0x1D2, dMaxDamage = 0x1FA, dHealTime = 0x200, dTypeFlags = 0x241, kDefSize = 0x249,
};

struct LoadedType {
    const ta::UnitDefRecord* def = nullptr;
    std::vector<std::uint8_t> model_file, cob_file;
    sim::PoseModel pose_model;
    ta::CobFile cob;
    sim::UnitTypeInfo info;
    sim::UnitTickTypeInfo tick_info;
    u32 remote_model = 0, remote_cob = 0, remote_def = 0;
};

struct World : sim::UnitWorld, sim::UnitTickSystems {
    std::vector<sim::Unit>* units = nullptr;
    std::vector<sim::UnitTickState>* ticks = nullptr;
    std::vector<sim::PlayerSlot>* players = nullptr;
    const sim::HeightMap* hm = nullptr;
    sim::BattleRng rng;
    std::vector<u32> log;
    // Pointer-valued log fields are recorded as these tokens on both sides.
    u32 unit_token(const sim::Unit& u) const { return 0x40000000u | u.id; }
    void rec(int id, u32 ecx, std::initializer_list<u32> args) {
        log.push_back(static_cast<u32>(id));
        log.push_back(ecx);
        for (u32 a : args) log.push_back(a);
    }
    // UnitWorld
    sim::Unit* unit_by_id(std::uint16_t id) override { return id < units->size() ? &(*units)[id] : nullptr; }
    const sim::HeightMap& heightmap() override { return *hm; }
    bool yard_cells_free(sim::Unit& u, i32 value) override {
        rec(17, 0, {unit_token(u), static_cast<u32>(value)});  // the original's stub swallows the call
        return false;
    }
    void claim_yard_cells(sim::Unit&) override {}
    void on_activate_flags_changed(sim::Unit& u, std::uint8_t gained, std::uint8_t lost) override {
        if (gained & sim::unit_flags::kActivated) rec(14, 0, {unit_token(u), 3, 0});
        if (lost & sim::unit_flags::kActivated) rec(14, 0, {unit_token(u), 4, 0});
        if (gained & sim::unit_flags::kCloaked) rec(14, 0, {unit_token(u), 0xE, 0});
        if (lost & sim::unit_flags::kCloaked) rec(14, 0, {unit_token(u), 0xF, 0});
        rec(15, 0, {unit_token(u)});
        const auto& owner = (*players)[static_cast<std::size_t>((*ticks)[u.id].owner)];
        if (owner.active && (owner.controller == 1 || owner.controller == 2))
            rec(16, 0, {0, 0x11u | static_cast<u32>(u.id) << 8 | static_cast<u32>(u.activate_flags) << 24, 4});
    }
    std::uint32_t random_mod(std::uint32_t n) override { return rng.next_mod(n); }
    void emit_sfx(sim::Unit& u, i32 piece, i32 type) override { rec(10, unit_token(u), {static_cast<u32>(piece), static_cast<u32>(type)}); }
    void explode_piece(sim::Unit& u, i32 piece, i32 type) override { rec(11, unit_token(u), {static_cast<u32>(piece), static_cast<u32>(type)}); }
    void attach_unit(sim::Unit& u, i32 a, i32 b, i32 c) override {
        rec(12, unit_token(u), {static_cast<u32>(a), static_cast<u32>(b), static_cast<u32>(c)});
    }
    void drop_unit(sim::Unit& u, i32 a) override { rec(13, unit_token(u), {static_cast<u32>(a)}); }
    // UnitTickSystems
    void weapons(sim::Unit& u, u32&) override { rec(1, 0, {unit_token(u)}); }
    void damage(sim::Unit* a, sim::Unit& t, i32 amount, i32 kind, i32 heading) override {
        rec(2, 0, {a ? unit_token(*a) : 0u, unit_token(t), static_cast<u32>(amount), static_cast<u32>(kind), static_cast<u32>(heading)});
    }
    void repair_heal(sim::Unit& w, sim::Unit& t, float rate) override {
        u32 bits;
        std::memcpy(&bits, &rate, 4);
        rec(3, 0, {unit_token(w), unit_token(t), bits});
    }
    void primary_orders(sim::Unit& u, u32&) override { rec(4, 0, {unit_token(u)}); }
    void secondary_orders(sim::Unit& u, u32&) override { rec(5, 0, {unit_token(u)}); }
    void motion(sim::Unit& u, u32&) override { rec(6, 0x7E000000u | u.id, {unit_token(u)}); }
    void settle_height(sim::Unit& u, u32&) override { rec(7, 0, {unit_token(u)}); }
    void killed(sim::Unit& u, std::uint8_t kind, u32&) override { rec(8, 0, {unit_token(u), kind}); }
    void broadcast_player_units(int player, u32&) override { rec(9, 0, {0x20000000u | static_cast<u32>(player)}); }
    void camera_follow_step(u32&) override {}
};

struct Stats {
    std::size_t scenarios = 0, ticks = 0, calls = 0, mismatches = 0, failures = 0, faults = 0;
};

bool run_scenario(harness::Remote& r, const Layout& L, std::vector<LoadedType>& types, const sim::HeightMap& hm, int ticks_to_run,
                  unsigned seed, Stats& st) {
    std::mt19937 g(seed);
    r.w32(L.bump_ptr, L.heap);
    // Fresh game state block, unit table, players.
    std::vector<std::uint8_t> zero(0x40000, 0);
    r.write(L.gs + 0x1B63, zero.data(), 10 * kPlayerSize);
    r.write(L.units, zero.data(), (kMaxUnits + 2) * kUnitSize);
    r.w32(L.gs + 0x14353, 0);  // live unit count left by the previous scenario

    // Pick unit types for this scenario and place their files in remote memory.
    const int ntypes = 3;
    std::vector<int> pick;
    u32 file_at = L.files;
    for (int k = 0; k < ntypes; ++k) {
        LoadedType& t = types[g() % types.size()];
        pick.push_back(static_cast<int>(&t - &types[0]));
        t.remote_model = file_at;
        file_at += (static_cast<u32>(t.model_file.size()) + 15) & ~15u;
        r.write(t.remote_model, t.model_file.data(), t.model_file.size());
        if (!r.call(kRelocate, 0, {t.remote_model, t.remote_model}) || !r.call(kNegateXZ, t.remote_model, {t.remote_model})) return false;
        t.remote_cob = file_at;
        file_at += (static_cast<u32>(t.cob_file.size()) + 15) & ~15u;
        std::vector<std::uint8_t> img = t.cob_file;
        auto rel = [&](std::size_t off) {
            u32 v;
            std::memcpy(&v, &img[off], 4);
            v += t.remote_cob;
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
        r.write(t.remote_cob, img.data(), img.size());
        // Type block.
        t.remote_def = L.defs + static_cast<u32>(k) * 0x400;
        std::vector<std::uint8_t> d(kDefSize, 0);
        auto put = [&](u32 off, const void* p, std::size_t n) { std::memcpy(&d[off], p, n); };
        put(dCob, &t.remote_cob, 4);
        put(dModelMaxY, &t.info.model_max_y, 4);
        put(dWindGen, &t.tick_info.wind_generator, 4);
        put(dMaxDamage, &t.info.max_damage, 4);
        put(dHealTime, &t.tick_info.heal_time, 2);
        put(dTypeFlags, &t.tick_info.type_flags, 4);
        r.write(t.remote_def, d.data(), d.size());
        r.w32(L.models + static_cast<u32>(k + 1) * 4, t.remote_model);
    }

    // Players: slots 0..2 playing (controllers 1, 2, 3), each owning a range of the table.
    std::vector<sim::PlayerSlot> players(10);
    const int per = kMaxUnits / 3;
    for (int p = 0; p < 3; ++p) {
        sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        s.active = true;
        s.controller = static_cast<std::uint8_t>(p + 1);
        s.state = 0;
        s.first_unit = 1 + p * per;
        s.last_unit = s.first_unit + per - 1;
        const u32 at = L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize;
        r.w32(at + pActive, 1);
        r.write(at + pController, &s.controller, 1);
        r.write(at + pState, &s.state, 1);
        r.w32(at + pFirst, L.units + static_cast<u32>(s.first_unit) * kUnitSize);
        r.w32(at + pLast, L.units + static_cast<u32>(s.last_unit) * kUnitSize);
    }

    // Units.
    std::vector<sim::Unit> units(kMaxUnits + 2);
    std::vector<sim::UnitTickState> tstate(kMaxUnits + 2);
    World world;
    world.units = &units;
    world.ticks = &tstate;
    world.players = &players;
    world.hm = &hm;
    const u32 rng0 = static_cast<u32>(g()) | 1u;
    r.w32(kBattleRngState, rng0);
    world.rng.set_state(rng0);
    std::vector<std::unique_ptr<sim::UnitScriptHost>> hosts(units.size());
    std::vector<u32> remote_script(units.size(), 0), remote_state(units.size(), 0);
    for (int i = 1; i <= kMaxUnits; ++i) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
        u.id = static_cast<std::uint16_t>(i);
        t.owner = (i - 1) / per;
        if (t.owner > 2 || g() % 6 == 0) continue;  // empty slot
        const int k = static_cast<int>(g() % ntypes);
        LoadedType& lt = types[static_cast<std::size_t>(pick[static_cast<std::size_t>(k)])];
        t.type_index = static_cast<std::uint16_t>(k + 1);
        t.tick_type = &lt.tick_info;
        u.type = &lt.info;
        u.pos = {static_cast<i32>((g() % 9000 + 300) << 16), static_cast<i32>(g() % 3 == 0 ? 0 : (g() % 255) << 16),
                 static_cast<i32>((g() % 9000 + 300) << 16)};
        u.heading = static_cast<std::uint16_t>(g());
        u.hit_points = static_cast<std::int16_t>(g() % (lt.info.max_damage + 50));
        u.flags = sim::unit_flags::kAlive | (g() % 2 ? 0x30u : 0u) | (g() % 8 == 0 ? 0x4000u : 0u);
        u.build_fraction_left = g() % 4 == 0 ? 0.5f : 0.0f;
        t.has_motion = g() % 2;
        t.damaged_timer = static_cast<std::uint8_t>(g() % 4);
        t.transfer_holdoff = g() % 4;
        t.last_damage_kind = static_cast<std::uint8_t>(g() % 12);
        const u32 at = L.units + static_cast<u32>(i) * kUnitSize;
        r.w32(at + uMotion, t.has_motion ? 0x7E000000u | static_cast<u32>(i) : 0u);
        r.write(at + uHeading, &u.heading, 2);
        r.write(at + uPosX, &u.pos[0], 4);
        r.write(at + uPosY, &u.pos[1], 4);
        r.write(at + uPosZ, &u.pos[2], 4);
        r.w32(at + uDef, lt.remote_def);
        r.w32(at + uOwner, L.gs + 0x1B63 + static_cast<u32>(t.owner) * kPlayerSize);
        r.write(at + uDefIndex, &t.type_index, 2);
        r.write(at + uId, &u.id, 2);
        r.write(at + uLastDamageKind, &t.last_damage_kind, 1);
        r.write(at + uDamagedTimer, &t.damaged_timer, 1);
        r.w32(at + uHoldoff, t.transfer_holdoff);
        r.write(at + uBuildLeft, &u.build_fraction_left, 4);
        r.write(at + uHp, &u.hit_points, 2);
        r.w32(at + uFlags, u.flags);
        // Original script object: state built from the model and script, unit callback table.
        u32 state = 0;
        if (!r.call(kBuildState, 0, {lt.remote_model, lt.remote_cob, at}, &state) || !state) return false;
        const u32 so = L.heap + (64u << 20) + static_cast<u32>(i) * 0x4000;  // script object + tables
        std::vector<std::uint8_t> sz(0x4000, 0);
        r.write(so, sz.data(), sz.size());
        r.w32(so + 0x00, kUnitVtable);
        r.w32(so + 0x04, 30);
        r.w32(so + 0x08, lt.remote_cob);
        r.w32(so + 0x10, so + 0x600);   // statics
        r.w32(so + 0x14, so + 0x1000);  // piece animations
        r.w32(so + 0x540, state);
        r.w32(at + uScript, so);
        r.w32(at + uState, state);
        remote_script[static_cast<std::size_t>(i)] = so;
        remote_state[static_cast<std::size_t>(i)] = state;
        // Engine mirror.
        u.pose.build(lt.pose_model, lt.cob.program.piece_names);
        hosts[static_cast<std::size_t>(i)] = std::make_unique<sim::UnitScriptHost>(u, world);
        u.script = std::make_unique<sim::ScriptInstance>(lt.cob.program, *hosts[static_cast<std::size_t>(i)], 30);
        u.script->set_step_limit(50000000);
    }
    // Run "Create" for every unit in table order on both sides.
    for (int i = 1; i <= kMaxUnits; ++i) {
        if (!units[static_cast<std::size_t>(i)].script) continue;
        r.write(L.names, "Create", 7);
        if (!r.call(kCallNamed, remote_script[static_cast<std::size_t>(i)], {L.names, 0, 1})) return false;
        units[static_cast<std::size_t>(i)].script->call("Create", nullptr, true);
    }
    ++st.scenarios;

    sim::UnitTickEnvironment env;
    std::int16_t camera = 0;
    i32 env_live = 0;
    auto compare = [&](const std::string& what) -> bool {
        bool ok = true;
        auto bad = [&](const std::string& m) {
            if (ok) std::printf("scenario %u %s: %s\n", seed, what.c_str(), m.c_str());
            ok = false;
        };
        // Stub call log: pointers to units/players become tokens.
        const u32 end = r.r32(L.log_ptr);
        std::vector<u32> log((end - L.log_base) / 4);
        if (!log.empty()) r.read(L.log_base, log.data(), log.size() * 4);
        r.w32(L.log_ptr, L.log_base);
        for (auto& w : log) {
            if (w >= L.units && w < L.units + (kMaxUnits + 2) * kUnitSize && (w - L.units) % kUnitSize == 0)
                w = 0x40000000u | ((w - L.units) / kUnitSize);
            else if (w >= L.gs + 0x1B63 && w < L.gs + 0x1B63 + 10 * kPlayerSize && (w - L.gs - 0x1B63) % kPlayerSize == 0)
                w = 0x20000000u | ((w - L.gs - 0x1B63) / kPlayerSize);
            else if (w >= L.heap + (64u << 20) && w < L.heap + (64u << 20) + 0x4000u * (kMaxUnits + 2) && (w - L.heap - (64u << 20)) % 0x4000 == 0)
                w = 0x40000000u | ((w - L.heap - (64u << 20)) / 0x4000);  // ECX of script callbacks = the script object
        }
        // The network packet is built on the stack: compare its content instead of its address.
        for (std::size_t i = 0; i + 1 < log.size();) {
            const u32 id = log[i];
            int nargs = 0;
            for (const Stub& s : kStubs)
                if (static_cast<u32>(s.id) == id) nargs = s.args;
            if (id != 6 && (id < 10 || id > 13)) log[i + 1] = 0;  // ECX is undefined for stdcall callees
            if (id == 16) {
                log[i + 2] = 0;  // player net id
                log[i + 3] = 0;  // packet address (content not captured)
            }
            i += 2 + static_cast<std::size_t>(nargs);
        }
        std::vector<u32> mine = world.log;
        for (std::size_t i = 0; i + 1 < mine.size();) {
            const u32 id = mine[i];
            int nargs = 0;
            for (const Stub& s : kStubs)
                if (static_cast<u32>(s.id) == id) nargs = s.args;
            if (id == 16) mine[i + 3] = 0;
            i += 2 + static_cast<std::size_t>(nargs);
        }
        world.log.clear();
        if (log != mine) {
            bad("subsystem call log differs");
            std::size_t first = 0;
            while (first < log.size() && first < mine.size() && log[first] == mine[first]) ++first;
            const std::size_t from = first > 12 ? first - 12 : 0;
            for (std::size_t i = from; i < first + 12 && i < (std::max)(log.size(), mine.size()); ++i) {
                const long long a = i < log.size() ? static_cast<long long>(log[i]) : -1;
                const long long b = i < mine.size() ? static_cast<long long>(mine[i]) : -1;
                std::printf("    log[%zu] orig %llx ours %llx%s\n", i, a, b, a != b ? "   <--" : "");
            }
        }
        if (static_cast<i32>(r.r32(L.gs + 0x14353)) != env_live) bad("live unit count");
        if (r.r32(kBattleRngState) != world.rng.state()) bad("battle RNG state");
        for (int i = 1; i <= kMaxUnits; ++i) {
            sim::Unit& u = units[static_cast<std::size_t>(i)];
            const sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
            if (!u.script) continue;
            const u32 at = L.units + static_cast<u32>(i) * kUnitSize;
            std::vector<std::uint8_t> ru(kUnitSize);
            r.read(at, ru.data(), ru.size());
            auto f8 = [&](u32 off) { return ru[off]; };
            auto f16 = [&](u32 off) { std::uint16_t v; std::memcpy(&v, &ru[off], 2); return v; };
            auto f32 = [&](u32 off) { u32 v; std::memcpy(&v, &ru[off], 4); return v; };
            const std::string un = "unit " + std::to_string(i) + " ";
            if (f32(uFlags) != u.flags) bad(un + "flags");
            if (f8(uHealth) != t.health_percent || f8(uPrevHealth) != t.prev_health_percent) bad(un + "health percent");
            if (f8(uDamagedTimer) != t.damaged_timer || f32(uHoldoff) != t.transfer_holdoff) bad(un + "timers");
            if (f8(uActivate) != u.activate_flags || f8(uScriptFlags) != u.script_flags) bad(un + "activate/script flags");
            if (f16(uNetDirty) != u.net_dirty) bad(un + "net dirty");
            // Script: threads + count, piece animations, statics.
            const u32 so = remote_script[static_cast<std::size_t>(i)];
            std::vector<u32> blk(sim::ScriptInstance::kThreads * sim::ScriptInstance::kThreadWords + 1);
            r.read(so + 0x1C, blk.data(), blk.size() * 4);
            const auto& mem = u.script->thread_memory();
            for (std::size_t w = 0; w < blk.size(); ++w)
                if (blk[w] != mem[w]) {
                    bad(un + "script thread word " + std::to_string(w));
                    break;
                }
            std::vector<i32> an(u.script->piece_anims().size());
            if (!an.empty()) r.read(so + 0x1000, an.data(), an.size() * 4);
            if (an != u.script->piece_anims()) bad(un + "script piece animations");
            std::vector<i32> sv(u.script->statics().size());
            if (!sv.empty()) r.read(so + 0x600, sv.data(), sv.size() * 4);
            if (sv != u.script->statics()) bad(un + "script statics");
            // Pose: move/turn/flags of every piece.
            const u32 state = remote_state[static_cast<std::size_t>(i)];
            std::vector<std::uint8_t> ps(u.pose.pieces.size() * 54);
            if (!ps.empty()) r.read(state + 0x22, ps.data(), ps.size());
            for (std::size_t p = 0; p < u.pose.pieces.size(); ++p) {
                const auto& o = u.pose.pieces[p];
                i32 mv[3];
                std::int16_t tu[3];
                std::uint16_t fl;
                std::memcpy(mv, &ps[p * 54 + 4], 12);
                std::memcpy(tu, &ps[p * 54 + 16], 6);
                std::memcpy(&fl, &ps[p * 54 + 40], 2);
                if (mv[0] != o.move[0] || mv[1] != o.move[1] || mv[2] != o.move[2] || tu[0] != o.turn[0] || tu[1] != o.turn[1] ||
                    tu[2] != o.turn[2] || fl != o.flags) {
                    bad(un + "piece " + std::to_string(p));
                    break;
                }
            }
            if (!u.script->fault().empty()) bad(un + "script fault: " + u.script->fault());
        }
        if (!ok) ++st.mismatches;
        return ok;
    };
    auto engine_faulted = [&]() {
        for (const auto& u : units)
            if (u.script && !u.script->fault().empty()) {
                ++st.faults;
                std::printf("scenario %u: stopped, engine script fault: %s\n", seed, u.script->fault().c_str());
                return true;
            }
        return false;
    };
    if (engine_faulted()) return true;
    if (!compare("after Create")) return true;

    for (int tick = 0; tick < ticks_to_run; ++tick) {
        // Environment for this step (both sides).
        env.game_tick = g() % 4 == 0 ? static_cast<u32>(g()) : static_cast<u32>(tick);
        env.wind_changed = g() % 8 == 0;
        env.wind_heading = static_cast<std::uint16_t>(g());
        env.wind_speed = static_cast<i32>(g() % 3000);
        env.water_damage_enabled = g() % 3 == 0;
        env.water_damage = static_cast<i32>(g() % 3) * 50;
        env.sea_level = static_cast<std::uint8_t>(g());
        env.broadcast_unit_states = g() % 2;
        r.w32(L.gs + 0x38A47, env.game_tick);
        r.w32(L.gs + 0x37EE2, env.wind_changed ? 1u : 0u);
        r.write(L.gs + 0x37ED8, &env.wind_heading, 2);
        r.w32(L.gs + 0x37EDA, static_cast<u32>(env.wind_speed));
        r.w32(L.schema + 0xD4C, env.water_damage_enabled ? 1u : 0u);
        r.w32(L.schema + 0xD50, static_cast<u32>(env.water_damage));
        r.write(L.gs + 0x1427F, &env.sea_level, 1);
        r.w32(L.gs + 0x2A44, env.broadcast_unit_states ? 1u : 0u);
        r.w32(L.gs + 0x14373, 0);  // camera auto-follow off (it reads the keyboard)

        // Sometimes call a random script function of a random unit.
        if (g() % 3 == 0) {
            const int i = 1 + static_cast<int>(g() % kMaxUnits);
            sim::Unit& u = units[static_cast<std::size_t>(i)];
            if (u.script) {
                const auto& names = u.script->program().function_names;
                const std::string name = names.empty() || g() % 8 == 0 ? std::string("NoSuchFunction") : names[g() % names.size()];
                const bool run_now = g() % 2;
                const i32 argc = static_cast<i32>(g() % 5);
                i32 a[4];
                for (auto& x : a) x = g() % 10 < 8 ? static_cast<i32>(g() % 2) : static_cast<i32>(g() % 65536);
                u.script->call_with_args(u.script->program().find_function(name), nullptr, run_now, argc, a[0], a[1], a[2], a[3]);
                if (engine_faulted()) return true;
                r.write(L.names, name.c_str(), name.size() + 1);
                if (!r.call(kScriptCall, remote_script[static_cast<std::size_t>(i)],
                            {L.names, 0, run_now ? 1u : 0u, static_cast<u32>(argc), static_cast<u32>(a[0]), static_cast<u32>(a[1]),
                             static_cast<u32>(a[2]), static_cast<u32>(a[3])}))
                    return false;
                ++st.calls;
                if (!compare("call " + name + " tick " + std::to_string(tick))) return true;
            }
        }

        // One unit pass.
        env_live = sim::run_unit_pass(units, tstate, players, env, world, camera);
        if (engine_faulted()) return true;
        if (!r.call(kUnitPass, 0, {})) return false;
        ++st.ticks;
        if (!compare("tick " + std::to_string(tick))) return true;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: unitloop_harness <TotalA.exe> <game_dir> [scenarios] [ticks]\n");
        return 2;
    }
    const int scenarios = argc > 3 ? std::atoi(argv[3]) : 50;
    const int ticks = argc > 4 ? std::atoi(argv[4]) : 300;
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[2]);
    ta::UnitDefContext ctx;
    ta::load_unit_context(vfs, ctx);
    const ta::UnitDefTable defs = ta::load_unit_defs(vfs, ctx);
    std::vector<std::uint8_t> tnt;
    sim::HeightMap hm;
    if (!vfs.read("maps\\Moon Quartet.tnt", tnt) || !ta::tnt_load_heights(tnt, hm)) {
        std::fprintf(stderr, "cannot load the Moon Quartet heights\n");
        return 1;
    }
    // Every unit type with a model and a script.
    std::vector<LoadedType> types;
    for (std::size_t i = 1; i < defs.units.size(); ++i) {
        const auto& d = defs.units[i];
        LoadedType t;
        t.def = &d;
        std::string err;
        if (!vfs.read("objects3d\\" + d.object_name + ".3DO", t.model_file) || !vfs.read("scripts\\" + d.unit_name + ".COB", t.cob_file))
            continue;
        const auto m = ta::load_3do(t.model_file, true);
        if (!m.root || !ta::load_cob(t.cob_file, t.cob, err)) continue;
        t.pose_model = ta::to_pose_model(m.root.get());
        t.info.max_damage = d.max_damage ? d.max_damage : 1;
        t.info.model_max_y = d.model_max_y;
        t.tick_info.wind_generator = d.wind_generator;
        t.tick_info.heal_time = static_cast<std::uint16_t>(d.heal_time);
        t.tick_info.type_flags = d.type_flags;
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
        if (!run_scenario(r, L, types, hm, ticks, static_cast<unsigned>(s * 7 + 11), st)) {
            ++st.failures;
            std::printf("scenario %d: original call failed - restarting\n", s);
            r.stop();
            if (!r.start(argv[1]) || !setup(r, L, hm)) return 1;
        }
    }
    std::printf("scenarios %zu, unit passes %zu, script calls %zu, mismatching scenarios %zu, engine faults %zu, original failures %zu\n",
                st.scenarios, st.ticks, st.calls, st.mismatches, st.faults, st.failures);
    return st.mismatches || st.failures ? 1 : 0;
}