// Differential tester for the projectile system (sim/projectile.h) inside a
// small battle. Like combat_harness it builds, in a suspended TotalA.exe,
// three players with units of random real types (models, scripts, object
// states) on the Moon Quartet heights, plus the projectile table, map cells
// with unit occupancy and features, feature types and animation slots, and the
// real weapon definitions (with random variations of their flags and numbers
// for coverage). Each step it launches shots with the original creation
// routines (directed, ballistic, ownerless, from motion), assigns chase
// targets, feeds detonate packets, kills units, runs the unit pass and the
// projectile update, and compares the whole projectile table, the camera
// follow state, the stub call log (sounds, effects, feature damage, network
// packets), every touched unit and player field, scripts, poses and the RNG.
//
// Usage: projectile_harness <TotalA.exe> <game_dir> [scenarios] [ticks] [first seed]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/combat.h"
#include "sim/orders.h"
#include "sim/fire.h"
#include "sim/pose.h"
#include "sim/projectile.h"
#include "sim/rng.h"
#include "sim/script_vm.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"
#include "ta/cob.h"
#include "ta/hpi.h"
#include "ta/model3do.h"
#include "ta/tnt.h"
#include "ta/unitdefs.h"
#include "ta/weapondefs.h"

namespace {

using harness::u32;
using i32 = std::int32_t;
using u16 = std::uint16_t;
using u8 = std::uint8_t;

constexpr u32 kGameStatePtr = 0x00511DE8, kBattleRngState = 0x0051FC88;
constexpr u32 kRelocate = 0x004CB4C0, kNegateXZ = 0x004CB590, kBuildState = 0x0045A950;
constexpr u32 kCallNamed = 0x004B0940, kUnitPass = 0x0048AD30, kKillUnit = 0x004864B0;
constexpr u32 kUpdate = 0x0049B720, kFireDirected = 0x0049C9C0, kFireBallistic = 0x0049CDE0, kSpawnMotion = 0x0049D000,
              kSpawnFree = 0x0049DF10, kDetonateMatching = 0x0049AF90;
// The weapon fire routines (unit, slot, target unit, &target position).
constexpr u32 kFireTurret = 0x0049D580, kFireVertical = 0x0049DB70, kFireLineOfSight = 0x0049D9C0, kFireDropped = 0x0049DD60;
constexpr u32 kUnitVtable = 0x004FD698;
constexpr u32 kUnitSize = 0x118, kPlayerSize = 0x14B, kProjSize = 0x6B, kWeaponSize = 0x115, kCellSize = 13;
constexpr int kMaxUnits = 16, kFeatureTypes = 12, kAnimSlots = 24;
constexpr i32 kRegionX = 100, kRegionZ = 100, kRegionSize = 40;  // cells

enum class StubRet { Zero, Variable, Silent };
// Subsystems replaced by stubs: address, argument count, log id, packet dwords logged from
// argument 1, argument whose three dwords are logged (-1 none), pointer arguments to
// normalise (bit 31 = ECX), pointer arguments zeroed before comparing, return.
struct Stub {
    u32 address;
    int args;
    int id;
    int extra = 0;
    int deref = -1;
    u32 ptrs = 0;
    u32 zero = 0;
    StubRet ret = StubRet::Zero;
};
constexpr u32 A0 = 1, A1 = 2, A3 = 8, ECXP = 0x80000000u;
const Stub kStubs[] = {
    {0x0049E1A0, 1, 1, 0, -1, A0},            // weapons(unit)
    {0x0043B7C0, 1, 4, 0, -1, A0},            // primary orders(unit)
    {0x0043BAD0, 1, 5, 0, -1, A0},            // secondary orders(unit)
    {0x0043DD20, 1, 6, 0, -1, A0 | ECXP},     // motion(unit) [thiscall on the motion object]
    {0x0048A870, 1, 7, 0, -1, A0},            // settle height(unit)
    {0x0048B710, 1, 9, 0, -1, A0},            // broadcast player units(player)
    {0x00480EB0, 2, 10, 0, -1, ECXP},         // script emit sfx(piece, type) [thiscall]
    {0x00481140, 2, 11, 0, -1, ECXP},         // script explode(piece, type)
    {0x00481340, 3, 12, 0, -1, ECXP},         // script attach(unit, piece, mode)
    {0x004813B0, 1, 13, 0, -1, ECXP},         // script drop(unit)
    {0x0047F780, 3, 14, 0, -1, A0},           // speech(unit, event, 0)
    {0x0041C110, 1, 15, 0, -1, A0},           // HUD dirty(unit)
    {0x00451DF0, 3, 16, 9, -1, 0, A1},        // network send(net id, packet, size) + 36 packet bytes
    {0x0047DAC0, 2, 17, 0, -1, A0},           // yard open(unit, open)
    {0x00406F80, 3, 20, 0, -1, A0 | A1},      // retarget(attacker, victim, amount)
    {0x00494FF0, 1, 21},                      // statistics activity(kind)
    {0x00438760, 1, 22},                      // mission type by name(name) [thiscall on a stack object]
    {0x0043A0C0, 6, 23},                      // mission order constructor(type, 0, 0, amount, 0, 0) [thiscall]
    {0x0043ACB0, 2, 24, 0, -1, A0},           // prepend order(unit, order)
    {0x00401180, 2, 25, 0, -1, A0, 0, StubRet::Variable},  // pay energy(resource slot, float amount)
    {0x00482910, 4, 26, 0, -1, A0},           // reveal dead unit(&pos, sight, radar, 60)
    {0x004904C0, 1, 27, 0, -1, A0},           // victory: unit destroyed(unit) [thiscall]
    {0x00439EB0, 2, 28, 0, -1, A0},           // clear orders(unit, 1)
    {0x0047F8C0, 1, 29, 0, -1, A0},           // forget speech(unit)
    {0x00480250, 2, 30, 0, -1, A0},           // leave group(unit, -1)
    {0x0048AAC0, 4, 32, 0, -1, A0},           // detach from carrier(unit, 0, -1, 1)
    {0x0047CBD0, 1, 33, 0, -1, A0},           // clear footprint(unit)
    {0x00482090, 1, 34, 0, -1, A0},           // forget fog(unit)
    {0x004948B0, 2, 35},                      // kill flash(attacker slot, victim state)
    {0x00486360, 3, 37, 0, -1, A0},           // spawn wreck(unit, level, burn)
    // 0x00489740 (release the links pointing at a dying unit) runs for real: sim::release_referrers.
    {0x00450380, 1, 39},                      // network: player out of units(net id)
    {0x0047BD70, 1, 40, 0, -1, A0},           // AI: player out of units(player)
    {0x00491D70, 1, 41},                      // commander death: close menus(1)
    {0x00486F10, 1, 42},                      // commander death: kill the team(slot)
    {0x0047F300, 3, 60, 0, 1, 0, A1},         // sound(index, &pos, broadcast)
    {0x00472810, 2, 61, 0, 0, 0, A0},         // smoke puff(&pos, 9)
    {0x00420A30, 4, 62, 0, 0, 0, A0},         // explosion(&pos, sequence, 0, water)
    {0x004729D0, 2, 63, 0, 0, 0, A0},         // launch smoke(&pos, 9)
    {0x0041C640, 3, 64},                      // camera shake(x, y, duration)
    {0x004244B0, 4, 65, 0, -1, A0 | A3},      // feature damage(cell, x, z, weapon)
    {0x00485E30, 1, 50, 0, -1, 0, 0, StubRet::Silent},  // script object destructor [thiscall]
    {0x0045AAA0, 1, 51, 0, -1, 0, 0, StubRet::Silent},  // free object state
    {0x0043DD10, 0, 52, 0, -1, 0, 0, StubRet::Silent},  // motion destructor [thiscall]
    {0x004B4F20, 0, 53, 0, -1, 0, 0, StubRet::Silent},  // free [cdecl]
};

const Stub* stub_by_id(u32 id) {
    for (const Stub& s : kStubs)
        if (static_cast<u32>(s.id) == id) return &s;
    return nullptr;
}
std::size_t entry_words(const Stub& s) { return 2 + static_cast<std::size_t>(s.args + s.extra + (s.deref >= 0 ? 3 : 0)); }

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8_(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
};

// Logs {id, ECX, args..., packet dwords..., dereferenced dwords...} at *log_ptr, then returns with `ret 4*args`.
void emit_stub(Emitter& e, const Stub& s, u32 log_ptr_addr, u32 ret_addr) {
    const bool cdecl_ret = s.address == 0x004B4F20;
    if (s.ret != StubRet::Silent) {
        e.u8_(0xA1); e.u32le(log_ptr_addr);                          // mov eax,[log_ptr]
        e.u8_(0xC7); e.u8_(0x00); e.u32le(static_cast<u32>(s.id));   // mov dword [eax],id
        e.u8_(0x89); e.u8_(0x48); e.u8_(0x04);                        // mov [eax+4],ecx
        for (int i = 0; i < s.args; ++i) {
            e.u8_(0x8B); e.u8_(0x54); e.u8_(0x24); e.u8_(static_cast<std::uint8_t>(4 + 4 * i));  // mov edx,[esp+4+4i]
            e.u8_(0x89); e.u8_(0x50); e.u8_(static_cast<std::uint8_t>(8 + 4 * i));              // mov [eax+8+4i],edx
        }
        int at = 8 + 4 * s.args;
        auto copy_from = [&](int arg, int n) {
            e.u8_(0x8B); e.u8_(0x54); e.u8_(0x24); e.u8_(static_cast<std::uint8_t>(4 + 4 * arg));  // mov edx,[esp+4+4*arg]
            for (int k = 0; k < n; ++k) {
                e.u8_(0x8B); e.u8_(0x4A); e.u8_(static_cast<std::uint8_t>(4 * k));  // mov ecx,[edx+4k]
                e.u8_(0x89); e.u8_(0x48); e.u8_(static_cast<std::uint8_t>(at));     // mov [eax+at],ecx
                at += 4;
            }
        };
        if (s.extra) copy_from(1, s.extra);
        if (s.deref >= 0) copy_from(s.deref, 3);
        e.u8_(0x83); e.u8_(0x05); e.u32le(log_ptr_addr); e.u8_(static_cast<std::uint8_t>(at));  // add [log_ptr],at
    }
    if (s.ret == StubRet::Variable) {
        e.u8_(0xA1); e.u32le(ret_addr);  // mov eax,[ret]
    } else {
        e.u8_(0x33); e.u8_(0xC0);  // xor eax,eax
    }
    if (cdecl_ret || s.args == 0) {
        e.u8_(0xC3);
    } else {
        e.u8_(0xC2); e.u8_(static_cast<std::uint8_t>(4 * s.args)); e.u8_(0x00);
    }
}

struct Layout {
    u32 caller_pass = 0, caller_kill = 0, heap = 0, bump_ptr = 0, stubs = 0, log_ptr = 0, log_base = 0, ret_val = 0, gs = 0, units = 0,
        defs = 0, models = 0, cells = 0, schema = 0, names = 0, files = 0, pinfo = 0, empty_def = 0, proj = 0, wdefs = 0, dmaps = 0,
        fdefs = 0, fanims = 0, motion = 0, args = 0;
    i32 map_w = 0, map_h = 0;
};

// Remote unit / player / type / game state offsets.
enum : u32 {
    uMotion = 0x00, uSlots = 0x04, uHeading = 0x66, uPosX = 0x6A, uPosY = 0x6E, uPosZ = 0x72, uDef = 0x92, uOwner = 0x96, uScript = 0x9A,
    uState = 0x9E, uDefIndex = 0xA6, uId = 0xA8, uCloak = 0xB0, uKills = 0xB8, uNetDirty = 0xBA, uResource = 0xBC, uMetalMake = 0xD4,
    uResourcePlayer = 0xEC, uLastAttacker = 0xF0, uLastAttackerSlot = 0xF4, uLastDamageKind = 0xF5, uHealth = 0xF6, uPrevHealth = 0xF7,
    uDamagedTimer = 0xFA, uHoldoff = 0xFB, uOwnerSlot = 0xFF, uBuildLeft = 0x104, uHp = 0x108, uActivate = 0x10E, uScriptFlags = 0x10F,
    uFlags = 0x110,
    pActive = 0x00, pNetId = 0x04, pInfo = 0x27, pFirst = 0x67, pLast = 0x6B, pController = 0x73, pUnitsKilled = 0xFC,
    pUnitsLost = 0xFE, pCommandersKilled = 0x104, pCommandersLost = 0x106, pSharesVision = 0x129, pAlive = 0x144, pState = 0x146,
    pFlags = 0x149,
    dName = 0x20, dMinX = 0x15E, dMinY = 0x162, dMinZ = 0x166, dMaxX = 0x16A, dModelMaxY = 0x16E, dMaxZ = 0x172, dEnergyCost = 0x186,
    dMetalCost = 0x18A, dCob = 0x18E, dDamageModifier = 0x1AA, dWindGen = 0x1D2, dBuildTime = 0x1EA, dMaxDamage = 0x1FA,
    dHealTime = 0x200, dExplodeAs = 0x220, dSelfDestructAs = 0x224, dTypeFlags = 0x241, kDefSize = 0x249,
    gLocalPlayer = 0x2A42, gLocalView = 0x2A43, gNetFlags = 0x2A44, gProjCount = 0x141F3, gProjTable = 0x141F7, gFeatureAnims = 0x1420B,
    gFeatureCount = 0x14253, gGravity = 0x14263, gFeatureDefs = 0x1426F, gMapFlags = 0x14281, gSea = 0x1427F, gCamUnit = 0x142F3,
    gCamTrack = 0x142F7, gCamRest = 0x1433F, gCamRestTicks = 0x1434B, gWind = 0x37ECC, gDifficulty = 0x37EEE, gGameMode = 0x37EF6,
    gFlash = 0x37F06, gOptions = 0x37F2F, gCommanderNames = 0x37F5F, kSideSize = 0x232, sNoSeaTrigger = 0xD48,
};

u32 fbits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

bool setup(harness::Remote& r, Layout& L, const sim::HeightMap& hm, const std::vector<sim::MapCell>& base_cells) {
    L.heap = r.alloc(128u << 20);
    L.bump_ptr = r.alloc(0x1000);
    L.stubs = r.alloc(0x4000, PAGE_EXECUTE_READWRITE);
    L.log_ptr = r.alloc(0x1000);
    L.log_base = r.alloc(8u << 20);
    L.ret_val = r.alloc(0x1000);
    L.gs = r.alloc(0x40000);
    L.units = r.alloc(static_cast<SIZE_T>(65536) * kUnitSize);
    L.defs = r.alloc(0x10000);
    L.models = r.alloc(0x1000);
    L.map_w = hm.width();
    L.map_h = hm.height();
    L.cells = r.alloc(static_cast<SIZE_T>(hm.width()) * hm.height() * kCellSize);
    L.schema = r.alloc(0x1000);
    L.names = r.alloc(0x1000);
    L.files = r.alloc(32u << 20);
    L.pinfo = r.alloc(0x1000);
    L.empty_def = r.alloc(0x1000);
    L.proj = r.alloc(static_cast<SIZE_T>(sim::kMaxProjectiles) * kProjSize);
    L.wdefs = r.alloc(256 * kWeaponSize);
    L.dmaps = r.alloc(1u << 20);
    L.fdefs = r.alloc(0x10000);
    L.fanims = r.alloc(0x10000);
    L.motion = r.alloc(0x10000);
    L.args = r.alloc(0x1000);
    if (!L.heap || !L.stubs || !L.log_base || !L.gs || !L.units || !L.cells || !L.files || !L.pinfo || !L.empty_def || !L.proj || !L.wdefs ||
        !L.dmaps || !L.fdefs || !L.fanims || !L.motion || !L.args)
        return false;
    Emitter e;
    // Bump allocators: (tag, size), (size), and realloc (ptr, tag, size) for fresh blocks.
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
    for (const Stub& s : kStubs) {
        const u32 at = L.stubs + static_cast<u32>(e.b.size());
        emit_stub(e, s, L.log_ptr, L.ret_val);
        while (e.b.size() % 16) e.u8_(0xCC);
        if (!r.patch_jump(s.address, at)) return false;
    }
    // Callers that preset the stack word the original's kill routine reads uninitialised.
    const u32 in = L.ret_val + 0x10, out = L.ret_val + 0x14, kind_arg = L.ret_val + 0x18, unit_arg = L.ret_val + 0x1C;
    auto call_rel = [&](u32 target) {
        const u32 next = L.stubs + static_cast<u32>(e.b.size()) + 5;
        e.u8_(0xE8); e.u32le(target - next);
    };
    L.caller_pass = L.stubs + static_cast<u32>(e.b.size());
    e.u8_(0xA1); e.u32le(in);
    e.u8_(0x89); e.u8_(0x44); e.u8_(0x24); e.u8_(0xC0);
    call_rel(kUnitPass);
    e.u8_(0x8B); e.u8_(0x44); e.u8_(0x24); e.u8_(0xC0);
    e.u8_(0xA3); e.u32le(out);
    e.u8_(0xC3);
    while (e.b.size() % 16) e.u8_(0xCC);
    L.caller_kill = L.stubs + static_cast<u32>(e.b.size());
    e.u8_(0xA1); e.u32le(in);
    e.u8_(0x89); e.u8_(0x44); e.u8_(0x24); e.u8_(0xE4);
    e.u8_(0xFF); e.u8_(0x35); e.u32le(kind_arg);
    e.u8_(0xFF); e.u8_(0x35); e.u32le(unit_arg);
    call_rel(kKillUnit);
    e.u8_(0x8B); e.u8_(0x44); e.u8_(0x24); e.u8_(0xE4);
    e.u8_(0xA3); e.u32le(out);
    e.u8_(0xC3);
    while (e.b.size() % 16) e.u8_(0xCC);
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
    r.w32(L.gs + 0x1439B, L.empty_def);
    r.w32(L.gs + gProjTable, L.proj);
    r.w32(L.gs + gFeatureDefs, L.fdefs);
    r.w32(L.gs + gFeatureAnims, L.fanims);
    std::vector<std::uint8_t> cells(base_cells.size() * kCellSize, 0);
    for (std::size_t i = 0; i < base_cells.size(); ++i) {
        const sim::MapCell& c = base_cells[i];
        std::uint8_t* d = &cells[i * kCellSize];
        std::memcpy(d, &c.unit, 2);
        std::memcpy(d + 2, &c.unit2, 2);
        d[4] = c.height;
        d[5] = c.height_max;
        d[6] = c.height_min;
        std::memcpy(d + 8, &c.feature, 2);
        std::memcpy(d + 10, &c.feature_aux, 2);
        d[12] = c.flags;
    }
    return r.write(L.cells, cells.data(), cells.size());
}

void put_cell(harness::Remote& r, const Layout& L, i32 index, const sim::MapCell& c) {
    std::uint8_t d[kCellSize] = {};
    std::memcpy(d, &c.unit, 2);
    std::memcpy(d + 2, &c.unit2, 2);
    d[4] = c.height;
    d[5] = c.height_max;
    d[6] = c.height_min;
    std::memcpy(d + 8, &c.feature, 2);
    std::memcpy(d + 10, &c.feature_aux, 2);
    d[12] = c.flags;
    r.write(L.cells + static_cast<u32>(index) * kCellSize, d, kCellSize);
}

struct LoadedType {
    const ta::UnitDefRecord* def = nullptr;
    std::vector<std::uint8_t> model_file, cob_file;
    sim::PoseModel pose_model;
    ta::CobFile cob;
    sim::UnitTypeInfo info;
    sim::UnitTickTypeInfo tick_info;
    u32 remote_model = 0, remote_cob = 0, remote_def = 0;
};

// Per-unit weapon slot state (owned by the weapon system in the engine).
struct SlotState {
    const sim::WeaponType* weapon = nullptr;
    u16 heading = 0, pitch = 0;
    u32 aim_drop = 0;
};

struct World : sim::UnitWorld, sim::UnitTickSystems, sim::CombatSystems, sim::ProjectileSystems {
    std::vector<sim::Unit>* units = nullptr;
    std::vector<sim::UnitTickState>* ticks = nullptr;
    std::vector<sim::PlayerSlot>* players = nullptr;
    sim::Combat* combat = nullptr;
    sim::ProjectileWorld* pw = nullptr;
    const sim::HeightMap* hm = nullptr;
    const sim::MapGrid* map = nullptr;
    sim::BattleRng rng;
    std::vector<u32> log;
    u32 pay_result = 0;
    bool kill_flash = false;
    std::vector<std::array<SlotState, 3>> slots;
    std::vector<i32> motion_speeds;
    std::vector<sim::Vec3i> anim_pos;
    static u32 tok(const sim::Unit& u) { return 0x40000000u | u.id; }
    static u32 wtok(const sim::WeaponType& w) { return 0x5D000000u | w.index; }
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
        if (owner.active && (owner.controller == 1 || owner.controller == 2)) rec(16, 0, {0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    }
    std::uint32_t random_mod(std::uint32_t n) override { return rng.next_mod(n); }
    void emit_sfx(sim::Unit& u, i32 piece, i32 type) override { rec(10, tok(u), {static_cast<u32>(piece), static_cast<u32>(type)}); }
    void explode_piece(sim::Unit& u, i32 piece, i32 type) override { rec(11, tok(u), {static_cast<u32>(piece), static_cast<u32>(type)}); }
    void attach_unit(sim::Unit& u, i32 a, i32 b, i32 c) override {
        rec(12, tok(u), {static_cast<u32>(a), static_cast<u32>(b), static_cast<u32>(c)});
    }
    void drop_unit(sim::Unit& u, i32 a) override { rec(13, tok(u), {static_cast<u32>(a)}); }
    // UnitTickSystems
    void weapons(sim::Unit& u, u32&) override { rec(1, 0, {tok(u)}); }
    void damage(sim::Unit* a, sim::Unit& t, i32 amount, i32 kind, i32 heading) override {
        sim::apply_damage(*combat, a, t, amount, kind, heading);
    }
    void repair_heal(sim::Unit& w, sim::Unit& t, float rate) override { sim::repair_heal(*combat, w, t, rate); }
    void primary_orders(sim::Unit& u, u32&) override { rec(4, 0, {tok(u)}); }
    void secondary_orders(sim::Unit& u, u32&) override { rec(5, 0, {tok(u)}); }
    void motion(sim::Unit& u, u32&) override { rec(6, 0x7E000000u | u.id, {tok(u)}); }
    void settle_height(sim::Unit& u, u32&) override { rec(7, 0, {tok(u)}); }
    void killed(sim::Unit& u, std::uint8_t kind, u32& stale) override { sim::kill_unit(*combat, u, kind, stale); }
    void broadcast_player_units(int player, u32&) override { rec(9, 0, {0x20000000u | static_cast<u32>(player)}); }
    void camera_follow_step(u32&) override {}
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
        u32 d[9] = {};
        std::memcpy(d, p, static_cast<std::size_t>((std::min)(size, 36)));
        rec(16, 0, {static_cast<u32>(net), 0, static_cast<u32>(size), d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8]});
    }
    void reveal_dead_unit(sim::Unit& u) override {
        const auto height = static_cast<std::int16_t>(static_cast<u32>(u.type ? u.type->model_max_y : 0) >> 16);
        rec(26, 0, {0x42000000u | u.id, 0, static_cast<u32>(static_cast<i32>(height)), 60});
    }
    void victory_unit_destroyed(sim::Unit& u) override { rec(27, 0, {tok(u)}); }
    void clear_orders(sim::Unit& u) override { rec(28, 0, {tok(u), 1}); }
    void forget_speech(sim::Unit& u) override { rec(29, 0, {tok(u)}); }
    void leave_group(sim::Unit& u) override { rec(30, 0, {tok(u), 0xFFFFFFFFu}); }
    void remove_projectiles(sim::Unit& u) override { sim::remove_owner_bursts(*pw, u); }
    void detach_from_carrier(sim::Unit& u) override { rec(32, 0, {tok(u), 0, 0xFFFFFFFFu, 1}); }
    void clear_footprint(sim::Unit& u) override { rec(33, 0, {tok(u)}); }
    void forget_fog(sim::Unit& u) override { rec(34, 0, {tok(u)}); }
    void kill_counted(std::uint8_t slot, sim::Unit& v) override {
        if (kill_flash) rec(35, 0, {slot, owner_of(v).state});
    }
    void detonate(sim::Unit& u, bool self_destruct) override { sim::detonate_unit_death(*pw, u, self_destruct); }
    void spawn_wreck(sim::Unit& u, i32 level, bool burn) override { rec(37, 0, {tok(u), static_cast<u32>(level), burn ? 1u : 0u}); }
    void release_unit(sim::Unit& u) override { sim::release_referrers(u); }
    void release_motion(sim::Unit&) override {}
    void player_out_of_units(int) override {}
    void commander_died(int player) override {
        rec(41, 0, {1});
        rec(42, 0, {static_cast<u32>(player)});
    }
    // ProjectileSystems
    const sim::WeaponType* slot_weapon(const sim::Unit& u, int k) override { return u.weapons[static_cast<std::size_t>(k)].type; }
    std::uint16_t slot_fire_heading(const sim::Unit& u, int k) override { return u.weapons[static_cast<std::size_t>(k)].aim_heading; }
    i32 motion_speed(const sim::Unit& u) override { return motion_speeds[u.id]; }
    void play_sound(std::uint16_t sound, const sim::Vec3i& p) override {
        rec(60, 0, {sound, 0, 0, static_cast<u32>(p[0]), static_cast<u32>(p[1]), static_cast<u32>(p[2])});
    }
    void spawn_smoke(const sim::Vec3i& p) override { rec(61, 0, {0, 9, static_cast<u32>(p[0]), static_cast<u32>(p[1]), static_cast<u32>(p[2])}); }
    void spawn_muzzle_smoke(const sim::Vec3i& p) override {
        rec(63, 0, {0, 9, static_cast<u32>(p[0]), static_cast<u32>(p[1]), static_cast<u32>(p[2])});
    }
    void spawn_explosion(const sim::Vec3i& p, const sim::WeaponType& w, bool water_art, bool in_water) override {
        rec(62, 0, {0, 0x5E000000u | (static_cast<u32>(w.index) << 1) | (water_art ? 1u : 0u), 0, in_water ? 1u : 0u, static_cast<u32>(p[0]),
                    static_cast<u32>(p[1]), static_cast<u32>(p[2])});
    }
    void camera_shake(i32 x, i32 y, i32 d) override { rec(64, 0, {static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(d)}); }
    void damage_feature(sim::MapCell& c, i32 x, i32 z, const sim::WeaponType& w) override {
        rec(65, 0, {0x5C000000u | static_cast<u32>(&c - map->cells.data()), static_cast<u32>(x), static_cast<u32>(z), wtok(w)});
    }
    sim::Vec3i feature_anim_pos(std::uint16_t slot) override { return slot < anim_pos.size() ? anim_pos[slot] : sim::Vec3i{}; }
};

