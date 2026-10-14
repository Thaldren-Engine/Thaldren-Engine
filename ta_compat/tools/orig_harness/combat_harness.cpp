// Differential tester for damage, healing and death (sim/combat.h) together
// with the per-step unit pass. Inside a suspended TotalA.exe it builds a small
// battle like unitloop_harness (three players, units of random real types with
// their scripts and object states, logging stubs for every subsystem not
// modelled), but runs the original damage, repair/heal and kill routines for
// real. Each step it applies random damage, heals, kills and received
// network packets on both sides, then one unit pass, and compares the stub
// call log (with network packet contents), every unit and player field these
// routines touch, scripts, poses, the battle RNG and the live count.
//
// Usage: combat_harness <TotalA.exe> <game_dir> [scenarios] [ticks]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/combat.h"
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
constexpr u32 kRelocate = 0x004CB4C0, kNegateXZ = 0x004CB590, kBuildState = 0x0045A950;
constexpr u32 kCallNamed = 0x004B0940, kScriptCall = 0x004B0A70, kUnitPass = 0x0048AD30;
constexpr u32 kApplyDamage = 0x00489BB0, kApplyDamagePacket = 0x00489CE0, kRepairHeal = 0x0041BD10, kKillUnit = 0x004864B0,
              kApplyKilledPacket = 0x004866D0;
constexpr u32 kUnitVtable = 0x004FD698;
constexpr u32 kUnitSize = 0x118, kPlayerSize = 0x14B;
constexpr int kMaxUnits = 16;

enum class StubRet { Zero, Variable, Silent };
// Subsystems replaced by stubs: address, argument count, log id, extra logged packet dwords, return.
struct Stub {
    u32 address;
    int args;
    int id;
    int extra = 0;
    StubRet ret = StubRet::Zero;
};
const Stub kStubs[] = {
    {0x0049E1A0, 1, 1},   // weapons(unit)
    {0x0043B7C0, 1, 4},   // primary orders(unit)
    {0x0043BAD0, 1, 5},   // secondary orders(unit)
    {0x0043DD20, 1, 6},   // motion(unit) [thiscall on the motion object]
    {0x0048A870, 1, 7},   // settle height(unit)
    {0x0048B710, 1, 9},   // broadcast player units(player)
    {0x00480EB0, 2, 10},  // script emit sfx(piece, type) [thiscall]
    {0x00481140, 2, 11},  // script explode(piece, type)
    {0x00481340, 3, 12},  // script attach(unit, piece, mode)
    {0x004813B0, 1, 13},  // script drop(unit)
    {0x0047F780, 3, 14},  // speech(unit, event, 0)
    {0x0041C110, 1, 15},  // HUD dirty(unit)
    {0x00451DF0, 3, 16, 3},  // network send(net id, packet, size) + the first 12 packet bytes
    {0x0047DAC0, 2, 17},  // yard open(unit, open)
    {0x00406F80, 3, 20},  // retarget(attacker, victim, amount)
    {0x00494FF0, 1, 21},  // statistics activity(kind)
    {0x00438760, 1, 22},  // mission type by name(name) [thiscall on a stack object]
    {0x0043A0C0, 6, 23},  // mission order constructor(type, 0, 0, amount, 0, 0) [thiscall]
    {0x0043ACB0, 2, 24},  // prepend order(unit, order)
    {0x00401180, 2, 25, 0, StubRet::Variable},  // pay energy(resource slot, float amount)
    {0x00482910, 4, 26},  // reveal dead unit(&pos, sight, radar, 60)
    {0x004904C0, 1, 27},  // victory: unit destroyed(unit) [thiscall]
    {0x00439EB0, 2, 28},  // clear orders(unit, 1)
    {0x0047F8C0, 1, 29},  // forget speech(unit)
    {0x00480250, 2, 30},  // leave group(unit, -1)
    {0x0049C880, 1, 31},  // remove projectiles(unit)
    {0x0048AAC0, 4, 32},  // detach from carrier(unit, 0, -1, 1)
    {0x0047CBD0, 1, 33},  // clear footprint(unit)
    {0x00482090, 1, 34},  // forget fog(unit)
    {0x004948B0, 2, 35},  // kill flash(attacker slot, victim state)
    {0x0049B000, 2, 36},  // detonate(unit, self destruct)
    {0x00486360, 3, 37},  // spawn wreck(unit, level, burn)
    {0x00489740, 1, 38},  // release unit(unit)
    {0x00450380, 1, 39},  // network: player out of units(net id)
    {0x0047BD70, 1, 40},  // AI: player out of units(player)
    {0x00491D70, 1, 41},  // commander death: close menus(1)
    {0x00486F10, 1, 42},  // commander death: kill the team(slot)
    {0x00485E30, 1, 50, 0, StubRet::Silent},  // script object destructor [thiscall]
    {0x0045AAA0, 1, 51, 0, StubRet::Silent},  // free object state
    {0x0043DD10, 0, 52, 0, StubRet::Silent},  // motion destructor [thiscall]
    {0x004B4F20, 0, 53, 0, StubRet::Silent},  // free [cdecl]
};

const Stub* stub_by_id(u32 id) {
    for (const Stub& s : kStubs)
        if (static_cast<u32>(s.id) == id) return &s;
    return nullptr;
}

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
};

// Logs {id, ECX, args..., packet dwords...} at *log_ptr, then returns with `ret 4*args`.
void emit_stub(Emitter& e, const Stub& s, u32 log_ptr_addr, u32 ret_addr) {
    const bool cdecl_ret = s.address == 0x004B4F20;
    if (s.ret != StubRet::Silent) {
        e.u8(0xA1); e.u32le(log_ptr_addr);                         // mov eax,[log_ptr]
        e.u8(0xC7); e.u8(0x00); e.u32le(static_cast<u32>(s.id));   // mov dword [eax],id
        e.u8(0x89); e.u8(0x48); e.u8(0x04);                         // mov [eax+4],ecx
        for (int i = 0; i < s.args; ++i) {
            e.u8(0x8B); e.u8(0x54); e.u8(0x24); e.u8(static_cast<std::uint8_t>(4 + 4 * i));  // mov edx,[esp+4+4i]
            e.u8(0x89); e.u8(0x50); e.u8(static_cast<std::uint8_t>(8 + 4 * i));              // mov [eax+8+4i],edx
        }
        if (s.extra) {
            e.u8(0x8B); e.u8(0x54); e.u8(0x24); e.u8(0x08);  // mov edx,[esp+8] (packet)
            for (int k = 0; k < s.extra; ++k) {
                e.u8(0x8B); e.u8(0x4A); e.u8(static_cast<std::uint8_t>(4 * k));                        // mov ecx,[edx+4k]
                e.u8(0x89); e.u8(0x48); e.u8(static_cast<std::uint8_t>(8 + 4 * (s.args + k)));          // mov [eax+..],ecx
            }
        }
        e.u8(0x83); e.u8(0x05); e.u32le(log_ptr_addr); e.u8(static_cast<std::uint8_t>(4 * (s.args + s.extra + 2)));
    }
    if (s.ret == StubRet::Variable) {
        e.u8(0xA1); e.u32le(ret_addr);  // mov eax,[ret]
    } else {
        e.u8(0x33); e.u8(0xC0);  // xor eax,eax
    }
    if (cdecl_ret || s.args == 0) {
        e.u8(0xC3);
    } else {
        e.u8(0xC2); e.u8(static_cast<std::uint8_t>(4 * s.args)); e.u8(0x00);
    }
}

struct Layout {
    u32 heap = 0, bump_ptr = 0, stubs = 0, log_ptr = 0, log_base = 0, ret_val = 0, gs = 0, units = 0, defs = 0, models = 0,
        cells = 0, schema = 0, names = 0, files = 0, pinfo = 0, empty_def = 0;
};

bool setup(harness::Remote& r, Layout& L, const sim::HeightMap& hm) {
    L.heap = r.alloc(128u << 20);
    L.bump_ptr = r.alloc(0x1000);
    L.stubs = r.alloc(0x4000, PAGE_EXECUTE_READWRITE);
    L.log_ptr = r.alloc(0x1000);
    L.log_base = r.alloc(4u << 20);
    L.ret_val = r.alloc(0x1000);
    L.gs = r.alloc(0x40000);
    L.units = r.alloc(static_cast<SIZE_T>(65536) * kUnitSize);
    L.defs = r.alloc(0x10000);
    L.models = r.alloc(0x1000);
    L.cells = r.alloc(static_cast<SIZE_T>(hm.width()) * hm.height() * 13);
    L.schema = r.alloc(0x1000);
    L.names = r.alloc(0x1000);
    L.files = r.alloc(32u << 20);
    L.pinfo = r.alloc(0x1000);
    L.empty_def = r.alloc(0x1000);
    if (!L.heap || !L.stubs || !L.log_base || !L.gs || !L.units || !L.cells || !L.files || !L.pinfo || !L.empty_def) return false;
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
        emit_stub(e, s, L.log_ptr, L.ret_val);
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
    r.w32(L.gs + 0x391E9, L.schema);  // its first word (network provider type) stays 0
    r.w32(L.gs + 0x1439B, L.empty_def);
    std::vector<std::uint8_t> cells(static_cast<std::size_t>(hm.width()) * hm.height() * 13, 0);
    for (i32 z = 0; z < hm.height(); ++z)
        for (i32 x = 0; x < hm.width(); ++x) cells[(static_cast<std::size_t>(z) * hm.width() + x) * 13 + 4] = hm.at(x, z);
    return r.write(L.cells, cells.data(), cells.size());
}