struct Stats {
    std::size_t scenarios = 0, ticks = 0, updates = 0, directed = 0, ballistic = 0, free_shots = 0, motion_shots = 0, matches = 0,
                chases = 0, kills = 0, explosions = 0, damage_packets = 0, feature_hits = 0, max_live = 0, live_sum = 0, mismatches = 0,
                failures = 0, faults = 0, both_crashed = 0, full_table = 0, water_hits = 0, damage_events = 0, fire_routines = 0;
    std::size_t kind[12] = {};  // shot-steps by kind (see the report)
    std::size_t packets[256] = {};
};

std::string random_case(std::string s, std::mt19937& g) {
    for (char& ch : s)
        if (g() % 2) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : (ch >= 'A' && ch <= 'Z' ? ch + 32 : ch));
    return s;
}

sim::WeaponType weapon_type_from(const ta::WeaponDefRecord& d, int slot) {
    sim::WeaponType w;
    w.index = static_cast<std::uint8_t>(slot);
    w.flags = d.flags;
    w.velocity = d.weapon_velocity;
    w.start_velocity = d.start_velocity;
    w.acceleration = d.weapon_acceleration;
    w.range = d.range;
    w.weapon_timer = static_cast<u16>(d.weapon_timer);
    w.turn_rate = static_cast<u16>(d.turn_rate);
    w.burst = static_cast<u16>(d.burst);
    w.burst_rate = static_cast<u16>(d.burst_rate);
    w.spray_angle = static_cast<u16>(d.spray_angle);
    w.duration = static_cast<u16>(d.duration);
    w.random_decay = static_cast<u16>(d.random_decay);
    w.smoke_delay = static_cast<u16>(d.smoke_delay);
    w.flight_time = static_cast<u16>(d.flight_time);
    w.hold_time = static_cast<u16>(d.hold_time);
    w.area_of_effect = static_cast<u16>(d.area_of_effect);
    w.default_damage = static_cast<u16>(d.default_damage);
    w.edge_effectiveness = d.edge_effectiveness;
    w.shake_magnitude = d.shake_magnitude;
    w.shake_duration = d.shake_duration;
    w.accuracy = static_cast<u16>(d.accuracy);
    w.tolerance = static_cast<u16>(d.tolerance);
    w.pitch_tolerance = static_cast<u16>(d.pitch_tolerance);
    w.coverage = d.coverage;
    w.min_barrel_angle = d.min_barrel_angle;
    switch (d.fire) {
    case ta::WeaponFireFn::Turret: w.fire = sim::WeaponFireRoutine::Turret; break;
    case ta::WeaponFireFn::VLaunch: w.fire = sim::WeaponFireRoutine::VerticalLaunch; break;
    case ta::WeaponFireFn::LineOfSight: w.fire = sim::WeaponFireRoutine::LineOfSight; break;
    case ta::WeaponFireFn::Dropped: w.fire = sim::WeaponFireRoutine::Dropped; break;
    default: w.fire = sim::WeaponFireRoutine::None; break;
    }
    w.sound_start = static_cast<u16>(slot * 3);
    w.sound_hit = static_cast<u16>(slot * 3 + 1);
    w.sound_water = d.sound_water.empty() ? 0xFFFF : static_cast<u16>(slot * 3 + 2);
    if (d.has_damage_map) w.damage = d.damage_overrides;
    return w;
}