// Remote unit / player / type offsets.
enum : u32 {
    uMotion = 0x00, uOrders = 0x5C, uHeading = 0x66, uPosX = 0x6A, uPosY = 0x6E, uPosZ = 0x72, uCarrier = 0x86, uCargoHead = 0x8A,
    uDef = 0x92, uOwner = 0x96, uScript = 0x9A, uState = 0x9E, uDefIndex = 0xA6, uId = 0xA8, uKills = 0xB8, uNetDirty = 0xBA,
    uResource = 0xBC, uMetalMake = 0xD4, uResourcePlayer = 0xEC, uLastAttacker = 0xF0, uLastAttackerSlot = 0xF4,
    uLastDamageKind = 0xF5, uHealth = 0xF6, uPrevHealth = 0xF7, uDamagedTimer = 0xFA, uHoldoff = 0xFB, uOwnerSlot = 0xFF,
    uBuildLeft = 0x104, uHp = 0x108, uActivate = 0x10E, uScriptFlags = 0x10F, uFlags = 0x110,
    pActive = 0x00, pNetId = 0x04, pInfo = 0x27, pFirst = 0x67, pLast = 0x6B, pController = 0x73, pUnitsKilled = 0xFC,
    pUnitsLost = 0xFE, pCommandersKilled = 0x104, pCommandersLost = 0x106, pSharesVision = 0x129, pAlive = 0x144, pState = 0x146,
    pFlags = 0x149,
    dName = 0x20, dModelMaxY = 0x16E, dEnergyCost = 0x186, dMetalCost = 0x18A, dCob = 0x18E, dDamageModifier = 0x1AA,
    dWindGen = 0x1D2, dBuildTime = 0x1EA, dMaxDamage = 0x1FA, dHealTime = 0x200, dTypeFlags = 0x241, kDefSize = 0x249,
    gLocalPlayer = 0x2A42, gLocalView = 0x2A43, gMapFlags = 0x14281, gDifficulty = 0x37EEE, gGameMode = 0x37EF6,
    gFlash = 0x37F06, gCommanderNames = 0x37F5F, kSideSize = 0x232,
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

u32 fbits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

struct World : sim::UnitWorld, sim::UnitTickSystems, sim::CombatSystems {
    std::vector<sim::Unit>* units = nullptr;
    std::vector<sim::UnitTickState>* ticks = nullptr;
    std::vector<sim::PlayerSlot>* players = nullptr;
    sim::Combat* combat = nullptr;
    const sim::HeightMap* hm = nullptr;
    sim::BattleRng rng;
    std::vector<u32> log;
    u32 pay_result = 0;
    bool kill_flash = false;
    // Pointer-valued log fields are recorded as these tokens on both sides.
    static u32 tok(const sim::Unit& u) { return 0x40000000u | u.id; }
    void rec(int id, u32 ecx, std::initializer_list<u32> args) {
        log.push_back(static_cast<u32>(id));
        log.push_back(ecx);
        for (u32 a : args) log.push_back(a);
    }
    sim::PlayerSlot& owner_of(const sim::Unit& u) { return (*players)[static_cast<std::size_t>((*ticks)[u.id].owner)]; }
    // UnitWorld
    sim::Unit* unit_by_id(std::uint16_t id) override { return id < units->size() ? &(*units)[id] : nullptr; }
    const sim::HeightMap& heightmap() override { return *hm; }
    bool yard_cells_free(sim::Unit& u, i32 value) override {
        rec(17, 0, {tok(u), static_cast<u32>(value)});
        return false;
    }
    void claim_yard_cells(sim::Unit&) override {}
    void on_activate_flags_changed(sim::Unit& u, std::uint8_t gained, std::uint8_t lost) override {
        if (gained & sim::unit_flags::kActivated) rec(14, 0, {tok(u), 3, 0});
        if (lost & sim::unit_flags::kActivated) rec(14, 0, {tok(u), 4, 0});
        if (gained & sim::unit_flags::kCloaked) rec(14, 0, {tok(u), 0xE, 0});
        if (lost & sim::unit_flags::kCloaked) rec(14, 0, {tok(u), 0xF, 0});
        rec(15, 0, {tok(u)});
        const auto& owner = owner_of(u);
        if (owner.active && (owner.controller == 1 || owner.controller == 2)) rec(16, 0, {0, 0, 4, 0, 0, 0});  // compared by size only
    }
    std::uint32_t random_mod(std::uint32_t n) override { return rng.next_mod(n); }
    void emit_sfx(sim::Unit& u, i32 piece, i32 type) override { rec(10, tok(u), {static_cast<u32>(piece), static_cast<u32>(type)}); }
    void explode_piece(sim::Unit& u, i32 piece, i32 type) override { rec(11, tok(u), {static_cast<u32>(piece), static_cast<u32>(type)}); }
    void attach_unit(sim::Unit& u, i32 a, i32 b, i32 c) override {
        rec(12, tok(u), {static_cast<u32>(a), static_cast<u32>(b), static_cast<u32>(c)});
    }
    void drop_unit(sim::Unit& u, i32 a) override { rec(13, tok(u), {static_cast<u32>(a)}); }
    // UnitTickSystems
    void weapons(sim::Unit& u) override { rec(1, 0, {tok(u)}); }
    void damage(sim::Unit* a, sim::Unit& t, i32 amount, i32 kind, i32 heading) override {
        sim::apply_damage(*combat, a, t, amount, kind, heading);
    }
    void repair_heal(sim::Unit& w, sim::Unit& t, float rate) override { sim::repair_heal(*combat, w, t, rate); }
    void primary_orders(sim::Unit& u) override { rec(4, 0, {tok(u)}); }
    void secondary_orders(sim::Unit& u) override { rec(5, 0, {tok(u)}); }
    void motion(sim::Unit& u) override { rec(6, 0x7E000000u | u.id, {tok(u)}); }
    void settle_height(sim::Unit& u) override { rec(7, 0, {tok(u)}); }
    void killed(sim::Unit& u, std::uint8_t kind) override { sim::kill_unit(*combat, u, kind); }
    void broadcast_player_units(int player) override { rec(9, 0, {0x20000000u | static_cast<u32>(player)}); }
    void camera_follow_step() override {}
    // CombatSystems
    void retarget(sim::Unit* a, sim::Unit& v, std::uint16_t amount) override { rec(20, 0, {a ? tok(*a) : 0u, tok(v), amount}); }
    void add_paralysis(sim::Unit& v, std::uint16_t amount) override {
        rec(22, 0, {0x00508D80});
        rec(23, 0, {0, 0, 0, amount, 0, 0});
        rec(24, 0, {tok(v), 0});
    }
    bool pay_energy(sim::Unit& w, float amount) override {
        rec(25, 0, {0x41000000u | w.id, fbits(amount)});
        return pay_result != 0;
    }
    void stats_activity(i32 kind) override { rec(21, 0, {static_cast<u32>(kind)}); }
    void network_send(i32 net, const std::uint8_t* p, i32 size) override {
        u32 d[3] = {};
        std::memcpy(d, p, static_cast<std::size_t>(size));
        rec(16, 0, {static_cast<u32>(net), 0, static_cast<u32>(size), d[0], d[1], d[2]});
    }
    void reveal_dead_unit(sim::Unit& u) override {
        // Sight range (+0x202, 0 here) and the integer part of the model height (+0x170).
        const auto height = static_cast<std::int16_t>(static_cast<u32>(u.type ? u.type->model_max_y : 0) >> 16);
        rec(26, 0, {0x42000000u | u.id, 0, static_cast<u32>(static_cast<i32>(height)), 60});
    }
    void victory_unit_destroyed(sim::Unit& u) override { rec(27, 0, {tok(u)}); }
    void clear_orders(sim::Unit& u) override { rec(28, 0, {tok(u), 1}); }
    void forget_speech(sim::Unit& u) override { rec(29, 0, {tok(u)}); }
    void leave_group(sim::Unit& u) override { rec(30, 0, {tok(u), 0xFFFFFFFFu}); }
    void remove_projectiles(sim::Unit& u) override { rec(31, 0, {tok(u)}); }
    void detach_from_carrier(sim::Unit& u) override { rec(32, 0, {tok(u), 0, 0xFFFFFFFFu, 1}); }
    void clear_footprint(sim::Unit& u) override { rec(33, 0, {tok(u)}); }
    void forget_fog(sim::Unit& u) override { rec(34, 0, {tok(u)}); }
    void kill_counted(std::uint8_t slot, sim::Unit& v) override {
        // The kill-lead messages need a network game (provider type 0 here); then the kill flash.
        if (kill_flash) rec(35, 0, {slot, owner_of(v).state});
    }
    void detonate(sim::Unit& u, bool self_destruct) override { rec(36, 0, {tok(u), self_destruct ? 1u : 0u}); }
    void spawn_wreck(sim::Unit& u, i32 level, bool burn) override { rec(37, 0, {tok(u), static_cast<u32>(level), burn ? 1u : 0u}); }
    void release_unit(sim::Unit& u) override { rec(38, 0, {tok(u)}); }
    void release_motion(sim::Unit&) override {}
    void player_out_of_units(int) override {}  // only network/AI announcements, none with provider type 0 here
    void commander_died(int player) override {
        rec(41, 0, {1});
        rec(42, 0, {static_cast<u32>(player)});
    }
};

struct Stats {
    std::size_t scenarios = 0, ticks = 0, ops = 0, deaths = 0, mismatches = 0, failures = 0, faults = 0;
};

std::string random_case(std::string s, std::mt19937& g) {
    for (char& ch : s)
        if (g() % 2) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : (ch >= 'A' && ch <= 'Z' ? ch + 32 : ch));
    return s;
}