void write_weapon(harness::Remote& r, const Layout& L, const sim::WeaponType& w, u32 dmap) {
    std::vector<std::uint8_t> d(kWeaponSize, 0);
    auto put = [&](u32 off, const void* p, std::size_t n) { std::memcpy(&d[off], p, n); };
    put(0x64, &dmap, 4);
    put(0x68, &w.velocity, 4);
    put(0x6C, &w.start_velocity, 4);
    put(0x70, &w.acceleration, 4);
    const u32 seq = 0x5E000000u | (static_cast<u32>(w.index) << 1), wseq = seq | 1u;
    put(0x78, &seq, 4);
    put(0x7C, &wseq, 4);
    put(0xCC, &w.shake_magnitude, 4);
    put(0xD0, &w.shake_duration, 4);
    put(0xD4, &w.default_damage, 2);
    put(0xD6, &w.area_of_effect, 2);
    put(0xD8, &w.edge_effectiveness, 4);
    put(0xDC, &w.range, 4);
    put(0xE6, &w.weapon_timer, 2);
    put(0xE8, &w.turn_rate, 2);
    put(0xEA, &w.burst, 2);
    put(0xEC, &w.burst_rate, 2);
    put(0xEE, &w.spray_angle, 2);
    put(0xF0, &w.duration, 2);
    put(0xF2, &w.random_decay, 2);
    put(0xF4, &w.sound_start, 2);
    put(0xF6, &w.sound_hit, 2);
    put(0xF8, &w.sound_water, 2);
    put(0xFA, &w.smoke_delay, 2);
    put(0xFC, &w.flight_time, 2);
    put(0xFE, &w.hold_time, 2);
    put(0x10A, &w.index, 1);
    put(0x104, &w.accuracy, 2);
    put(0x106, &w.tolerance, 2);
    put(0x108, &w.pitch_tolerance, 2);
    put(0xE0, &w.coverage, 4);
    put(0xC8, &w.min_barrel_angle, 4);
    put(0x111, &w.flags, 4);
    r.write(L.wdefs + static_cast<u32>(w.index) * kWeaponSize, d.data(), d.size());
}

// Writes the damage tables ({begin, end} at +5/+9 of a small header; pairs of {name, damage}); returns their addresses.
std::vector<u32> write_damage_maps(harness::Remote& r, const Layout& L, const std::vector<sim::WeaponType>& ws) {
    std::vector<u32> out(256, 0);
    u32 at = L.dmaps;
    for (const auto& w : ws) {
        if (w.damage.empty()) continue;
        const u32 hdr = at;
        at += 16;
        const u32 pairs = at;
        at += static_cast<u32>(w.damage.size()) * 8;
        std::vector<u32> pr;
        for (const auto& [name, dmg] : w.damage) {
            r.write(at, name.c_str(), name.size() + 1);
            pr.push_back(at);
            pr.push_back(static_cast<u32>(dmg));
            at += (static_cast<u32>(name.size()) + 4) & ~3u;
        }
        r.write(pairs, pr.data(), pr.size() * 4);
        const u32 end = pairs + static_cast<u32>(w.damage.size()) * 8;
        r.w32(hdr + 5, pairs);
        r.w32(hdr + 9, end);
        out[w.index] = hdr;
    }
    return out;
}