bool run_scenario(harness::Remote& r, const Layout& L, std::vector<LoadedType>& types, const sim::HeightMap& hm, int ticks_to_run,
                  unsigned seed, Stats& st) {
    std::mt19937 g(seed);
    r.w32(L.bump_ptr, L.heap);
    std::vector<std::uint8_t> zero(0x40000, 0);
    r.write(L.gs + 0x1B63, zero.data(), 10 * kPlayerSize);
    r.write(L.units, zero.data(), (kMaxUnits + 2) * kUnitSize);
    r.write(L.gs + gCommanderNames, zero.data(), 4 * kSideSize);
    r.w32(L.gs + 0x14353, 0);

    // Unit types.
    const int ntypes = 3;
    std::vector<int> pick;
    u32 file_at = L.files;
    for (int k = 0; k < ntypes; ++k) {
        LoadedType& t = types[g() % types.size()];
        pick.push_back(static_cast<int>(&t - &types[0]));
        t.remote_model = file_at;
        file_at += (static_cast<u32>(t.model_file.size()) + 15) & ~15u;
        r.write(t.remote_model, t.model_file.data(), t.model_file.size());
        if (!r.call(kRelocate, 0, {t.remote_model, t.remote_model}) || !r.call(kNegateXZ, t.remote_model, {t.remote_model})) { std::printf("relocate failed\n"); return false; }
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
        // Random combat values for this scenario (scripts and models stay real).
        t.tick_info.damage_modifier = g() % 2 ? 0x10000 : static_cast<i32>(g() % 0x20000) - (g() % 8 == 0 ? 0x8000 : 0);
        t.tick_info.build_time = g() % 10 == 0 ? 0 : static_cast<i32>(g() % 20000);
        t.tick_info.build_cost_energy = g() % 8 == 0 ? 0.0f : static_cast<float>(g() % 100000) / 7.0f;
        t.tick_info.build_cost_metal = static_cast<float>(g() % 5000) / 3.0f;
        t.remote_def = L.defs + static_cast<u32>(k) * 0x400;
        std::vector<std::uint8_t> d(kDefSize, 0);
        auto put = [&](u32 off, const void* p, std::size_t n) { std::memcpy(&d[off], p, n); };
        put(dName, t.tick_info.name.c_str(), (std::min)(t.tick_info.name.size() + 1, std::size_t{32}));
        put(dCob, &t.remote_cob, 4);
        put(dModelMaxY, &t.info.model_max_y, 4);
        put(dEnergyCost, &t.tick_info.build_cost_energy, 4);
        put(dMetalCost, &t.tick_info.build_cost_metal, 4);
        put(dDamageModifier, &t.tick_info.damage_modifier, 4);
        put(dWindGen, &t.tick_info.wind_generator, 4);
        put(dBuildTime, &t.tick_info.build_time, 4);
        put(dMaxDamage, &t.info.max_damage, 4);
        put(dHealTime, &t.tick_info.heal_time, 2);
        put(dTypeFlags, &t.tick_info.type_flags, 4);
        r.write(t.remote_def, d.data(), d.size());
        r.w32(L.models + static_cast<u32>(k + 1) * 4, t.remote_model);
    }
    std::vector<std::uint8_t> ed(kDefSize, 0);
    r.write(L.empty_def, ed.data(), ed.size());
    sim::UnitTypeInfo empty_type;
    empty_type.max_damage = 0;
    sim::UnitTickTypeInfo empty_tick;
    empty_tick.damage_modifier = 0;  // the zeroed type block dead units point at

    // Players: slots 0..2 playing (controllers 1, 2, 3), a few idle slots with net ids.
    std::vector<sim::PlayerSlot> players(10);
    const int per = kMaxUnits / 3;
    std::vector<std::string> side_names(3);
    for (int s = 0; s < 3; ++s) {
        const LoadedType& t = types[static_cast<std::size_t>(pick[g() % pick.size()])];
        side_names[static_cast<std::size_t>(s)] = random_case(t.tick_info.name, g);
        const std::string& n = side_names[static_cast<std::size_t>(s)];
        r.write(L.gs + gCommanderNames + static_cast<u32>(s) * kSideSize, n.c_str(), (std::min)(n.size() + 1, std::size_t{32}));
    }
    for (int p = 0; p < 10; ++p) {
        sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        const bool playing = p < 3;
        s.active = playing || g() % 3 == 0;
        s.controller = static_cast<std::uint8_t>(playing ? p + 1 : (g() % 2 ? 0 : 1 + g() % 3));
        s.state = static_cast<std::uint8_t>(g() % 4);
        s.net_id = g() % 6 == 0 ? -1 : static_cast<i32>(g() % 5);
        if (playing) {
            s.first_unit = 1 + p * per;
            s.last_unit = s.first_unit + per - 1;
        }
        s.units_killed = static_cast<std::uint16_t>(g() % 50);
        s.units_lost = static_cast<std::uint16_t>(g() % 50);
        s.commanders_killed = static_cast<std::uint16_t>(g() % 3);
        s.commanders_lost = static_cast<std::uint16_t>(g() % 3);
        s.flags = static_cast<std::uint16_t>(g());
        for (auto& v : s.shares_vision) v = static_cast<std::uint8_t>(g() % 3 == 0);
        const int side = static_cast<int>(g() % 3);
        s.commander_name = side_names[static_cast<std::size_t>(side)];
        const u32 at = L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize;
        const u32 info = L.pinfo + static_cast<u32>(p) * 0x100;
        const std::uint8_t side_byte = static_cast<std::uint8_t>(side);
        r.write(info + 0x95, &side_byte, 1);
        r.w32(at + pActive, s.active ? 1u : 0u);
        r.w32(at + pNetId, static_cast<u32>(s.net_id));
        r.w32(at + pInfo, info);
        r.write(at + pController, &s.controller, 1);
        r.write(at + pState, &s.state, 1);
        r.write(at + pUnitsKilled, &s.units_killed, 2);
        r.write(at + pUnitsLost, &s.units_lost, 2);
        r.write(at + pCommandersKilled, &s.commanders_killed, 2);
        r.write(at + pCommandersLost, &s.commanders_lost, 2);
        r.write(at + pFlags, &s.flags, 2);
        r.write(at + pSharesVision, s.shares_vision.data(), 10);
        // Idle slots get an empty range (first after last).
        r.w32(at + pFirst, L.units + static_cast<u32>(playing ? s.first_unit : 1) * kUnitSize);
        r.w32(at + pLast, L.units + static_cast<u32>(playing ? s.last_unit : 0) * kUnitSize);
    }

    // Units.
    std::vector<sim::Unit> units(kMaxUnits + 2);
    std::vector<sim::UnitTickState> tstate(kMaxUnits + 2);
    sim::CombatEnvironment cenv;
    cenv.empty_type = &empty_type;
    cenv.empty_tick_type = &empty_tick;
    World world;
    sim::Combat combat{units, tstate, players, cenv, world};
    world.units = &units;
    world.ticks = &tstate;
    world.players = &players;
    world.combat = &combat;
    world.hm = &hm;
    const u32 rng0 = static_cast<u32>(g()) | 1u;
    r.w32(kBattleRngState, rng0);
    world.rng.set_state(rng0);
    std::vector<std::unique_ptr<sim::UnitScriptHost>> hosts(units.size());
    std::vector<u32> remote_script(units.size(), 0), remote_state(units.size(), 0);
    std::vector<int> present;
    auto unit_addr = [&](int i) { return L.units + static_cast<u32>(i) * kUnitSize; };
    for (int i = 1; i <= kMaxUnits; ++i) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
        u.id = static_cast<std::uint16_t>(i);
        t.owner = (i - 1) / per;
        if (t.owner > 2 || g() % 6 == 0) continue;  // empty slot
        present.push_back(i);
        ++players[static_cast<std::size_t>(t.owner)].alive_count;
    }
    for (int p = 0; p < 3; ++p) {
        sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        if (g() % 4 == 0) s.alive_count = 1;  // so that a death empties the player
        r.write(L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize + pAlive, &s.alive_count, 2);
    }
    for (int i : present) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
        const int k = static_cast<int>(g() % ntypes);
        LoadedType& lt = types[static_cast<std::size_t>(pick[static_cast<std::size_t>(k)])];
        t.type_index = static_cast<std::uint16_t>(k + 1);
        t.tick_type = &lt.tick_info;
        u.type = &lt.info;
        u.pos = {static_cast<i32>((g() % 9000 + 300) << 16), static_cast<i32>(g() % 3 == 0 ? 0 : (g() % 255) << 16),
                 static_cast<i32>((g() % 9000 + 300) << 16)};
        u.heading = static_cast<std::uint16_t>(g());
        u.hit_points = static_cast<std::int16_t>(static_cast<i32>(g() % (lt.info.max_damage + 50)) - (g() % 8 == 0 ? 100 : 0));
        u.flags = sim::unit_flags::kAlive | (g() % 2 ? 0x30u : 0u) | (g() % 12 == 0 ? 0x4000u : 0u);
        u.activate_flags = static_cast<std::uint8_t>(g() % 2 ? sim::unit_flags::kArmored : 0);
        u.build_fraction_left = g() % 4 == 0 ? 0.5f : 0.0f;
        t.has_motion = g() % 2;
        t.damaged_timer = static_cast<std::uint8_t>(g() % 4);
        t.transfer_holdoff = g() % 4;
        t.last_damage_kind = static_cast<std::uint8_t>(g() % 12);
        t.prev_health_percent = static_cast<std::uint8_t>(g() % 101);
        t.health_percent = t.prev_health_percent;
        t.kill_count = static_cast<std::uint16_t>(g() % 40);
        t.last_attacker_slot = static_cast<std::uint8_t>(g() % 11);
        t.last_attacker = g() % 2 ? &units[static_cast<std::size_t>(present[g() % present.size()])] : nullptr;
        t.metal_make = static_cast<float>(g() % 100000) / 9.0f;
        const u32 at = unit_addr(i);
        const u32 owner_at = L.gs + 0x1B63 + static_cast<u32>(t.owner) * kPlayerSize;
        const std::uint8_t owner_slot = static_cast<std::uint8_t>(t.owner);
        r.w32(at + uMotion, t.has_motion ? 0x7E000000u | static_cast<u32>(i) : 0u);
        r.write(at + uHeading, &u.heading, 2);
        r.write(at + uPosX, &u.pos[0], 4);
        r.write(at + uPosY, &u.pos[1], 4);
        r.write(at + uPosZ, &u.pos[2], 4);
        r.w32(at + uDef, lt.remote_def);
        r.w32(at + uOwner, owner_at);
        r.write(at + uOwnerSlot, &owner_slot, 1);
        r.w32(at + uResourcePlayer, owner_at);
        r.write(at + uDefIndex, &t.type_index, 2);
        r.write(at + uId, &u.id, 2);
        r.write(at + uKills, &t.kill_count, 2);
        r.write(at + uMetalMake, &t.metal_make, 4);
        r.w32(at + uLastAttacker, t.last_attacker ? unit_addr(t.last_attacker->id) : 0u);
        r.write(at + uLastAttackerSlot, &t.last_attacker_slot, 1);
        r.write(at + uLastDamageKind, &t.last_damage_kind, 1);
        r.write(at + uHealth, &t.health_percent, 1);
        r.write(at + uPrevHealth, &t.prev_health_percent, 1);
        r.write(at + uDamagedTimer, &t.damaged_timer, 1);
        r.w32(at + uHoldoff, t.transfer_holdoff);
        r.write(at + uBuildLeft, &u.build_fraction_left, 4);
        r.write(at + uHp, &u.hit_points, 2);
        r.write(at + uActivate, &u.activate_flags, 1);
        r.w32(at + uFlags, u.flags);
        u32 state = 0;
        if (!r.call(kBuildState, 0, {lt.remote_model, lt.remote_cob, at}, &state) || !state) { std::printf("build state failed\n"); return false; }
        const u32 so = L.heap + (64u << 20) + static_cast<u32>(i) * 0x4000;
        std::vector<std::uint8_t> sz(0x4000, 0);
        r.write(so, sz.data(), sz.size());
        r.w32(so + 0x00, kUnitVtable);
        r.w32(so + 0x04, 30);
        r.w32(so + 0x08, lt.remote_cob);
        r.w32(so + 0x10, so + 0x600);
        r.w32(so + 0x14, so + 0x1000);
        r.w32(so + 0x540, state);
        r.w32(at + uScript, so);
        r.w32(at + uState, state);
        remote_script[static_cast<std::size_t>(i)] = so;
        remote_state[static_cast<std::size_t>(i)] = state;
        u.pose.build(lt.pose_model, lt.cob.program.piece_names);
        hosts[static_cast<std::size_t>(i)] = std::make_unique<sim::UnitScriptHost>(u, world);
        u.script = std::make_unique<sim::ScriptInstance>(lt.cob.program, *hosts[static_cast<std::size_t>(i)], 30);
        u.script->set_step_limit(50000000);
    }
    for (int i : present) {
        r.write(L.names, "Create", 7);
        if (!r.call(kCallNamed, remote_script[static_cast<std::size_t>(i)], {L.names, 0, 1})) { std::printf("Create failed\n"); return false; }
        units[static_cast<std::size_t>(i)].script->call("Create", nullptr, true);
    }
    ++st.scenarios;

    sim::UnitTickEnvironment env;
    std::int16_t camera = 0;
    i32 env_live = 0;
    auto norm_ptr = [&](u32& w) {
        if (w >= L.units && w < L.units + (kMaxUnits + 2) * kUnitSize) {
            const u32 off = (w - L.units) % kUnitSize, idx = (w - L.units) / kUnitSize;
            if (off == 0) w = 0x40000000u | idx;
            else if (off == uResource) w = 0x41000000u | idx;
            else if (off == uPosX) w = 0x42000000u | idx;
        } else if (w >= L.gs + 0x1B63 && w < L.gs + 0x1B63 + 10 * kPlayerSize && (w - L.gs - 0x1B63) % kPlayerSize == 0) {
            w = 0x20000000u | ((w - L.gs - 0x1B63) / kPlayerSize);
        } else if (w >= L.heap + (64u << 20) && w < L.heap + (64u << 20) + 0x4000u * (kMaxUnits + 2) && (w - L.heap - (64u << 20)) % 0x4000 == 0) {
            w = 0x40000000u | ((w - L.heap - (64u << 20)) / 0x4000);
        }
    };
    // Zero what the two sides cannot agree on: undefined ECX, stack addresses, packet bytes past the size.
    auto normalise = [&](std::vector<u32>& log, bool remote) {
        for (std::size_t i = 0; i + 1 < log.size();) {
            const Stub* s = stub_by_id(log[i]);
            if (!s) break;
            if (remote) {
                if (log[i] != 6 && (log[i] < 10 || log[i] > 13)) log[i + 1] = 0;
                if (log[i] == 23) log[i + 2] = 0;  // the mission type byte read from an uninitialised stack slot
            }
            if (log[i] == 16) {
                log[i + 3] = 0;
                const u32 size = log[i + 4];
                if (size == 4) log[i + 2] = log[i + 5] = log[i + 6] = log[i + 7] = 0;
                std::uint8_t bytes[12];
                std::memcpy(bytes, &log[i + 5], 12);
                for (u32 b = size < 12 ? size : 12; b < 12; ++b) bytes[b] = 0;
                std::memcpy(&log[i + 5], bytes, 12);
            }
            i += 2 + static_cast<std::size_t>(s->args + s->extra);
        }
    };
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
        for (auto& w : log) norm_ptr(w);
        normalise(log, true);
        std::vector<u32> mine = world.log;
        world.log.clear();
        normalise(mine, false);
        if (log != mine) {
            bad("subsystem call log differs");
            for (int i : present)
                std::printf("    unit %d: %s\n", i, tstate[static_cast<std::size_t>(i)].tick_type ? tstate[static_cast<std::size_t>(i)].tick_type->name.c_str() : "?");
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
        for (int p = 0; p < 10; ++p) {
            const sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
            std::vector<std::uint8_t> rp(kPlayerSize);
            r.read(L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize, rp.data(), rp.size());
            auto f16 = [&](u32 off) { std::uint16_t v; std::memcpy(&v, &rp[off], 2); return v; };
            if (f16(pUnitsKilled) != s.units_killed || f16(pUnitsLost) != s.units_lost || f16(pCommandersKilled) != s.commanders_killed ||
                f16(pCommandersLost) != s.commanders_lost || f16(pAlive) != s.alive_count || f16(pFlags) != s.flags)
                bad("player " + std::to_string(p) + " counters");
        }
        for (int i : present) {
            sim::Unit& u = units[static_cast<std::size_t>(i)];
            const sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
            const u32 at = unit_addr(i);
            std::vector<std::uint8_t> ru(kUnitSize);
            r.read(at, ru.data(), ru.size());
            auto f8 = [&](u32 off) { return ru[off]; };
            auto f16 = [&](u32 off) { std::uint16_t v; std::memcpy(&v, &ru[off], 2); return v; };
            auto f32 = [&](u32 off) { u32 v; std::memcpy(&v, &ru[off], 4); return v; };
            const std::string un = "unit " + std::to_string(i) + " ";
            if (f32(uFlags) != u.flags) bad(un + "flags");
            if (f16(uDefIndex) != t.type_index) bad(un + "type index");
            if (static_cast<std::int16_t>(f16(uHp)) != u.hit_points) bad(un + "hit points");
            if (f16(uKills) != t.kill_count) bad(un + "kill count");
            u32 la = f32(uLastAttacker);
            norm_ptr(la);
            if (la != (t.last_attacker ? World::tok(*t.last_attacker) : 0u) || f8(uLastAttackerSlot) != t.last_attacker_slot)
                bad(un + "last attacker");
            if (f8(uLastDamageKind) != t.last_damage_kind) bad(un + "last damage kind");
            if (f32(uMetalMake) != fbits(t.metal_make)) bad(un + "metal make");
            if (f8(uHealth) != t.health_percent || f8(uPrevHealth) != t.prev_health_percent) bad(un + "health percent");
            if (f8(uDamagedTimer) != t.damaged_timer || f32(uHoldoff) != t.transfer_holdoff) bad(un + "timers");
            if (f8(uActivate) != u.activate_flags || f8(uScriptFlags) != u.script_flags) bad(un + "activate/script flags");
            if (f16(uNetDirty) != u.net_dirty) bad(un + "net dirty");
            if ((f32(uScript) != 0) != (u.script != nullptr) || (f32(uMotion) != 0) != t.has_motion) bad(un + "script/motion presence");
            if (!u.script) continue;  // dead
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
    auto alive = [&](int i) { return (units[static_cast<std::size_t>(i)].flags & sim::unit_flags::kAlive) != 0; };
    auto any_unit = [&]() { return present[g() % present.size()]; };
    auto fail = [&](const std::string& what) {
        std::printf("scenario %u: the original failed during %s\n", seed, what.c_str());
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
        cenv.local_player = static_cast<std::uint8_t>(g() % 4);
        cenv.local_view = static_cast<std::uint8_t>(g() % 4);
        cenv.game_mode = static_cast<i32>(g() % 3);
        cenv.ai_difficulty = static_cast<i32>(g() % 3);
        cenv.fog_of_war = g() % 2;
        world.kill_flash = g() % 2;
        world.pay_result = g() % 4 ? 1u : 0u;
        r.w32(L.gs + 0x38A47, env.game_tick);
        r.w32(L.gs + 0x37EE2, env.wind_changed ? 1u : 0u);
        r.write(L.gs + 0x37ED8, &env.wind_heading, 2);
        r.w32(L.gs + 0x37EDA, static_cast<u32>(env.wind_speed));
        r.w32(L.schema + 0xD4C, env.water_damage_enabled ? 1u : 0u);
        r.w32(L.schema + 0xD50, static_cast<u32>(env.water_damage));
        r.write(L.gs + 0x1427F, &env.sea_level, 1);
        r.w32(L.gs + 0x2A44, env.broadcast_unit_states ? 1u : 0u);
        r.w32(L.gs + 0x14373, 0);
        r.write(L.gs + gLocalPlayer, &cenv.local_player, 1);
        r.write(L.gs + gLocalView, &cenv.local_view, 1);
        r.w32(L.gs + gGameMode, static_cast<u32>(cenv.game_mode));
        r.w32(L.gs + gDifficulty, static_cast<u32>(cenv.ai_difficulty));
        const std::uint8_t map_flags = cenv.fog_of_war ? 2 : 0, flash = world.kill_flash ? 0x80 : 0;
        r.write(L.gs + gMapFlags, &map_flags, 1);
        r.write(L.gs + gFlash, &flash, 1);
        r.w32(L.ret_val, world.pay_result);

        // A few random combat operations, each checked on its own.
        const int nops = static_cast<int>(g() % 4);
        for (int op = 0; op < nops; ++op) {
            const int kind_of_op = static_cast<int>(g() % 10);
            std::string what;
            if (kind_of_op < 4) {  // damage
                const int ti = any_unit();
                const int ai = g() % 3 == 0 ? 0 : any_unit();
                const i32 kinds[] = {0, 1, 1, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 0x50, 0x70};
                const i32 kind = kinds[g() % (sizeof kinds / sizeof kinds[0])];
                const i32 amount = g() % 10 == 0 ? static_cast<i32>(g() % 70000) - 5000 : static_cast<i32>(g() % 600);
                const i32 heading = static_cast<i32>(g());
                sim::Unit* a = ai ? &units[static_cast<std::size_t>(ai)] : nullptr;
                sim::apply_damage(combat, a, units[static_cast<std::size_t>(ti)], amount, kind, heading);
                if (!r.call(kApplyDamage, 0, {ai ? unit_addr(ai) : 0u, unit_addr(ti), static_cast<u32>(amount), static_cast<u32>(kind),
                                              static_cast<u32>(heading)}))
                    return fail("damage " + std::to_string(ti) + " by " + std::to_string(ai) + " kind " + std::to_string(kind));
                what = "damage " + std::to_string(ti) + " by " + std::to_string(ai) + " amount " + std::to_string(amount) + " kind " +
                       std::to_string(kind);
            } else if (kind_of_op < 6) {  // repair / heal
                const int ti = any_unit();
                const int wi = g() % 2 ? ti : any_unit();
                const float rate = g() % 5 == 0 ? static_cast<float>(g() % 1000) / 3.0f : static_cast<float>(g() % 64) * 8.0f / 30.0f;
                sim::repair_heal(combat, units[static_cast<std::size_t>(wi)], units[static_cast<std::size_t>(ti)], rate);
                if (!r.call(kRepairHeal, 0, {unit_addr(wi), unit_addr(ti), fbits(rate)})) return fail("heal");
                what = "heal " + std::to_string(ti) + " by " + std::to_string(wi);
            } else if (kind_of_op < 8) {  // kill
                const int ui = any_unit();
                if (!alive(ui)) continue;
                const i32 kind = static_cast<i32>(g() % 12);
                if (g() % 2) units[static_cast<std::size_t>(ui)].hit_points = static_cast<std::int16_t>(-static_cast<i32>(g() % 500));
                const std::int16_t hp = units[static_cast<std::size_t>(ui)].hit_points;
                r.write(unit_addr(ui) + uHp, &hp, 2);
                sim::kill_unit(combat, units[static_cast<std::size_t>(ui)], kind);
                if (!r.call(kKillUnit, 0, {unit_addr(ui), static_cast<u32>(kind)})) return fail("kill kind " + std::to_string(kind));
                ++st.deaths;
                what = "kill " + std::to_string(ui) + " kind " + std::to_string(kind);
            } else if (kind_of_op == 8) {  // damage packet from the network
                sim::DamagePacket p{};
                p[0] = 0x0B;
                const int ti = g() % 10 == 0 ? 0 : any_unit(), ai = g() % 3 == 0 ? 0 : any_unit();
                p[1] = static_cast<std::uint8_t>(ti);
                p[3] = static_cast<std::uint8_t>(ai);
                const u32 amount = g() % 4 == 0 ? static_cast<u32>(g()) : g() % 400;
                p[5] = static_cast<std::uint8_t>(amount);
                p[6] = static_cast<std::uint8_t>(amount >> 8);
                p[7] = static_cast<std::uint8_t>(g());
                p[8] = static_cast<std::uint8_t>(g() % 12);
                sim::apply_damage_packet(combat, p);
                r.write(L.names, p.data(), p.size());
                if (!r.call(kApplyDamagePacket, 0, {L.names})) return fail("damage packet");
                what = "damage packet";
            } else {  // kill packet from the network
                const int ui = any_unit();
                if (!alive(ui)) continue;
                sim::KilledPacket p{};
                p[0] = 0x0C;
                p[1] = static_cast<std::uint8_t>(ui);
                const i32 net = g() % 5 == 0 ? -1 : static_cast<i32>(g() % 6);
                std::memcpy(&p[3], &net, 4);
                p[7] = static_cast<std::uint8_t>(g() % 3 == 0 ? 0 : any_unit());
                p[9] = static_cast<std::uint8_t>(g() % 4 == 0 ? 0 : g() % 3 == 0 ? 0x80 + g() % 128 : 1 + g() % 100);
                const std::uint8_t types_[] = {0x10, 0x30, 0x40, 0x50, 0x60, 0x70, 0x00, 0x20, 0x90, 0xB0};
                p[10] = static_cast<std::uint8_t>(types_[g() % 10] | (g() % 3 == 0 ? 0 : g() % 4));
                const bool skip = g() % 2;
                sim::apply_killed_packet(combat, p, skip);
                r.write(L.names, p.data(), p.size());
                if (!r.call(kApplyKilledPacket, 0, {L.names, skip ? 1u : 0u})) return fail("kill packet");
                ++st.deaths;
                what = "kill packet";
            }
            ++st.ops;
            if (engine_faulted()) return true;
            if (!compare(what + " tick " + std::to_string(tick))) return true;
        }

        // One unit pass.
        env_live = sim::run_unit_pass(units, tstate, players, env, world, camera);
        if (engine_faulted()) return true;
        if (!r.call(kUnitPass, 0, {})) return fail("unit pass");
        ++st.ticks;
        if (!compare("tick " + std::to_string(tick))) return true;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: combat_harness <TotalA.exe> <game_dir> [scenarios] [ticks]\n");
        return 2;
    }
    const int scenarios = argc > 3 ? std::atoi(argv[3]) : 50;
    const int ticks = argc > 4 ? std::atoi(argv[4]) : 200;
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
    std::vector<LoadedType> types;
    int skipped_no_killed = 0;
    for (std::size_t i = 1; i < defs.units.size(); ++i) {
        const auto& d = defs.units[i];
        LoadedType t;
        t.def = &d;
        std::string err;
        if (!vfs.read("objects3d\\" + d.object_name + ".3DO", t.model_file) || !vfs.read("scripts\\" + d.unit_name + ".COB", t.cob_file))
            continue;
        const auto m = ta::load_3do(t.model_file, true);
        if (!m.root || !ta::load_cob(t.cob_file, t.cob, err)) continue;
        // Without a Killed function the original's wreck level is an uninitialised stack value.
        if (t.cob.program.find_function("Killed") < 0) {
            ++skipped_no_killed;
            continue;
        }
        t.pose_model = ta::to_pose_model(m.root.get());
        t.info.max_damage = d.max_damage ? d.max_damage : 1;
        t.info.model_max_y = d.model_max_y;
        t.tick_info.wind_generator = d.wind_generator;
        t.tick_info.heal_time = static_cast<std::uint16_t>(d.heal_time);
        t.tick_info.type_flags = d.type_flags;
        t.tick_info.name = d.unit_name;
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
    std::printf("unit types %zu (%d without a Killed function left out)\n", types.size(), skipped_no_killed);
    std::printf("scenarios %zu, unit passes %zu, combat operations %zu, kills %zu, mismatching scenarios %zu, engine faults %zu, "
                "original failures %zu\n",
                st.scenarios, st.ticks, st.ops, st.deaths, st.mismatches, st.faults, st.failures);
    return st.mismatches || st.failures ? 1 : 0;
}