bool run_scenario(harness::Remote& r, const Layout& L, std::vector<LoadedType>& types, const sim::HeightMap& hm,
                  const std::vector<sim::MapCell>& base_cells, const std::vector<sim::WeaponType>& pristine,
                  const std::vector<u32>& dmaps, std::vector<i32>& dirty, int ticks_to_run, unsigned seed, Stats& st) {
    // Cells changed by the previous scenario go back to the base map.
    for (i32 idx : dirty) put_cell(r, L, idx, base_cells[static_cast<std::size_t>(idx)]);
    dirty.clear();
    std::mt19937 g(seed);
    r.w32(L.bump_ptr, L.heap);
    std::vector<std::uint8_t> zero(0x40000, 0);
    r.write(L.gs + 0x1B63, zero.data(), 11 * kPlayerSize);
    r.write(L.units, zero.data(), (kMaxUnits + 2) * kUnitSize);
    r.write(L.gs + gCommanderNames, zero.data(), 4 * kSideSize);
    r.write(L.proj, zero.data(), sim::kMaxProjectiles * kProjSize);
    r.w32(L.gs + 0x14353, 0);
    r.w32(L.gs + gProjCount, 0);
    r.w32(L.gs + gCamTrack, 0);
    r.w32(L.gs + gCamUnit, 0);
    r.write(L.gs + gCamRest, zero.data(), 14);

    // Weapons: the real definitions, a few varied for coverage.
    std::vector<sim::WeaponType> weapons = pristine;
    const int ntypes = 3;
    std::vector<int> pick;
    for (int k = 0; k < ntypes; ++k) pick.push_back(static_cast<int>(g() % types.size()));
    std::vector<int> death_weapons;
    for (int k : pick) {
        const auto* d = types[static_cast<std::size_t>(k)].def;
        for (int s : {d->explode_as, d->self_destruct_as})
            if (s >= 0) death_weapons.push_back(s);
    }
    auto is_death_weapon = [&](int s) { return std::find(death_weapons.begin(), death_weapons.end(), s) != death_weapons.end(); };
    std::vector<int> loaded;
    for (int s = 0; s < 256; ++s)
        if (pristine[static_cast<std::size_t>(s)].velocity != 0 || pristine[static_cast<std::size_t>(s)].flags != 0) loaded.push_back(s);
    std::vector<int> used;
    for (int k = 0; k < 8; ++k) used.push_back(loaded[g() % loaded.size()]);
    constexpr u32 kFlagPool[] = {sim::weapon_flags::kLineOfSight, sim::weapon_flags::kBallistic, sim::weapon_flags::kBeam,
                                 sim::weapon_flags::kMeteor, sim::weapon_flags::kParalyzer, sim::weapon_flags::kDropped,
                                 sim::weapon_flags::kStartSmoke, sim::weapon_flags::kEndSmoke, sim::weapon_flags::kSoundTrigger,
                                 sim::weapon_flags::kGuidance, sim::weapon_flags::kTracks, sim::weapon_flags::kUnitsOnly,
                                 sim::weapon_flags::kGroundBounce, sim::weapon_flags::kWaterWeapon, sim::weapon_flags::kSmokeTrail,
                                 sim::weapon_flags::kSelfPropelled, sim::weapon_flags::kPropeller, sim::weapon_flags::kNoExplode,
                                 sim::weapon_flags::kBurnBlow, sim::weapon_flags::kTwoPhase, sim::weapon_flags::kCruise,
                                 sim::weapon_flags::kNoAutoRange, sim::weapon_flags::kInterceptor};
    for (int s : used) {
        if (is_death_weapon(s) || g() % 2) continue;
        sim::WeaponType& w = weapons[static_cast<std::size_t>(s)];
        const int nflips = 1 + static_cast<int>(g() % 3);
        for (int k = 0; k < nflips; ++k) w.flags ^= kFlagPool[g() % (sizeof kFlagPool / sizeof kFlagPool[0])];
        if (g() % 3 == 0) w.burst = static_cast<u16>(g() % 5);
        if (g() % 3 == 0) w.burst_rate = static_cast<u16>(g() % 9);
        if (g() % 4 == 0) w.spray_angle = static_cast<u16>(g() % 3000);
        if (g() % 4 == 0) w.random_decay = static_cast<u16>(g() % 40);
        if (g() % 3 == 0) w.weapon_timer = static_cast<u16>(g() % 4 ? g() % 200 : 0);
        if (g() % 3 == 0) w.area_of_effect = static_cast<u16>(g() % 300);
        if (g() % 3 == 0) w.turn_rate = static_cast<u16>(g() % 3000);
        if (g() % 4 == 0) w.flight_time = static_cast<u16>(g() % 150);
        if (g() % 4 == 0) w.duration = static_cast<u16>(g() % 20);
        if (g() % 4 == 0) w.smoke_delay = static_cast<u16>(g() % 10);
        if (g() % 4 == 0) w.edge_effectiveness = static_cast<float>(g() % 1000) / 999.0f;
        if (w.velocity == 0) w.velocity = static_cast<i32>(g() % 0x60000 + 0x8000);
        // A burst without a weapon timer divides by the shot's speed, which ballistic shots never
        // set (a stale value, 0 in a fresh slot: the original crashes). Keep that rare.
        if (w.burst > 0 && w.weapon_timer == 0 && g() % 4) w.weapon_timer = static_cast<u16>(1 + g() % 100);
    }
    for (const auto& w : weapons) write_weapon(r, L, w, dmaps[w.index]);

    // Unit types.
    u32 file_at = L.files;
    std::vector<sim::UnitTypeInfo> type_info(ntypes);
    for (int k = 0; k < ntypes; ++k) {
        LoadedType& t = types[static_cast<std::size_t>(pick[static_cast<std::size_t>(k)])];
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
        t.tick_info.damage_modifier = g() % 2 ? 0x10000 : static_cast<i32>(g() % 0x20000);
        t.tick_info.build_time = g() % 10 == 0 ? 0 : static_cast<i32>(g() % 20000);
        t.tick_info.build_cost_energy = static_cast<float>(g() % 100000) / 7.0f;
        t.tick_info.build_cost_metal = static_cast<float>(g() % 5000) / 3.0f;
        sim::UnitTypeInfo& ti = type_info[static_cast<std::size_t>(k)];
        ti = t.info;
        ti.explode_as = t.def->explode_as >= 0 ? &weapons[static_cast<std::size_t>(t.def->explode_as)] : nullptr;
        ti.self_destruct_as = t.def->self_destruct_as >= 0 ? &weapons[static_cast<std::size_t>(t.def->self_destruct_as)] : nullptr;
        t.remote_def = L.defs + static_cast<u32>(k) * 0x400;
        std::vector<std::uint8_t> d(kDefSize, 0);
        auto put = [&](u32 off, const void* p, std::size_t n) { std::memcpy(&d[off], p, n); };
        put(dName, t.tick_info.name.c_str(), (std::min)(t.tick_info.name.size() + 1, std::size_t{32}));
        put(dCob, &t.remote_cob, 4);
        put(dMinX, &ti.model_min_x, 4);
        put(dMinY, &ti.model_min_y, 4);
        put(dMinZ, &ti.model_min_z, 4);
        put(dMaxX, &ti.model_max_x, 4);
        put(dModelMaxY, &ti.model_max_y, 4);
        put(dMaxZ, &ti.model_max_z, 4);
        put(dEnergyCost, &t.tick_info.build_cost_energy, 4);
        put(dMetalCost, &t.tick_info.build_cost_metal, 4);
        put(dDamageModifier, &t.tick_info.damage_modifier, 4);
        put(dWindGen, &t.tick_info.wind_generator, 4);
        put(dBuildTime, &t.tick_info.build_time, 4);
        put(dMaxDamage, &ti.max_damage, 4);
        put(dHealTime, &t.tick_info.heal_time, 2);
        put(dTypeFlags, &t.tick_info.type_flags, 4);
        const u32 ea = ti.explode_as ? L.wdefs + static_cast<u32>(ti.explode_as->index) * kWeaponSize : 0u;
        const u32 sa = ti.self_destruct_as ? L.wdefs + static_cast<u32>(ti.self_destruct_as->index) * kWeaponSize : 0u;
        put(dExplodeAs, &ea, 4);
        put(dSelfDestructAs, &sa, 4);
        r.write(t.remote_def, d.data(), d.size());
        r.w32(L.models + static_cast<u32>(k + 1) * 4, t.remote_model);
    }
    std::vector<std::uint8_t> ed(kDefSize, 0);
    r.write(L.empty_def, ed.data(), ed.size());
    sim::UnitTypeInfo empty_type;
    empty_type.max_damage = 0;
    sim::UnitTickTypeInfo empty_tick;
    empty_tick.damage_modifier = 0;

    // Players: slots 0..2 playing (controllers 1, 2, 3), a few idle slots, and record 10 (the "no owner" slot).
    std::vector<sim::PlayerSlot> players(11);
    const int per = kMaxUnits / 3;
    std::vector<std::string> side_names(3);
    for (int s = 0; s < 3; ++s) {
        const LoadedType& t = types[static_cast<std::size_t>(pick[g() % pick.size()])];
        side_names[static_cast<std::size_t>(s)] = random_case(t.tick_info.name, g);
        const std::string& n = side_names[static_cast<std::size_t>(s)];
        r.write(L.gs + gCommanderNames + static_cast<u32>(s) * kSideSize, n.c_str(), (std::min)(n.size() + 1, std::size_t{32}));
    }
    for (int p = 0; p < 11; ++p) {
        sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        const bool playing = p < 3;
        s.active = playing || g() % 3 == 0;
        s.controller = static_cast<std::uint8_t>(playing ? (g() % 4 == 0 ? 1 + g() % 3 : p + 1) : (g() % 2 ? 0 : 1 + g() % 3));
        s.state = static_cast<std::uint8_t>(g() % 4);
        s.net_id = g() % 6 == 0 ? -1 : static_cast<i32>(g() % 5);
        if (playing) {
            s.first_unit = 1 + p * per;
            s.last_unit = s.first_unit + per - 1;
        }
        s.units_killed = static_cast<std::uint16_t>(g() % 50);
        s.units_lost = static_cast<std::uint16_t>(g() % 50);
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
        r.w32(at + pFirst, L.units + static_cast<u32>(playing ? s.first_unit : 1) * kUnitSize);
        r.w32(at + pLast, L.units + static_cast<u32>(playing ? s.last_unit : 0) * kUnitSize);
    }

    // Map: the base cells, with units, features and heights varied in the battle region.
    sim::MapGrid map;
    map.width = hm.width();
    map.height = hm.height();
    map.cells = base_cells;
    std::vector<sim::FeatureTypeInfo> features(kFeatureTypes);
    for (auto& f : features) {
        f.footprint_x = static_cast<std::int16_t>(1 + g() % 4);
        f.footprint_z = static_cast<std::int16_t>(1 + g() % 4);
        f.height = static_cast<std::uint8_t>(g() % 60);
        std::vector<std::uint8_t> d(0x100, 0);
        std::memcpy(&d[0x94], &f.footprint_x, 2);
        std::memcpy(&d[0x96], &f.footprint_z, 2);
        d[0xFA] = f.height;
        r.write(L.fdefs + static_cast<u32>(&f - features.data()) * 0x100, d.data(), d.size());
    }
    r.w32(L.gs + gFeatureCount, kFeatureTypes);

    // Units.
    std::vector<sim::Unit> units(kMaxUnits + 2);
    std::vector<sim::UnitTickState> tstate(kMaxUnits + 2);
    sim::CombatEnvironment cenv;
    cenv.empty_type = &empty_type;
    cenv.empty_tick_type = &empty_tick;
    World world;
    sim::Combat combat{units, tstate, players, cenv, world};
    sim::ProjectileTable table;
    sim::ProjectileEnvironment penv;
    sim::ProjectileWorld pw{table, combat, hm, map, features, penv, world, world.rng};
    world.units = &units;
    world.ticks = &tstate;
    world.players = &players;
    world.combat = &combat;
    world.pw = &pw;
    world.hm = &hm;
    world.map = &map;
    world.slots.assign(units.size(), {});
    world.motion_speeds.assign(units.size(), 0);
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
        if (t.owner > 2 || g() % 6 == 0) continue;
        present.push_back(i);
        ++players[static_cast<std::size_t>(t.owner)].alive_count;
    }
    if (present.empty()) return true;
    for (int p = 0; p < 3; ++p) {
        sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        r.write(L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize + pAlive, &s.alive_count, 2);
    }
    const i32 region_lo = kRegionX * 16, region_span = kRegionSize * 16;
    auto region_pos = [&]() { return static_cast<i32>((static_cast<u32>(region_lo) + g() % static_cast<u32>(region_span)) << 16) + static_cast<i32>(g() % 0x10000); };
    for (int i : present) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
        const int k = static_cast<int>(g() % ntypes);
        LoadedType& lt = types[static_cast<std::size_t>(pick[static_cast<std::size_t>(k)])];
        t.type_index = static_cast<std::uint16_t>(k + 1);
        t.tick_type = &lt.tick_info;
        u.type = &type_info[static_cast<std::size_t>(k)];
        u.pos[0] = region_pos();
        u.pos[2] = region_pos();
        const i32 ground = hm.sample(static_cast<std::int16_t>(u.pos[0] >> 16), static_cast<std::int16_t>(u.pos[2] >> 16));
        u.pos[1] = static_cast<i32>(static_cast<u32>(ground + static_cast<i32>(g() % 4 == 0 ? g() % 100 : 0)) << 16);
        u.heading = static_cast<std::uint16_t>(g());
        u.hit_points = static_cast<std::int16_t>(g() % (u.type->max_damage + 50) + 1);
        u.flags = sim::unit_flags::kAlive | (g() % 2 ? 0x30u : 0u) | (g() % 30 == 0 ? 0x4000u : 0u) | (g() % 3 == 0 ? 0x20000000u : 0u);
        u.activate_flags = static_cast<std::uint8_t>(g() % 2 ? sim::unit_flags::kArmored : 0);
        u.build_fraction_left = g() % 8 == 0 ? 0.5f : 0.0f;
        u.cloak_holdoff_tick = g() % 1000;
        t.has_motion = g() % 2;
        t.prev_health_percent = static_cast<std::uint8_t>(g() % 101);
        t.health_percent = t.prev_health_percent;
        t.kill_count = static_cast<std::uint16_t>(g() % 40);
        t.last_attacker_slot = 10;
        t.metal_make = static_cast<float>(g() % 100000) / 9.0f;
        const u32 at = unit_addr(i);
        const u32 owner_at = L.gs + 0x1B63 + static_cast<u32>(t.owner) * kPlayerSize;
        const std::uint8_t owner_slot = static_cast<std::uint8_t>(t.owner);
        const u32 motion_at = L.motion + static_cast<u32>(i) * 0x40;
        world.motion_speeds[static_cast<std::size_t>(i)] = static_cast<i32>(g() % 0x40000) - 0x8000;
        r.w32(motion_at + 0x20, static_cast<u32>(world.motion_speeds[static_cast<std::size_t>(i)]));
        r.w32(at + uMotion, t.has_motion ? motion_at : 0u);
        for (int s = 0; s < 3; ++s) {
            SlotState& ss = world.slots[static_cast<std::size_t>(i)][static_cast<std::size_t>(s)];
            ss.weapon = g() % 5 == 0 ? nullptr : &weapons[static_cast<std::size_t>(used[g() % used.size()])];
            ss.heading = static_cast<u16>(g());
            ss.pitch = static_cast<u16>(static_cast<i32>(g() % 24000) - 12000);
            ss.aim_drop = g() % 4 == 0 ? 0u : g() % 0x800000;
            const u32 sb = at + uSlots + static_cast<u32>(s) * 0x1C;
            r.w32(sb + 0x0C, ss.weapon ? L.wdefs + static_cast<u32>(ss.weapon->index) * kWeaponSize : 0u);
            r.w32(sb + 0x10, ss.aim_drop);
            r.write(sb + 0x16, &ss.heading, 2);
            r.write(sb + 0x18, &ss.pitch, 2);
            const std::uint8_t fl = static_cast<std::uint8_t>(s << 2);
            r.write(sb + 0x1B, &fl, 1);
            sim::WeaponSlot& ws = u.weapons[static_cast<std::size_t>(s)];
            ws.type = ss.weapon;
            ws.aux = static_cast<i32>(ss.aim_drop);
            ws.aim_heading = ss.heading;
            ws.aim_pitch = ss.pitch;
            ws.flags = fl;
        }
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
        r.w32(at + uCloak, u.cloak_holdoff_tick);
        r.write(at + uKills, &t.kill_count, 2);
        r.write(at + uMetalMake, &t.metal_make, 4);
        r.write(at + uLastAttackerSlot, &t.last_attacker_slot, 1);
        r.write(at + uHealth, &t.health_percent, 1);
        r.write(at + uPrevHealth, &t.prev_health_percent, 1);
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
    // Region cells.
    std::vector<i32> region;
    for (i32 z = kRegionZ; z < kRegionZ + kRegionSize; ++z)
        for (i32 x = kRegionX; x < kRegionX + kRegionSize; ++x) region.push_back(z * map.width + x);
    for (i32 idx : region) {
        sim::MapCell& c = map.cells[static_cast<std::size_t>(idx)];
        if (g() % 4 == 0) c.height_max = static_cast<std::uint8_t>((std::min)(255, c.height_max + static_cast<int>(g() % 30)));
        if (g() % 4 == 0) c.height_min = static_cast<std::uint8_t>((std::max)(0, c.height_min - static_cast<int>(g() % 30)));
        const u32 roll = g() % 20;
        if (roll == 0) {
            c.feature = static_cast<u16>(g() % kFeatureTypes);
            c.flags = static_cast<std::uint8_t>(g() % 3 == 0 ? 1 : 0);
            c.feature_aux = static_cast<u16>(c.flags ? g() % kAnimSlots : g() % 200);
        } else if (roll == 1) {
            c.feature = 0xFFFE;
            c.feature_aux = static_cast<u16>((g() % 3) | ((g() % 3) << 8));
            c.flags = static_cast<std::uint8_t>(g() % 4 == 0 ? 1 : 0);
        } else if (roll == 2) {
            c.feature = static_cast<u16>(0xFFFB + g() % 5);
        }
    }
    for (int i : present) {
        const sim::Unit& u = units[static_cast<std::size_t>(i)];
        const i32 cx = u.pos[0] >> 20, cz = u.pos[2] >> 20;
        for (int k = 0; k < 4; ++k) {
            const i32 x = cx + (k == 0 ? 0 : static_cast<i32>(g() % 3) - 1), z = cz + (k == 0 ? 0 : static_cast<i32>(g() % 3) - 1);
            sim::MapCell* c = map.cell(x, z);
            if (!c) continue;
            if (c->unit == 0 && (k == 0 || g() % 2)) c->unit = u.id;
            else c->unit2 = u.id;
            region.push_back(z * map.width + x);
        }
    }
    for (i32 idx : region) {
        put_cell(r, L, idx, map.cells[static_cast<std::size_t>(idx)]);
        dirty.push_back(idx);
    }
    world.anim_pos.resize(kAnimSlots);
    for (int a = 0; a < kAnimSlots; ++a) {
        sim::Vec3i p{region_pos(), static_cast<i32>(g() % (200u << 16)), region_pos()};
        world.anim_pos[static_cast<std::size_t>(a)] = p;
        r.write(L.fanims + static_cast<u32>(a) * 0x30 + 8, p.data(), 12);
    }
    for (int i : present) {
        r.write(L.names, "Create", 7);
        if (!r.call(kCallNamed, remote_script[static_cast<std::size_t>(i)], {L.names, 0, 1})) { std::printf("Create failed\n"); return false; }
        units[static_cast<std::size_t>(i)].script->call("Create", nullptr, true);
    }
    ++st.scenarios;

    sim::UnitTickEnvironment env;
    env.addresses.game_state = L.gs;
    env.addresses.unit_table = L.units;
    std::int16_t camera = 0;
    i32 env_live = 0;
    const u32 cells_end = L.cells + static_cast<u32>(map.cells.size()) * kCellSize;
    auto norm_ptr = [&](u32& w) {
        if (w >= L.units && w < L.units + (kMaxUnits + 2) * kUnitSize) {
            const u32 off = (w - L.units) % kUnitSize, idx = (w - L.units) / kUnitSize;
            if (off == 0) w = 0x40000000u | idx;
            else if (off == uResource) w = 0x41000000u | idx;
            else if (off == uPosX) w = 0x42000000u | idx;
        } else if (w >= L.gs + 0x1B63 && w < L.gs + 0x1B63 + 11 * kPlayerSize && (w - L.gs - 0x1B63) % kPlayerSize == 0) {
            w = 0x20000000u | ((w - L.gs - 0x1B63) / kPlayerSize);
        } else if (w >= L.heap + (64u << 20) && w < L.heap + (64u << 20) + 0x4000u * (kMaxUnits + 2) && (w - L.heap - (64u << 20)) % 0x4000 == 0) {
            w = 0x40000000u | ((w - L.heap - (64u << 20)) / 0x4000);
        } else if (w >= L.motion && w < L.motion + 0x40 * (kMaxUnits + 2) && (w - L.motion) % 0x40 == 0) {
            w = 0x7E000000u | ((w - L.motion) / 0x40);
        } else if (w >= L.cells && w < cells_end && (w - L.cells) % kCellSize == 0) {
            w = 0x5C000000u | ((w - L.cells) / kCellSize);
        } else if (w >= L.wdefs && w < L.wdefs + 256 * kWeaponSize && (w - L.wdefs) % kWeaponSize == 0) {
            w = 0x5D000000u | ((w - L.wdefs) / kWeaponSize);
        }
    };
    auto normalise = [&](std::vector<u32>& log, bool remote) {
        for (std::size_t i = 0; i + 1 < log.size();) {
            const Stub* s = stub_by_id(log[i]);
            if (!s) break;
            if (remote) {
                if (s->ptrs & ECXP) norm_ptr(log[i + 1]);
                else log[i + 1] = 0;
                for (int a = 0; a < s->args; ++a) {
                    if (s->ptrs & (1u << a)) norm_ptr(log[i + 2 + static_cast<std::size_t>(a)]);
                    if (s->zero & (1u << a)) log[i + 2 + static_cast<std::size_t>(a)] = 0;
                }
                if (log[i] == 23) log[i + 2] = 0;  // the mission type byte read from an uninitialised stack slot
            }
            if (log[i] == 16) {
                const u32 size = log[i + 4];
                if (size == 4) log[i + 2] = 0;
                std::uint8_t bytes[36];
                std::memcpy(bytes, &log[i + 5], 36);
                u32 keep = size < 36 ? size : 36;
                if (bytes[0] == 0x0D && size == 36) keep = 26;  // the rest is uninitialised stack
                if (size == 4) keep = 0;
                for (u32 b = keep; b < 36; ++b) bytes[b] = 0;
                std::memcpy(&log[i + 5], bytes, 36);
            }
            i += entry_words(*s);
        }
    };
    auto count_log = [&](const std::vector<u32>& log) {
        for (std::size_t i = 0; i + 1 < log.size();) {
            const Stub* s = stub_by_id(log[i]);
            if (!s) break;
            if (log[i] == 64) ++st.explosions;
            if (log[i] == 62 && (log[i + 3] & 1)) ++st.water_hits;
            if (log[i] == 20) ++st.damage_events;
            if (log[i] == 16 && log[i + 4] != 4) ++st.packets[log[i + 5] & 0xFF];
            if (log[i] == 65) ++st.feature_hits;
            i += entry_words(*s);
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
        normalise(log, true);
        std::vector<u32> mine = world.log;
        world.log.clear();
        normalise(mine, false);
        count_log(log);
        if (log != mine) {
            bad("subsystem call log differs");
            // Entry by entry, from a few entries before the first difference.
            auto entries = [&](const std::vector<u32>& v) {
                std::vector<std::vector<u32>> out;
                for (std::size_t i = 0; i + 1 < v.size();) {
                    const Stub* s = stub_by_id(v[i]);
                    const std::size_t n = s ? entry_words(*s) : v.size() - i;
                    out.emplace_back(v.begin() + static_cast<std::ptrdiff_t>(i), v.begin() + static_cast<std::ptrdiff_t>((std::min)(v.size(), i + n)));
                    i += n;
                }
                return out;
            };
            const auto ea = entries(log), eb = entries(mine);
            std::size_t first = 0;
            while (first < ea.size() && first < eb.size() && ea[first] == eb[first]) ++first;
            auto show = [](const char* who, const std::vector<u32>& e) {
                std::printf("      %s", who);
                for (u32 w : e) std::printf(" %x", w);
                std::printf("\n");
            };
            for (std::size_t i = first > 6 ? first - 6 : 0; i < first + 4 && i < (std::max)(ea.size(), eb.size()); ++i) {
                std::printf("    entry %zu%s\n", i, i >= first ? "   <--" : "");
                if (i < ea.size()) show("orig", ea[i]);
                if (i < eb.size()) show("ours", eb[i]);
            }
        }
        if (static_cast<i32>(r.r32(L.gs + 0x14353)) != env_live) bad("live unit count");
        if (r.r32(kBattleRngState) != world.rng.state()) bad("battle RNG state");
        // Projectile table, all slots.
        const i32 count = static_cast<i32>(r.r32(L.gs + gProjCount));
        if (count != table.count) bad("projectile count " + std::to_string(count) + " vs " + std::to_string(table.count));
        std::vector<std::uint8_t> pt(sim::kMaxProjectiles * kProjSize);
        r.read(L.proj, pt.data(), pt.size());
        for (int s = 0; s < sim::kMaxProjectiles && ok; ++s) {
            const std::uint8_t* b = &pt[static_cast<std::size_t>(s) * kProjSize];
            const sim::Projectile& p = table.slots[static_cast<std::size_t>(s)];
            auto f32 = [&](u32 off) { u32 v; std::memcpy(&v, b + off, 4); return v; };
            auto f16 = [&](u32 off) { u16 v; std::memcpy(&v, b + off, 2); return v; };
            auto unit_ptr = [&](const sim::Unit* u) { return u ? unit_addr(u->id) : 0u; };
            std::string field;
            const u32 wp = p.weapon ? L.wdefs + static_cast<u32>(p.weapon->index) * kWeaponSize : 0u;
            if (f32(0) != wp) field = "weapon";
            for (int k = 0; k < 3 && field.empty(); ++k) {
                if (f32(4 + 4 * static_cast<u32>(k)) != static_cast<u32>(p.pos[static_cast<std::size_t>(k)])) field = "pos";
                if (f32(0x10 + 4 * static_cast<u32>(k)) != static_cast<u32>(p.trail[static_cast<std::size_t>(k)])) field = "trail";
                if (f32(0x1C + 4 * static_cast<u32>(k)) != static_cast<u32>(p.vel[static_cast<std::size_t>(k)])) field = "velocity";
                if (f32(0x28 + 4 * static_cast<u32>(k)) != static_cast<u32>(p.aim[static_cast<std::size_t>(k)])) field = "aim";
            }
            if (field.empty()) {
                if (f16(0x34) != static_cast<u16>(p.tumble)) field = "tumble";
                else if (f16(0x36) != p.yaw) field = "yaw";
                else if (f16(0x38) != p.elevation) field = "elevation";
                else if (f32(0x3A) != static_cast<u32>(p.speed)) field = "speed";
                else if (f32(0x3E) != static_cast<u32>(p.distance)) field = "distance";
                else if (f32(0x42) != p.spawn_tick) field = "spawn tick";
                else if (f32(0x46) != p.expire_tick) field = "expire tick";
                else if (f32(0x4A) != p.next_smoke_tick) field = "smoke tick";
                else if (f32(0x4E) != unit_ptr(p.target)) field = "target";
                else if (f32(0x52) != unit_ptr(p.owner)) field = "owner";
                else if (f32(0x56) != (p.intercepted >= 0 ? L.proj + static_cast<u32>(p.intercepted) * kProjSize : 0u)) field = "chased shot";
                else if (f16(0x5A) != static_cast<u16>(p.last_feature_x) || f16(0x5C) != static_cast<u16>(p.last_feature_z)) field = "feature cell";
                else if (f16(0x5E) != static_cast<u16>(p.ground_height)) field = "ground height";
                else if (f16(0x60) != p.burst_left) field = "burst";
                else if (f16(0x62) != static_cast<u16>(p.piece)) field = "piece";
                else if (f16(0x64) != static_cast<u16>(p.propeller)) field = "propeller";
                else if (b[0x66] != p.owner_slot) field = "owner slot";
                else if (f16(0x67) != static_cast<u16>(p.compact_index)) field = "compact index";
                else if (f16(0x69) != p.flags) field = "flags";
            }
            if (!field.empty()) bad("projectile slot " + std::to_string(s) + " " + field);
        }
        const u32 track = r.r32(L.gs + gCamTrack);
        const u32 my_track = table.camera_slot >= 0 ? L.proj + static_cast<u32>(table.camera_slot) * kProjSize : 0u;
        if (track != my_track) bad("camera followed shot");
        sim::Vec3i rest{};
        r.read(L.gs + gCamRest, rest.data(), 12);
        if (rest != table.camera_rest || r.rd<u16>(L.gs + gCamRestTicks) != table.camera_rest_ticks) bad("camera rest");
        for (int p = 0; p < 11; ++p) {
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
            for (int s = 0; s < 3; ++s) {
                const sim::WeaponSlot& ws = u.weapons[static_cast<std::size_t>(s)];
                const u32 so = uSlots + static_cast<u32>(s) * 0x1C;
                if (static_cast<std::int32_t>(f32(so + 8)) != ws.aim.result || f16(so + 0x16) != ws.aim_heading ||
                    f16(so + 0x18) != ws.aim_pitch || f8(so + 0x1B) != ws.flags)
                    bad(un + "weapon slot " + std::to_string(s));
            }
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
            if (f32(uCloak) != u.cloak_holdoff_tick) bad(un + "cloak hold-off");
            if ((f32(uScript) != 0) != (u.script != nullptr) || (f32(uMotion) != 0) != t.has_motion) bad(un + "script/motion presence");
            if (!u.script) continue;
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
    auto random_stale = [&]() -> u32 { return g() % 3 == 0 ? g() % 16 : static_cast<u32>(g()); };
    auto check_stale = [&](u32 mine, const std::string& what) {
        const u32 theirs = r.r32(L.ret_val + 0x14);
        if (theirs == mine) return true;
        std::printf("scenario %u %s: stale stack word orig %08x ours %08x\n", seed, what.c_str(), theirs, mine);
        ++st.mismatches;
        return false;
    };
    // A failed original call is expected only where the engine noted that the original crashes.
    auto fail = [&](const std::string& what) {
        if (!table.fault.empty()) {
            ++st.both_crashed;
            std::printf("scenario %u: the original crashed during %s, as the engine predicted (%s)\n", seed, what.c_str(), table.fault.c_str());
        } else {
            std::printf("scenario %u: the original failed during %s\n", seed, what.c_str());
        }
        return false;
    };
    auto put_vec = [&](u32 at, const sim::Vec3i& v) { r.write(at, v.data(), 12); };
    auto jitter = [&](const sim::Vec3i& c, u32 spread) {
        sim::Vec3i v = c;
        for (auto& x : v) x = static_cast<i32>(static_cast<u32>(x) + (g() % (spread * 2 + 1) - spread) * 0x10000u + g() % 0x10000);
        return v;
    };
    auto random_point = [&]() {
        if (g() % 3 == 0) return sim::Vec3i{region_pos(), static_cast<i32>(g() % (300u << 16)), region_pos()};
        return jitter(units[static_cast<std::size_t>(any_unit())].pos, 40);
    };
    if (engine_faulted()) return true;
    if (!compare("after Create")) return true;

    for (int tick = 0; tick < ticks_to_run; ++tick) {
        env.game_tick = static_cast<u32>(tick + 1000);
        env.wind_changed = g() % 8 == 0;
        env.wind_heading = static_cast<std::uint16_t>(g());
        env.wind_speed = static_cast<i32>(g() % 3000);
        env.water_damage_enabled = g() % 8 == 0;
        env.water_damage = static_cast<i32>(g() % 3) * 50;
        if (tick == 0 || g() % 20 == 0) env.sea_level = static_cast<std::uint8_t>(g() % 4 ? g() % 60 : g());
        env.broadcast_unit_states = g() % 2;
        cenv.local_player = static_cast<std::uint8_t>(g() % 4);
        cenv.local_view = static_cast<std::uint8_t>(g() % 4);
        cenv.game_mode = static_cast<i32>(g() % 3);
        cenv.ai_difficulty = static_cast<i32>(g() % 3);
        cenv.fog_of_war = g() % 2;
        world.kill_flash = g() % 2;
        world.pay_result = g() % 4 ? 1u : 0u;
        penv.game_tick = env.game_tick;
        if (tick == 0 || g() % 10 == 0) {
            penv.wind = {static_cast<i32>(g() % 0x8000) - 0x4000, 0, static_cast<i32>(g() % 0x8000) - 0x4000};
            penv.gravity = static_cast<i32>(g() % 0x2000);
            penv.no_sea_level_trigger = g() % 4 == 0;
            penv.options = static_cast<u16>((g() % 8 == 0 ? 0x80 : 0) | (g() % 8 == 0 ? 0x100 : 0));
        }
        penv.sea_level = env.sea_level;
        penv.net_game = env.broadcast_unit_states;
        if (g() % 15 == 0) table.camera_unit = g() % 3 ? &units[static_cast<std::size_t>(any_unit())] : nullptr;
        r.w32(L.gs + 0x38A47, env.game_tick);
        r.w32(L.gs + 0x37EE2, env.wind_changed ? 1u : 0u);
        r.write(L.gs + 0x37ED8, &env.wind_heading, 2);
        r.w32(L.gs + 0x37EDA, static_cast<u32>(env.wind_speed));
        r.w32(L.schema + 0xD4C, env.water_damage_enabled ? 1u : 0u);
        r.w32(L.schema + 0xD50, static_cast<u32>(env.water_damage));
        r.w32(L.schema + sNoSeaTrigger, penv.no_sea_level_trigger ? 1u : 0u);
        r.write(L.gs + gSea, &env.sea_level, 1);
        r.w32(L.gs + gNetFlags, env.broadcast_unit_states ? 1u : 0u);
        r.w32(L.gs + 0x14373, 0);
        r.write(L.gs + gLocalPlayer, &cenv.local_player, 1);
        r.write(L.gs + gLocalView, &cenv.local_view, 1);
        r.w32(L.gs + gGameMode, static_cast<u32>(cenv.game_mode));
        r.w32(L.gs + gDifficulty, static_cast<u32>(cenv.ai_difficulty));
        const std::uint8_t map_flags = cenv.fog_of_war ? 2 : 0, flash = world.kill_flash ? 0x80 : 0;
        r.write(L.gs + gMapFlags, &map_flags, 1);
        r.write(L.gs + gFlash, &flash, 1);
        r.w32(L.ret_val, world.pay_result);
        r.write(L.gs + gWind, penv.wind.data(), 12);
        r.w32(L.gs + gGravity, static_cast<u32>(penv.gravity));
        r.write(L.gs + gOptions, &penv.options, 2);
        r.w32(L.gs + gCamUnit, table.camera_unit ? unit_addr(table.camera_unit->id) : 0u);

        const int nops = static_cast<int>(seed % 8 == 3 ? g() % 80 : g() % 5);  // some scenarios fill the table
        for (int op = 0; op < nops; ++op) {
            const int kind_of_op = static_cast<int>(seed % 8 == 3 ? g() % 12 : g() % 20);
            std::string what;
            if (kind_of_op < 10 && g() % 2 == 0) {  // a weapon's fire routine on a random slot state
                const int oi = any_unit();
                sim::Unit& owner = units[static_cast<std::size_t>(oi)];
                if (!alive(oi) || !owner.script) continue;
                const int k = static_cast<int>(g() % 3);
                sim::WeaponSlot& ws = owner.weapons[static_cast<std::size_t>(k)];
                if (!ws.type || ws.type->fire == sim::WeaponFireRoutine::None) continue;
                // Dropped weapons read the bomb speed from the motion object (the original crashes without one).
                if (ws.type->fire == sim::WeaponFireRoutine::Dropped && !tstate[static_cast<std::size_t>(oi)].has_motion) continue;
                ws.flags = static_cast<std::uint8_t>((ws.flags & ~1u) | (g() % 4 ? 1u : 0u));
                ws.aim.result = g() % 4 ? 1 : 0;
                ws.stockpile = static_cast<std::uint8_t>(g() % 3);
                if (g() % 2) ws.aim_heading = static_cast<u16>(g());
                if (g() % 2) ws.aim_pitch = static_cast<u16>(static_cast<i32>(g() % 24000) - 12000);
                const u32 sb = unit_addr(oi) + uSlots + static_cast<u32>(k) * 0x1C;
                r.w32(sb + 0x08, static_cast<u32>(ws.aim.result));
                r.write(sb + 0x16, &ws.aim_heading, 2);
                r.write(sb + 0x18, &ws.aim_pitch, 2);
                r.write(sb + 0x1A, &ws.stockpile, 1);
                r.write(sb + 0x1B, &ws.flags, 1);
                const sim::Vec3i aim = g() % 3 ? random_point() : jitter(owner.pos, 2000);
                const int ti = g() % 3 == 0 ? 0 : any_unit();
                sim::Unit* target = ti ? &units[static_cast<std::size_t>(ti)] : nullptr;
                put_vec(L.args, aim);
                u32 fn = kFireTurret;
                switch (ws.type->fire) {
                case sim::WeaponFireRoutine::VerticalLaunch: fn = kFireVertical; break;
                case sim::WeaponFireRoutine::LineOfSight: fn = kFireLineOfSight; break;
                case sim::WeaponFireRoutine::Dropped: fn = kFireDropped; break;
                default: break;
                }
                const std::int32_t mine = sim::fire_weapon(pw, owner, k, target, aim);
                u32 ret = 0;
                if (!r.call(fn, 0, {unit_addr(oi), sb, ti ? unit_addr(ti) : 0u, L.args}, &ret)) return fail("fire routine " + std::to_string(fn) + " flags " + std::to_string(ws.type->flags) + " vel " + std::to_string(ws.type->velocity) + " stock " + std::to_string(ws.stockpile) + " aiming " + std::to_string(ws.flags & 1) + " result " + std::to_string(ws.aim.result));
                ++st.fire_routines;
                what = "fire routine";
                if ((ret != 0) != (mine != 0)) {
                    std::printf("scenario %u: fire routine result differs (orig %u ours %d)\n", seed, ret, mine);
                    ++st.mismatches;
                    return true;
                }
            } else if (kind_of_op < 10) {  // launch from a weapon slot
                const int oi = any_unit();
                sim::Unit& owner = units[static_cast<std::size_t>(oi)];
                if (!alive(oi) || !owner.script) continue;
                const int k = static_cast<int>(g() % 3);
                const sim::WeaponSlot& ws = owner.weapons[static_cast<std::size_t>(k)];
                if (!ws.type) continue;
                const sim::FireSlot slot{ws.type, k, ws.aim_heading, ws.aim_pitch, static_cast<u32>(ws.aux)};
                const sim::Vec3i muzzle = jitter(owner.pos, 20);
                const sim::Vec3i aim = random_point();
                const int ti = g() % 3 == 0 ? 0 : any_unit();
                sim::Unit* target = ti ? &units[static_cast<std::size_t>(ti)] : nullptr;
                put_vec(L.args, muzzle);
                put_vec(L.args + 16, aim);
                const u32 slot_at = unit_addr(oi) + uSlots + static_cast<u32>(k) * 0x1C;
                const int how = kind_of_op < 5 ? 0 : kind_of_op < 9 ? 1 : 2;
                if (how == 1 && ws.type->velocity == 0) continue;
                u32 ret = 0;
                bool mine = false;
                if (how == 0) {
                    mine = sim::fire_directed(pw, slot, owner, muzzle, aim, target);
                    if (!r.call(kFireDirected, 0, {slot_at, unit_addr(oi), L.args, L.args + 16, ti ? unit_addr(ti) : 0u}, &ret)) return fail("directed launch");
                    ++st.directed;
                    what = "directed launch";
                } else if (how == 1) {
                    mine = sim::fire_ballistic(pw, slot, owner, muzzle, aim, target);
                    if (!r.call(kFireBallistic, 0, {slot_at, unit_addr(oi), L.args, L.args + 16, ti ? unit_addr(ti) : 0u}, &ret)) return fail("ballistic launch");
                    ++st.ballistic;
                    what = "ballistic launch";
                } else {
                    if (!tstate[static_cast<std::size_t>(oi)].has_motion) continue;
                    mine = sim::spawn_from_motion(pw, slot, owner, muzzle);
                    if (!r.call(kSpawnMotion, 0, {slot_at, unit_addr(oi), L.args}, &ret)) return fail("launch from motion");
                    ++st.motion_shots;
                    what = "launch from motion";
                }
                if ((ret != 0) != mine) {
                    std::printf("scenario %u: %s result differs\n", seed, what.c_str());
                    ++st.mismatches;
                    return true;
                }
            } else if (kind_of_op < 12) {  // ownerless shot (meteor shower)
                int s = used[g() % used.size()];
                if (weapons[static_cast<std::size_t>(s)].flags & sim::weapon_flags::kInterceptor) continue;
                sim::Vec3i pos{region_pos(), static_cast<i32>((200u + g() % 600u) << 16), region_pos()};
                sim::Vec3i vel{static_cast<i32>(g() % 0x40000) - 0x20000, -static_cast<i32>(g() % 0x80000), static_cast<i32>(g() % 0x40000) - 0x20000};
                if (g() % 6 == 0) {
                    // Near a map edge, heading out: map-edge removal and clamped area damage.
                    const int edge = static_cast<int>(g() % 4);
                    const i32 far_x = (L.map_w * 16 - 1) << 16, far_z = (L.map_h * 16 - 1) << 16;
                    const i32 off = static_cast<i32>(g() % (64u << 16));
                    if (edge == 0) pos[0] = off, vel[0] = -static_cast<i32>(g() % 0x80000);
                    if (edge == 1) pos[0] = far_x - off, vel[0] = static_cast<i32>(g() % 0x80000);
                    if (edge == 2) pos[2] = off, vel[2] = -static_cast<i32>(g() % 0x80000);
                    if (edge == 3) pos[2] = far_z - off, vel[2] = static_cast<i32>(g() % 0x80000);
                    pos[1] = static_cast<i32>((g() % 200u) << 16);
                }
                const bool bcast = g() % 2;
                const bool mine = sim::spawn_free_projectile(pw, weapons[static_cast<std::size_t>(s)], pos, vel, bcast);
                put_vec(L.args, pos);
                put_vec(L.args + 16, vel);
                u32 ret = 0;
                if (!r.call(kSpawnFree, 0, {L.wdefs + static_cast<u32>(s) * kWeaponSize, L.args, L.args + 16, bcast ? 1u : 0u}, &ret)) return fail("free shot");
                if ((ret != 0) != mine) {
                    std::printf("scenario %u: free shot result differs\n", seed);
                    ++st.mismatches;
                    return true;
                }
                ++st.free_shots;
                what = "free shot";
            } else if (kind_of_op < 14) {  // chase assignment by the (not modelled) targeting code
                if (table.count == 0) continue;
                const int i = static_cast<int>(g() % static_cast<u32>(table.count));
                sim::Projectile& p = table.slots[static_cast<std::size_t>(i)];
                const int j = g() % 3 == 0 ? -1 : static_cast<int>(g() % static_cast<u32>(table.count));
                p.intercepted = j;
                r.w32(L.proj + static_cast<u32>(i) * kProjSize + 0x56, j >= 0 ? L.proj + static_cast<u32>(j) * kProjSize : 0u);
                if (g() % 2) {
                    const int ti = g() % 3 == 0 ? 0 : any_unit();
                    p.target = ti ? &units[static_cast<std::size_t>(ti)] : nullptr;
                    r.w32(L.proj + static_cast<u32>(i) * kProjSize + 0x4E, ti ? unit_addr(ti) : 0u);
                }
                ++st.chases;
                continue;
            } else if (kind_of_op < 16) {  // received detonate packet
                std::uint8_t pkt[14] = {0x0E};
                if (table.count > 0 && g() % 4) {
                    const sim::Projectile& p = table.slots[g() % static_cast<u32>(table.count)];
                    std::memcpy(&pkt[1], p.aim.data(), 12);
                    pkt[13] = p.weapon->index;
                    if (g() % 4 == 0) pkt[13] = static_cast<std::uint8_t>(g());
                } else {
                    for (int b = 1; b < 14; ++b) pkt[b] = static_cast<std::uint8_t>(g());
                }
                sim::detonate_matching(pw, pkt);
                r.write(L.names, pkt, 14);
                if (!r.call(kDetonateMatching, 0, {0, L.names})) return fail("detonate packet");
                ++st.matches;
                what = "detonate packet";
            } else if (kind_of_op < 17) {  // kill (runs the unit's death explosion and removes its burst launchers)
                const int ui = any_unit();
                if (!alive(ui)) continue;
                const i32 kind = static_cast<i32>(g() % 12);
                if (g() % 2) units[static_cast<std::size_t>(ui)].hit_points = static_cast<std::int16_t>(-static_cast<i32>(g() % 500));
                const std::int16_t hp = units[static_cast<std::size_t>(ui)].hit_points;
                r.write(unit_addr(ui) + uHp, &hp, 2);
                u32 stale = random_stale();
                r.w32(L.ret_val + 0x10, stale);
                r.w32(L.ret_val + 0x18, static_cast<u32>(kind));
                r.w32(L.ret_val + 0x1C, unit_addr(ui));
                sim::kill_unit(combat, units[static_cast<std::size_t>(ui)], kind, stale);
                if (!r.call(L.caller_kill, 0, {})) return fail("kill kind " + std::to_string(kind));
                if (!check_stale(stale, "kill " + std::to_string(ui))) return true;
                ++st.kills;
                what = "kill " + std::to_string(ui) + " kind " + std::to_string(kind);
            } else {
                continue;
            }
            if (std::getenv("PH_TRACE")) std::printf("trace: %s tick %d\n", what.c_str(), tick);
            if (engine_faulted()) return true;
            if (!compare(what + " tick " + std::to_string(tick))) return true;
        }

        // One unit pass, then the projectile update (the order of the original's step).
        env.stale_word = random_stale();
        r.w32(L.ret_val + 0x10, env.stale_word);
        env_live = sim::run_unit_pass(units, tstate, players, env, world, camera);
        if (engine_faulted()) return true;
        if (!r.call(L.caller_pass, 0, {})) return fail("unit pass");
        if (!check_stale(env.stale_word, "unit pass tick " + std::to_string(tick))) return true;
        ++st.ticks;
        if (!compare("unit pass tick " + std::to_string(tick))) return true;
        for (i32 i = 0; i < table.count; ++i) {
            const sim::Projectile& p = table.slots[static_cast<std::size_t>(i)];
            const u32 f = p.weapon->flags;
            namespace wf = sim::weapon_flags;
            if (p.burst_left) ++st.kind[0];
            else if (f & wf::kSelfPropelled) {
                ++st.kind[1];
                if ((f & wf::kTwoPhase) ? (p.flags & 0x30) != 0 : (f & wf::kGuidance) != 0) ++st.kind[2];
                if (f & wf::kCruise) ++st.kind[3];
            } else if (f & wf::kLineOfSight) ++st.kind[(f & wf::kBeam) ? 5 : 4];
            else if ((f & wf::kBallistic) && p.weapon->weapon_timer) ++st.kind[6];
            else if (f & (wf::kBallistic | wf::kDropped)) ++st.kind[7];
            else if (f & wf::kMeteor) ++st.kind[8];
            else ++st.kind[9];
            if (p.intercepted >= 0) ++st.kind[10];
        }
        if (table.camera_slot >= 0) ++st.kind[11];
        if (table.count == sim::kMaxProjectiles) ++st.full_table;
        if (std::getenv("PH_TRACE")) std::printf("trace: update tick %d count %d\n", tick, table.count);
        sim::update_projectiles(pw);
        if (std::getenv("PH_TRACE")) std::printf("trace: engine update done\n");
        if (!r.call(kUpdate, 0, {}, nullptr, 20000)) return fail("projectile update tick " + std::to_string(tick));
        if (!table.fault.empty()) {
            std::printf("scenario %u tick %d: engine notes an original crash (%s) but the original ran on\n", seed, tick, table.fault.c_str());
            ++st.mismatches;
            return true;
        }
        ++st.updates;
        st.live_sum += static_cast<std::size_t>(table.count);
        st.max_live = (std::max)(st.max_live, static_cast<std::size_t>(table.count));
        if (engine_faulted()) return true;
        if (!compare("projectile update tick " + std::to_string(tick))) return true;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: projectile_harness <TotalA.exe> <game_dir> [scenarios] [ticks] [first seed]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int scenarios = argc > 3 ? std::atoi(argv[3]) : 50;
    const int ticks = argc > 4 ? std::atoi(argv[4]) : 200;
    const int first = argc > 5 ? std::atoi(argv[5]) : 0;
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[2]);
    ta::UnitDefContext ctx;
    ta::load_unit_context(vfs, ctx);
    const ta::UnitDefTable defs = ta::load_unit_defs(vfs, ctx);
    const ta::WeaponDefTable wdefs = ta::load_weapon_defs(vfs, ta::WeaponDefOptions{});
    std::vector<std::uint8_t> tnt;
    sim::HeightMap hm;
    if (!vfs.read("maps\\Moon Quartet.tnt", tnt) || !ta::tnt_load_heights(tnt, hm)) {
        std::fprintf(stderr, "cannot load the Moon Quartet heights\n");
        return 1;
    }
    // Base cells: heights from the map, the extremes of each cell's four corners.
    std::vector<sim::MapCell> base(static_cast<std::size_t>(hm.width()) * hm.height());
    for (i32 z = 0; z < hm.height(); ++z)
        for (i32 x = 0; x < hm.width(); ++x) {
            sim::MapCell& c = base[static_cast<std::size_t>(z) * hm.width() + x];
            c.height = hm.at(x, z);
            std::uint8_t lo = 255, hi = 0;
            for (i32 dz = 0; dz < 2; ++dz)
                for (i32 dx = 0; dx < 2; ++dx) {
                    const std::uint8_t h = hm.at((std::min)(x + dx, hm.width() - 1), (std::min)(z + dz, hm.height() - 1));
                    lo = (std::min)(lo, h);
                    hi = (std::max)(hi, h);
                }
            c.height_min = lo;
            c.height_max = hi;
        }
    std::vector<sim::WeaponType> pristine;
    for (int s = 0; s < 256; ++s) pristine.push_back(weapon_type_from(wdefs.slots[static_cast<std::size_t>(s)], s));
    if (argc > 6) {
        for (const auto& w : pristine)
            if (w.burst > 0)
                std::printf("%3d %-24s flags %08x burst %d rate %d timer %d vel %d start %d acc %d\n", w.index,
                            wdefs.slots[w.index].name.c_str(), w.flags, w.burst, w.burst_rate, w.weapon_timer, w.velocity,
                            w.start_velocity, w.acceleration);
        return 0;
    }
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
        t.info.model_min_x = d.model_min_x;
        t.info.model_min_y = d.model_min_y;
        t.info.model_min_z = d.model_min_z;
        t.info.model_max_x = d.model_max_x;
        t.info.model_max_z = d.model_max_z;
        t.tick_info.wind_generator = d.wind_generator;
        t.tick_info.heal_time = static_cast<std::uint16_t>(d.heal_time);
        t.tick_info.type_flags = d.type_flags;
        t.tick_info.name = d.unit_name;
        types.push_back(std::move(t));
    }
    harness::Remote r;
    Layout L;
    std::vector<u32> dmaps;
    std::vector<i32> dirty;
    auto start = [&]() {
        dirty.clear();
        if (!r.start(argv[1]) || !setup(r, L, hm, base)) return false;
        dmaps = write_damage_maps(r, L, pristine);
        return true;
    };
    if (!start()) {
        std::fprintf(stderr, "cannot start the original\n");
        return 1;
    }
    Stats st;
    for (int s = first; s < first + scenarios; ++s) {
        if (!run_scenario(r, L, types, hm, base, pristine, dmaps, dirty, ticks, static_cast<unsigned>(s * 7 + 11), st)) {
            ++st.failures;
            r.stop();
            if (!start()) return 1;
        }
        if ((s - first + 1) % 50 == 0)
            std::printf("... %d scenarios, %zu updates, %zu mismatching\n", s - first + 1, st.updates, st.mismatches);
    }
    std::printf("unit types %zu\n", types.size());
    std::printf("scenarios %zu, unit passes %zu, projectile updates %zu (live shots: max %zu, mean %.1f)\n", st.scenarios, st.ticks,
                st.updates, st.max_live, st.updates ? static_cast<double>(st.live_sum) / static_cast<double>(st.updates) : 0.0);
    std::printf("launches: directed %zu, ballistic %zu, ownerless %zu, from motion %zu; chase assignments %zu, detonate packets %zu, kills %zu\n",
                st.directed, st.ballistic, st.free_shots, st.motion_shots, st.chases, st.matches, st.kills);
    std::printf("explosions %zu (water %zu), unit damage events %zu, feature hits %zu, steps with a full table %zu\n", st.explosions,
                st.water_hits, st.damage_events, st.feature_hits, st.full_table);
    std::printf("shot-steps: burst launchers %zu, self-propelled %zu (seeking %zu, cruise %zu), line of sight %zu, beams %zu, timed "
                "ballistic %zu, falling %zu, meteors %zu, inert %zu; chasing a shot %zu; camera following %zu\n",
                st.kind[0], st.kind[1], st.kind[2], st.kind[3], st.kind[4], st.kind[5], st.kind[6], st.kind[7], st.kind[8], st.kind[9],
                st.kind[10], st.kind[11]);
    std::printf("packets sent: damage %zu, kill %zu, free shot %zu, detonate %zu\n", st.packets[0x0B], st.packets[0x0C], st.packets[0x0D],
                st.packets[0x0E]);
    std::printf("fire routines %zu\n", st.fire_routines);
    std::printf("mismatching scenarios %zu, engine faults %zu, original failures %zu (of which predicted crashes %zu)\n", st.mismatches,
                st.faults, st.failures, st.both_crashed);
    return st.mismatches || st.failures != st.both_crashed ? 1 : 0;
}
