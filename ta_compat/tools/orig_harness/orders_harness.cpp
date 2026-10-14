// Differential tester for the unit order lists (sim/orders.h) and ground
// settle (sim/ground_settle.h). Inside a suspended TotalA.exe it builds the
// mission table with the original registration code, then a small battle like
// combat_harness (three players, units of random real types with scripts,
// object states, models and the Moon Quartet terrain). The original order
// processing, order construction/insertion/removal/clearing, paralysis orders,
// ground settle and slope fit run for real; the mission behaviours are replaced
// in the table by one logging stub that plays back a scripted list of results
// (the engine's behaviour hook plays back the same list), and the other
// subsystems are logging stubs. Each step it applies random order operations
// and direct calls, then one unit pass, and compares the stub call log, every
// order of every unit field by field, the units' target-link lists, the unit
// fields settle and the orders touch, scripts, poses, the battle RNG and the
// stale stack word.
//
// Usage: orders_harness <TotalA.exe> <game_dir> [scenarios] [ticks] [first scenario]

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/combat.h"
#include "sim/construction.h"
#include "sim/nano_spray.h"
#include "sim/ground_settle.h"
#include "sim/missions.h"
#include "sim/orders.h"
#include "sim/pose.h"
#include "sim/rng.h"
#include "sim/script_vm.h"
#include "sim/unit.h"
#include "sim/unit_tick.h"
#include "sim/weapons.h"
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
constexpr u32 kCallNamed = 0x004B0940, kUnitPass = 0x0048AD30;
constexpr u32 kApplyDamage = 0x00489BB0, kKillUnit = 0x004864B0;
constexpr u32 kPrimary = 0x0043B7C0, kSecondary = 0x0043BAD0, kSettle = 0x0048A870;
constexpr u32 kRegisterMissions = 0x0043C050, kMissionBegin = 0x00512344, kMissionEnd = 0x00512348;
constexpr u32 kAlloc = 0x004B4F10, kConstruct = 0x0043A0C0, kInsert = 0x0043AC60, kPrepend = 0x0043ACB0, kRemove = 0x00439F80,
              kClear = 0x00439EB0, kClock = 0x004B6340;
constexpr u32 kUnitVtable = 0x004FD698;
constexpr u32 kUnitSize = 0x118, kPlayerSize = 0x14B, kOrderBlock = 0x58;
constexpr int kMaxUnits = 16;

// Handlers of the 68 mission table entries (index = mission kind).
constexpr u32 kHandlers[sim::mission::kCount] = {
    0x439EA0, 0x403010, 0x411F50, 0x412D40, 0x412710, 0x413470, 0x4034A0, 0x403260, 0x402160, 0x403190, 0x401E00, 0x402FC0,
    0x402640, 0x402B70, 0x404270, 0x4030A0, 0x403070, 0x403040, 0x406300, 0x402DA0, 0x406780, 0x406900, 0x4021F0, 0x403F70,
    0x401CC0, 0x403A20, 0x4031D0, 0x402D10, 0x4061A0, 0x4033A0, 0x403160, 0x403160, 0x404AD0, 0x404730, 0x405980, 0x405300,
    0x405740, 0x404DB0, 0x402010, 0x402010, 0x402430, 0x405FE0, 0x406090, 0x403100, 0x4030D0, 0x401C20, 0x4038A0, 0x406AA0,
    0x413BC0, 0x40FBE0, 0x415250, 0x414380, 0x40F2A0, 0x4118E0, 0x413D80, 0x40FA20, 0x410E70, 0x4111B0, 0x414770, 0x414A80,
    0x4152F0, 0x414E70, 0x4103E0, 0x410850, 0x40F7D0, 0x411560, 0x401CE0, 0x401FD0,
};

enum class StubRet { Zero, Variable, Silent, Queue };
struct Stub {
    u32 address;
    int args;
    int id;
    int extra = 0;
    StubRet ret = StubRet::Zero;
    int queue = 0;  // StubRet::Queue: which answer queue
};
// Answer queues of the mission behaviour stubs (index into Layout::queue).
enum : int { qFireAtWill, qIssueAttack, qResolve, qResources, qEnemies, qCreate, qPlace, kQueues };
constexpr u32 kQueueSize = 0x2000;
const Stub kStubs[] = {
    {0x0049E1A0, 1, 1},   // weapons(unit)
    {0x0043DD20, 1, 6},   // motion(unit) [thiscall on the motion object]
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
    {0x00401180, 2, 25, 0, StubRet::Variable},  // pay energy(resource slot, float amount)
    {0x00482910, 4, 26},  // reveal dead unit(&pos, sight, radar, 60)
    {0x004904C0, 1, 27},  // victory: unit destroyed(unit) [thiscall]
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
    {0x004B4F20, 1, 53},  // free(block) [cdecl]
    {0x00456190, 2, 62},  // broadcast script call(unit, function)
    {0x00406F00, 0, 70, 0, StubRet::Silent},  // AI plan command registration (not simulated)
    // Called by the mission behaviours.
    {0x0043B700, 1, 81, 0, StubRet::Queue, qFireAtWill},   // fire-at-will target(unit)
    {0x0043B1F0, 3, 82, 0, StubRet::Queue, qIssueAttack},  // issue attack order(unit, target, forced)
    {0x004011C0, 2, 84, 0, StubRet::Queue, qResources},    // use resources if not stalled(float e, float m) [thiscall]
    {0x0041C150, 1, 85},                                   // build menu counts(unit)
    // Construction (sim/construction.h): the systems its routines reach.
    {0x00485F50, 8, 100, 0, StubRet::Queue, qCreate},      // create unit(player, type, x, y, z, built, layer, wanted)
    {0x0047DB70, 4, 101, 0, StubRet::Queue, qPlace},       // can place(def, exclude, cell, mode)
    {0x00438AD0, 2, 102},                                  // attach edge goal(cell, size) [thiscall on the order]
    {0x00438A00, 3, 103},                                  // attach ring goal(&pos, outer, inner) [thiscall on the order]
    {0x00456290, 7, 105},                                  // broadcast script call(unit, fn, argc, a, b, c, d)
    {0x004AB060, 2, 106},                                  // load a menu layout(gui, name) -> 0
    {0x004199B0, 2, 107},                                  // build menu counts(gui, unit)
    {0x00423550, 3, 108},                                  // destroy feature(x, z, reclaimed)
    {0x00435100, 0, 109, 0, StubRet::Silent},              // network provider type [thiscall] -> 0
    {0x00490520, 1, 110},                                  // victory: unit changes hands(unit) [thiscall]
    {0x00438930, 2, 88},                                   // attach circle goal(&order pos, radius) [thiscall on the order]
};
constexpr int kGoalDetachId = 63, kAttachedDtorId = 64, kHandlerId = 65;
constexpr int kEnemiesId = 80, kResolveId = 83, kNanoId = 86;
const Stub kGoalDetach{0, 1, kGoalDetachId};    // goal vtable +4 (attached) [thiscall]
const Stub kAttachedDtor{0, 1, kAttachedDtorId};  // attached object vtable +0 (flags) [thiscall]
// Custom stubs: visible enemies(player, &pos, radius, 0, &vector) logs 5 args + pos and
// fills the vector from its queue; resolve attack mission(&out, intent, unit, target, pos)
// logs 5 args and writes the queued kind to *out; nano beam(&from, &box, slot) logs both.
const Stub kEnemies{0x0040AD80, 5, kEnemiesId, 3};
const Stub kResolve{0x0043F0E0, 5, kResolveId, 0, StubRet::Queue, qResolve};
constexpr u32 kEnemiesStub = 0x0040AD80, kResolveStub = 0x0043F0E0, kNanoStub = 0x004720D0, kNanoReverseStub = 0x00472200;
constexpr int kNanoReverseId = 104;
constexpr u32 kGs519 = 0x519, kBuilderGuiName = 0x00502844;
// Construction routines called directly.
constexpr u32 kBuildProgress = 0x0041BA60, kDecay = 0x0041BCD0, kFinalize = 0x0041B8D0, kPulse = 0x00438650,
              kCanReclaim = 0x00489960, kYardFits = 0x0047D2E0, kSnap = 0x0047DDC0, kReclaimYield = 0x004237D0,
              kTransfer = 0x00488570, kNanoInit = 0x00473B50, kNanoAdvance = 0x00472EB0, kNanoVtable = 0x004FD5B8, kRand = 0x004E4870;

std::size_t record_size(u32 id) {
    if (id == static_cast<u32>(kHandlerId)) return 5;
    if (id == static_cast<u32>(kEnemiesId)) return 10;
    if (id == static_cast<u32>(kResolveId)) return 7;
    if (id == static_cast<u32>(kNanoId) || id == static_cast<u32>(kNanoReverseId)) return 12;
    if (id == static_cast<u32>(kGoalDetachId) || id == static_cast<u32>(kAttachedDtorId)) return 3;
    for (const Stub& s : kStubs)
        if (static_cast<u32>(s.id) == id) return 2 + static_cast<std::size_t>(s.args + s.extra);
    return 0;
}

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void bytes(std::initializer_list<std::uint8_t> l) { for (auto v : l) b.push_back(v); }
    void align() { while (b.size() % 16) b.push_back(0xCC); }
};

// eax = next value of the answer queue at q ({read pointer, end, values...}), 0 when used up.
void emit_queue_read(Emitter& e, u32 q) {
    e.u8(0x8B); e.u8(0x0D); e.u32le(q);      // mov ecx,[q]
    e.u8(0x3B); e.u8(0x0D); e.u32le(q + 4);  // cmp ecx,[q+4]
    e.bytes({0x72, 0x04});                   // jb have
    e.bytes({0x33, 0xC0});                   // xor eax,eax
    e.bytes({0xEB, 0x09});                   // jmp done
    e.bytes({0x8B, 0x01});                   // have: mov eax,[ecx]
    e.u8(0x83); e.u8(0x05); e.u32le(q); e.u8(0x04);  // add dword [q],4
}

void emit_stub(Emitter& e, const Stub& s, u32 log_ptr_addr, u32 ret_addr, u32 queue_base = 0, bool emit_ret = true) {
    const bool cdecl_ret = s.address == 0x004B4F20;
    if (s.ret != StubRet::Silent) {
        e.u8(0xA1); e.u32le(log_ptr_addr);
        e.u8(0xC7); e.u8(0x00); e.u32le(static_cast<u32>(s.id));
        e.u8(0x89); e.u8(0x48); e.u8(0x04);
        for (int i = 0; i < s.args; ++i) {
            e.u8(0x8B); e.u8(0x54); e.u8(0x24); e.u8(static_cast<std::uint8_t>(4 + 4 * i));
            e.u8(0x89); e.u8(0x50); e.u8(static_cast<std::uint8_t>(8 + 4 * i));
        }
        if (s.extra) {
            e.u8(0x8B); e.u8(0x54); e.u8(0x24); e.u8(0x08);
            for (int k = 0; k < s.extra; ++k) {
                e.u8(0x8B); e.u8(0x4A); e.u8(static_cast<std::uint8_t>(4 * k));
                e.u8(0x89); e.u8(0x48); e.u8(static_cast<std::uint8_t>(8 + 4 * (s.args + k)));
            }
        }
        e.u8(0x83); e.u8(0x05); e.u32le(log_ptr_addr); e.u8(static_cast<std::uint8_t>(4 * (s.args + s.extra + 2)));
    }
    if (s.ret == StubRet::Variable) {
        e.u8(0xA1); e.u32le(ret_addr);
    } else if (s.ret == StubRet::Queue) {
        emit_queue_read(e, queue_base + static_cast<u32>(s.queue) * kQueueSize);
    } else {
        e.u8(0x33); e.u8(0xC0);
    }
    if (!emit_ret) return;
    if (cdecl_ret || s.args == 0) {
        e.u8(0xC3);
    } else {
        e.u8(0xC2); e.u8(static_cast<std::uint8_t>(4 * s.args)); e.u8(0x00);
    }
}

// One scripted mission behaviour step: what the behaviour returns, which wait
// bits it sets (and the wake tick then), and optionally what it leaves in its
// event argument slot.
struct Step {
    u32 result = 2, wait_or = 1, wake = 0x7FFFFFF0u, slot_mode = 0, slot_value = 0;
};

struct Layout {
    u32 caller_pass = 0, caller_one = 0, caller_kill = 0, heap = 0, scenario_heap = 0, bump_ptr = 0, stubs = 0, log_ptr = 0, log_base = 0,
        ret_val = 0, gs = 0, units = 0, defs = 0, models = 0, cells = 0, schema = 0, names = 0, files = 0, pinfo = 0, empty_def = 0,
        script = 0, clock = 0, motion = 0, vtables = 0, attached = 0, handler = 0, queues = 0, weapons = 0, rand_state = 0,
        features = 0, nano = 0;
};
// ret_val block: +0 pay result, +0x10 stale in, +0x14 stale out, +0x18 kill kind, +0x1C kill unit,
// +0x20 one-arg target function, +0x24 its argument, +0x28 EDI for the call.
constexpr u32 kStaleIn = 0x10, kStaleOut = 0x14, kKillKind = 0x18, kKillUnit2 = 0x1C, kOneFn = 0x20, kOneArg = 0x24, kOneEdi = 0x28;

bool setup(harness::Remote& r, Layout& L, const sim::HeightMap& hm, std::string& table_report) {
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
    L.empty_def = L.defs;  // record 0 of the definition table
    L.script = r.alloc(0x10000);
    L.motion = r.alloc(0x2000);
    L.vtables = r.alloc(0x1000);
    L.attached = r.alloc(0x40000);
    L.queues = r.alloc(kQueues * kQueueSize);
    L.weapons = r.alloc(0x1000);
    L.rand_state = r.alloc(0x1000);
    L.features = r.alloc(0x10000);
    L.nano = r.alloc(0x1000);
    if (!L.rand_state || !L.features || !L.nano) return false;
    if (!L.queues || !L.weapons) return false;
    if (!L.heap || !L.stubs || !L.log_base || !L.gs || !L.units || !L.cells || !L.files || !L.pinfo || !L.empty_def || !L.script ||
        !L.motion || !L.vtables || !L.attached)
        return false;
    L.clock = L.ret_val + 0x40;
    Emitter e;
    auto here = [&]() { return L.stubs + static_cast<u32>(e.b.size()); };
    const int size_at[3] = {8, 4, 12};
    const u32 targets[3] = {0x004D83B0, kAlloc, 0x004D84A0};
    for (int k = 0; k < 3; ++k) {
        const u32 at = here();
        e.u8(0xA1); e.u32le(L.bump_ptr);
        e.u8(0x8B); e.u8(0x4C); e.u8(0x24); e.u8(static_cast<std::uint8_t>(size_at[k]));
        e.u8(0x83); e.u8(0xC1); e.u8(0x03); e.u8(0x83); e.u8(0xE1); e.u8(0xFC);
        e.u8(0x01); e.u8(0x0D); e.u32le(L.bump_ptr);
        e.u8(0xC3);
        e.align();
        if (!r.patch_jump(targets[k], at)) return false;
    }
    for (const Stub& s : kStubs) {
        const u32 at = here();
        emit_stub(e, s, L.log_ptr, L.ret_val, L.queues);
        e.align();
        if (!r.patch_jump(s.address, at)) return false;
    }
    // Visible enemies: the vector {allocator, begin, end, capacity} gets the next queued
    // list {count, unit addresses...} (begin = end = 0 for an empty list or none left).
    {
        const u32 at = here();
        const u32 q = L.queues + qEnemies * kQueueSize;
        emit_stub(e, kEnemies, L.log_ptr, L.ret_val, L.queues, false);
        e.u8(0x8B); e.u8(0x0D); e.u32le(q);      // mov ecx,[q]
        e.u8(0x3B); e.u8(0x0D); e.u32le(q + 4);  // cmp ecx,[q+4]
        e.bytes({0x72, 0x06});                   // jb have
        e.bytes({0x33, 0xC0, 0x33, 0xD2});       // xor eax,eax; xor edx,edx
        e.bytes({0xEB, 0x17});                   // jmp store
        e.bytes({0x8B, 0x01});                   // have: mov eax,[ecx] (count)
        e.bytes({0x8D, 0x51, 0x04});             // lea edx,[ecx+4] (begin)
        e.bytes({0x8D, 0x0C, 0x82});             // lea ecx,[edx+eax*4]
        e.u8(0x89); e.u8(0x0D); e.u32le(q);      // mov [q],ecx
        e.bytes({0x85, 0xC0, 0x75, 0x02});       // test eax,eax; jnz +2
        e.bytes({0x33, 0xD2});                   // xor edx,edx
        e.bytes({0x8D, 0x04, 0x82});             // lea eax,[edx+eax*4] (end)
        e.bytes({0x8B, 0x4C, 0x24, 0x14});       // store: mov ecx,[esp+0x14]
        e.bytes({0x89, 0x51, 0x04, 0x89, 0x41, 0x08, 0x89, 0x41, 0x0C});
        e.bytes({0xC2, 0x14, 0x00});             // ret 0x14
        e.align();
        if (!r.patch_jump(kEnemiesStub, at)) return false;
    }
    // Attack mission resolution: the queued kind byte goes to *out, which is returned.
    {
        const u32 at = here();
        emit_stub(e, kResolve, L.log_ptr, L.ret_val, L.queues, false);
        e.bytes({0x8B, 0x54, 0x24, 0x04});  // mov edx,[esp+4]
        e.bytes({0x88, 0x02});              // mov [edx],al
        e.bytes({0x8B, 0xC2});              // mov eax,edx
        e.bytes({0xC2, 0x14, 0x00});        // ret 0x14
        e.align();
        if (!r.patch_jump(kResolveStub, at)) return false;
    }
    // Nano beam: logs the three start and six box words, then the slot.
    {
        const u32 at = here();
        e.u8(0xA1); e.u32le(L.log_ptr);
        e.u8(0xC7); e.u8(0x00); e.u32le(static_cast<u32>(kNanoId));
        e.bytes({0x89, 0x48, 0x04});
        e.bytes({0x8B, 0x54, 0x24, 0x04});  // mov edx,[esp+4]
        for (int k = 0; k < 3; ++k) {
            e.bytes({0x8B, 0x4A, static_cast<std::uint8_t>(4 * k)});
            e.bytes({0x89, 0x48, static_cast<std::uint8_t>(8 + 4 * k)});
        }
        e.bytes({0x8B, 0x54, 0x24, 0x08});  // mov edx,[esp+8]
        for (int k = 0; k < 6; ++k) {
            e.bytes({0x8B, 0x4A, static_cast<std::uint8_t>(4 * k)});
            e.bytes({0x89, 0x48, static_cast<std::uint8_t>(0x14 + 4 * k)});
        }
        e.bytes({0x8B, 0x54, 0x24, 0x0C, 0x89, 0x50, 0x2C});  // slot
        e.u8(0x83); e.u8(0x05); e.u32le(L.log_ptr); e.u8(0x30);
        e.bytes({0x33, 0xC0, 0xC2, 0x0C, 0x00});
        e.align();
        if (!r.patch_jump(kNanoStub, at)) return false;
    }
    // Nano beam from a box to a point: logs the six box and three point words, then the slot.
    {
        const u32 at = here();
        e.u8(0xA1); e.u32le(L.log_ptr);
        e.u8(0xC7); e.u8(0x00); e.u32le(static_cast<u32>(kNanoReverseId));
        e.bytes({0x89, 0x48, 0x04});
        e.bytes({0x8B, 0x54, 0x24, 0x04});  // mov edx,[esp+4]
        for (int k = 0; k < 6; ++k) {
            e.bytes({0x8B, 0x4A, static_cast<std::uint8_t>(4 * k)});
            e.bytes({0x89, 0x48, static_cast<std::uint8_t>(8 + 4 * k)});
        }
        e.bytes({0x8B, 0x54, 0x24, 0x08});  // mov edx,[esp+8]
        for (int k = 0; k < 3; ++k) {
            e.bytes({0x8B, 0x4A, static_cast<std::uint8_t>(4 * k)});
            e.bytes({0x89, 0x48, static_cast<std::uint8_t>(0x20 + 4 * k)});
        }
        e.bytes({0x8B, 0x54, 0x24, 0x0C, 0x89, 0x50, 0x2C});  // slot
        e.u8(0x83); e.u8(0x05); e.u32le(L.log_ptr); e.u8(0x30);
        e.bytes({0x33, 0xC0, 0xC2, 0x0C, 0x00});
        e.align();
        if (!r.patch_jump(kNanoReverseStub, at)) return false;
    }
    // rand(): the C runtime's generator on our own state (the runtime is not initialised).
    {
        const u32 at = here();
        e.u8(0xA1); e.u32le(L.rand_state);
        e.u8(0x69); e.u8(0xC0); e.u32le(214013u);
        e.u8(0x05); e.u32le(2531011u);
        e.u8(0xA3); e.u32le(L.rand_state);
        e.u8(0xC1); e.u8(0xE8); e.u8(0x10);
        e.u8(0x25); e.u32le(0x7FFF);
        e.u8(0xC3);
        e.align();
        if (!r.patch_jump(kRand, at)) return false;
    }
    const u32 goal_detach = here();
    emit_stub(e, kGoalDetach, L.log_ptr, L.ret_val);
    e.align();
    const u32 attached_dtor = here();
    emit_stub(e, kAttachedDtor, L.log_ptr, L.ret_val);
    e.align();
    r.w32(L.vtables + 0x04, goal_detach);    // goal vtable at +0
    r.w32(L.vtables + 0x40, attached_dtor);  // attached object vtable at +0x40
    // Real-time clock: returns the counter, then advances it by 7.
    {
        const u32 at = here();
        e.u8(0xA1); e.u32le(L.clock);
        e.u8(0x83); e.u8(0x05); e.u32le(L.clock); e.u8(0x07);
        e.u8(0xC3);
        e.align();
        if (!r.patch_jump(kClock, at)) return false;
    }
    // Mission behaviour stub (stdcall unit, order, event): logs, then plays back the next Step.
    L.handler = here();
    {
        const u32 H = L.script;
        e.u8(0xA1); e.u32le(L.log_ptr);                                    // mov eax,[log_ptr]
        e.u8(0xC7); e.u8(0x00); e.u32le(static_cast<u32>(kHandlerId));     // mov dword [eax],id
        e.bytes({0x8B, 0x4C, 0x24, 0x04, 0x89, 0x48, 0x04});               // [eax+4] = unit
        e.bytes({0x8B, 0x4C, 0x24, 0x08, 0x89, 0x48, 0x08});               // [eax+8] = order
        e.bytes({0x8B, 0x4C, 0x24, 0x0C, 0x89, 0x48, 0x0C});               // [eax+0xC] = event
        e.bytes({0x8B, 0x4C, 0x24, 0x08, 0x0F, 0xB6, 0x49, 0x04, 0x89, 0x48, 0x10});  // [eax+0x10] = kind
        e.u8(0x83); e.u8(0x05); e.u32le(L.log_ptr); e.u8(0x14);            // add [log_ptr],20
        e.u8(0x8B); e.u8(0x0D); e.u32le(H);                                // mov ecx,[H]
        e.u8(0x3B); e.u8(0x0D); e.u32le(H + 4);                            // cmp ecx,[H+4]
        e.u8(0x72); e.u8(0x07);                                            // jb have (skip 7)
        e.u8(0xB9); e.u32le(H + 8);                                        // mov ecx,H+8 (default step)
        e.u8(0xEB); e.u8(0x07);                                            // jmp after (skip 7)
        e.u8(0x83); e.u8(0x05); e.u32le(H); e.u8(0x14);                    // have: add [H],20
        e.bytes({0x8B, 0x54, 0x24, 0x08});                                 // after: mov edx,[esp+8]
        e.bytes({0x8B, 0x41, 0x04});                                       // mov eax,[ecx+4]
        e.bytes({0x09, 0x42, 0x06});                                       // or [edx+6],eax
        e.bytes({0x85, 0xC0, 0x74, 0x06});                                 // test eax,eax; jz +6
        e.bytes({0x8B, 0x41, 0x08, 0x89, 0x42, 0x0A});                     // [edx+0xA] = wake
        e.bytes({0x8B, 0x41, 0x0C, 0x85, 0xC0, 0x74, 0x07});               // slot mode? jz +7
        e.bytes({0x8B, 0x41, 0x10, 0x89, 0x44, 0x24, 0x0C});               // [esp+0xC] = slot value
        e.bytes({0x8B, 0x01});                                             // mov eax,[ecx]
        e.bytes({0xC2, 0x0C, 0x00});                                       // ret 12
        e.align();
    }
    // Callers that preset the stack word the original's kill routine reads uninitialised,
    // call, and save what is left there afterwards.
    const u32 in = L.ret_val + kStaleIn, out = L.ret_val + kStaleOut;
    auto call_rel = [&](u32 target) {
        const u32 next = here() + 5;
        e.u8(0xE8); e.u32le(target - next);
    };
    L.caller_pass = here();
    e.u8(0xA1); e.u32le(in);
    e.bytes({0x89, 0x44, 0x24, 0xC0});  // mov [esp-0x40],eax
    call_rel(kUnitPass);
    e.bytes({0x8B, 0x44, 0x24, 0xC0});
    e.u8(0xA3); e.u32le(out);
    e.u8(0xC3);
    e.align();
    L.caller_kill = here();
    e.u8(0xA1); e.u32le(in);
    e.bytes({0x89, 0x44, 0x24, 0xE4});  // mov [esp-0x1C],eax
    e.u8(0xFF); e.u8(0x35); e.u32le(L.ret_val + kKillKind);
    e.u8(0xFF); e.u8(0x35); e.u32le(L.ret_val + kKillUnit2);
    call_rel(kKillUnit);
    e.bytes({0x8B, 0x44, 0x24, 0xE4});
    e.u8(0xA3); e.u32le(out);
    e.u8(0xC3);
    e.align();
    // One-argument routine (primary/secondary orders, settle): its entry - 0x14 is 0x1C below this frame.
    L.caller_one = here();
    e.u8(0xA1); e.u32le(in);
    e.bytes({0x89, 0x44, 0x24, 0xE4});                   // mov [esp-0x1C],eax
    e.u8(0x8B); e.u8(0x3D); e.u32le(L.ret_val + kOneEdi);  // mov edi,[edi value]
    e.u8(0xFF); e.u8(0x35); e.u32le(L.ret_val + kOneArg);  // push [arg]
    e.u8(0xFF); e.u8(0x15); e.u32le(L.ret_val + kOneFn);   // call [fn]
    e.bytes({0x8B, 0x44, 0x24, 0xE4});
    e.u8(0xA3); e.u32le(out);
    e.u8(0xC3);
    e.align();
    if (e.b.size() > 0x4000 || !r.write(L.stubs, e.b.data(), e.b.size())) return false;
    r.w32(L.bump_ptr, L.heap);
    r.w32(L.log_ptr, L.log_base);
    r.w32(kGameStatePtr, L.gs);
    r.w32(L.gs + 0x14357, L.units);
    r.w32(L.gs + 0x14287, L.cells);
    r.w32(L.gs + 0x14233, static_cast<u32>(hm.width()));
    r.w32(L.gs + 0x14237, static_cast<u32>(hm.height()));
    r.w32(L.gs + 0x14377, L.models);
    r.w32(L.gs + 0x391E9, L.schema);
    r.w32(L.gs + 0x1439B, L.defs);
    r.w32(L.gs + 0x1426F, L.features);
    std::vector<std::uint8_t> cells(static_cast<std::size_t>(hm.width()) * hm.height() * 13, 0);
    for (i32 z = 0; z < hm.height(); ++z)
        for (i32 x = 0; x < hm.width(); ++x) cells[(static_cast<std::size_t>(z) * hm.width() + x) * 13 + 4] = hm.at(x, z);
    if (!r.write(L.cells, cells.data(), cells.size())) return false;

    // The mission table, built by the original's registration code.
    if (!r.call(kRegisterMissions, 0, {})) return false;
    const u32 begin = r.r32(kMissionBegin), end = r.r32(kMissionEnd);
    const auto names = ta::UnitDefContext::default_mission_names();
    int bad = 0;
    const u32 count = (end - begin) / 0x19;
    if (count != sim::mission::kCount) ++bad;
    for (u32 k = 0; k < count && k < sim::mission::kCount; ++k) {
        const u32 entry = begin + k * 0x19;
        char nm[64] = {};
        r.read(r.r32(entry + 0x15), nm, 63);
        if (names[k] != nm || r.r32(entry + 0x11) != sim::mission::default_flags()[k] || r.r32(entry + 4) != kHandlers[k]) {
            ++bad;
            std::printf("mission table entry %u: %s flags %08x handler %08x\n", k, nm, r.r32(entry + 0x11), r.r32(entry + 4));
        }
        // Behaviours the engine implements run for real; the rest play back the script.
        if (!sim::mission_implemented(static_cast<std::uint8_t>(k))) r.w32(entry + 4, L.handler);
    }
    table_report = "mission table: " + std::to_string(count) + " entries, " + std::to_string(bad) + " differences";
    r.w32(L.log_ptr, L.log_base);
    L.scenario_heap = (r.r32(L.bump_ptr) + 0xFFF) & ~0xFFFu;
    return bad == 0;
}

// Remote unit / player / type / order offsets.
enum : u32 {
    uMotion = 0x00, uOrders = 0x5C, uSecondary = 0x60, uPitch = 0x64, uHeading = 0x66, uBank = 0x68, uPosX = 0x6A, uPosY = 0x6E,
    uPosZ = 0x72, uCarrier = 0x86, uCargoHead = 0x8A, uDef = 0x92, uOwner = 0x96, uScript = 0x9A, uState = 0x9E, uReferrers = 0xA2,
    uDefIndex = 0xA6, uId = 0xA8, uHoverPhase = 0xAA, uKills = 0xB8, uNetDirty = 0xBA, uResource = 0xBC, uMetalMake = 0xD4,
    uResourcePlayer = 0xEC, uLastAttacker = 0xF0, uLastAttackerSlot = 0xF4, uLastDamageKind = 0xF5, uHealth = 0xF6, uPrevHealth = 0xF7,
    uGroup = 0xAC, uDamagedTimer = 0xFA, uHoldoff = 0xFB, uOwnerSlot = 0xFF, uBuildLeft = 0x104, uHp = 0x108, uActivate = 0x10E, uScriptFlags = 0x10F,
    uFlags = 0x110,
    pActive = 0x00, pNetId = 0x04, pInfo = 0x27, pFirst = 0x67, pLast = 0x6B, pController = 0x73, pUnitsKilled = 0xFC,
    pUnitsLost = 0xFE, pCommandersKilled = 0x104, pCommandersLost = 0x106, pSharesVision = 0x129, pAlive = 0x144, pState = 0x146,
    pFlags = 0x149,
    dName = 0x20, dModelMaxY = 0x16E, dEnergyCost = 0x186, dMetalCost = 0x18A, dCob = 0x18E, dMaxVelocity = 0x192,
    dFootprint = 0x14A, dYardMap = 0x14E, dBuildOptions = 0x156, dModelMinY = 0x162, dAvgSize = 0x182,
    dBuildDistance = 0x212, dMaxSlope = 0x228, dMaxWaterDepth = 0x1BE, dMinWaterDepth = 0x1C0, dBmCode = 0x22F, uFootprint = 0x7E,
    uCell = 0x76, uEnergyMake = 0xBC,
    dDamageModifier = 0x1AA, dWindGen = 0x1D2, dBuildTime = 0x1EA, dMaxDamage = 0x1FA, dHealTime = 0x200, dWaterline = 0x22C,
    dDefaultMission = 0x230, dTypeFlags = 0x241, kDefSize = 0x249, dCanDo = 0x245, dWorkerTime = 0x1FE, dModelMinX = 0x15E,
    dModelMinZ = 0x166, dModelMaxX = 0x16A, dModelMaxZ = 0x172, uCloakHoldoff = 0xB0, pAllied = 0x108, gGravity = 0x14263,
    gUnitsEnd = 0x1435B,
    wVelocity = 0x68, wEnergy = 0xC0, wMetal = 0xC4, wMinAngle = 0xC8, wRange = 0xDC, wReload = 0xE4, wFlags = 0x111,
    gLocalPlayer = 0x2A42, gLocalView = 0x2A43, gMapFlags = 0x14281, gDifficulty = 0x37EEE, gGameMode = 0x37EF6, gFlash = 0x37F06,
    gCommanderNames = 0x37F5F, kSideSize = 0x232,
    oKind = 0x04, oStep = 0x05, oWait = 0x06, oWake = 0x0A, oUnit = 0x0E, oLink = 0x12, oPos = 0x22, oScratch = 0x2E, oParams = 0x36,
    oFlags = 0x42, oCreated = 0x46, oNext = 0x4A, oEvents = 0x4E, oAttached = 0x52,
    mGoal = 0x00, mSpeed = 0x20, mStart = 0x2A, mGoalObject = 0x10,
};

struct LoadedType {
    const ta::UnitDefRecord* def = nullptr;
    std::vector<std::uint8_t> model_file, cob_file;
    sim::PoseModel pose_model;
    ta::CobFile cob;
    sim::UnitTypeInfo info;
    sim::UnitTickTypeInfo tick_info;
    sim::GroundSettleType settle;
    u32 remote_model = 0, remote_cob = 0, remote_def = 0;
};

u32 fbits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

// Real behaviour steps by mission kind and result (results 0..9, 10 = other).
std::array<std::array<std::size_t, 11>, sim::mission::kCount> g_real_runs{};
std::size_t g_event_slot_writes = 0;  // real behaviour steps that changed their event argument

struct MotionInfo {
    u32 block = 0;
    u32 goal_current = 0;
    i32 speed = 0;
    u32 start = 0;
};

struct World : sim::UnitWorld, sim::UnitTickSystems, sim::CombatSystems, sim::OrderSystems, sim::GroundSettleSystems,
               sim::MissionSystems, sim::ConstructionSystems {
    std::vector<sim::Unit>* units = nullptr;
    std::vector<sim::UnitTickState>* ticks = nullptr;
    std::vector<sim::PlayerSlot>* players = nullptr;
    sim::Combat* combat = nullptr;
    sim::OrderContext* orders = nullptr;
    sim::Missions* missions = nullptr;
    sim::UnitTickEnvironment* env = nullptr;
    u32 units_base = 0;  // the original's unit table (answers hold unit addresses)
    std::array<std::vector<u32>, kQueues> queue;
    std::array<std::size_t, kQueues> queue_at{};
    std::size_t behaviour_calls = 0;  // since the last deal of answers
    std::vector<sim::GroundSettleType>* settle_types = nullptr;  // by type index
    const sim::HeightMap* hm = nullptr;
    sim::BattleRng rng;
    sim::Lcg15 crt;  // the C runtime rand() stream
    std::vector<u32> log;
    std::vector<MotionInfo> motions;
    std::vector<Step> steps;
    Step fallback;  // once the script runs out: continue, asleep until a later step
    std::size_t step_at = 0;
    u32 next_address = 0, clock = 0, pay_result = 0;
    bool kill_flash = false;
    std::map<u32, sim::MissionOrder*> live;
    std::set<u32> all_orders;

    ~World() override {
        for (auto& [a, o] : live) delete o;
    }
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
    void weapons(sim::Unit& u, u32&) override { rec(1, 0, {tok(u)}); }
    void damage(sim::Unit* a, sim::Unit& t, i32 amount, i32 kind, i32 heading) override {
        sim::apply_damage(*combat, a, t, amount, kind, heading);
    }
    void repair_heal(sim::Unit& w, sim::Unit& t, float rate) override { sim::repair_heal(*combat, w, t, rate); }
    void primary_orders(sim::Unit& u, u32& stale) override {
        missions->caller = sim::MissionCaller::Primary;
        sim::run_primary_orders(*orders, u);
        stale = env->addresses.player(env->pass_slot);
    }
    void secondary_orders(sim::Unit& u, u32& stale) override {
        if (std::getenv("OH_TRACE")) std::printf("  secondary %u\n", u.id);
        missions->caller = sim::MissionCaller::Secondary;
        sim::run_secondary_orders(*orders, u, stale);
    }
    void motion(sim::Unit& u, u32&) override { rec(6, 0x7E000000u | u.id, {tok(u)}); }
    void settle_height(sim::Unit& u, u32& stale) override { if (std::getenv("OH_TRACE")) std::printf("  settle %u\n", u.id); settle(u, stale); }
    void settle(sim::Unit& u, u32& stale) {
        const sim::UnitTickState& t = (*ticks)[u.id];
        sim::settle_on_ground(u, t.has_motion, (*settle_types)[t.type_index], env->sea_level, env->game_tick, *hm, *this, stale);
    }
    void killed(sim::Unit& u, std::uint8_t kind, u32& stale) override { if (std::getenv("OH_TRACE")) std::printf("  kill %u\n", u.id); sim::kill_unit(*combat, u, kind, stale); }
    void broadcast_player_units(int player, u32&) override { rec(9, 0, {0x20000000u | static_cast<u32>(player)}); }
    void camera_follow_step(u32&) override {}
    // CombatSystems
    void retarget(sim::Unit* a, sim::Unit& v, std::uint16_t amount) override { rec(20, 0, {a ? tok(*a) : 0u, tok(v), amount}); }
    void add_paralysis(sim::Unit& v, std::uint16_t amount) override { sim::add_paralysis(*orders, v, amount); }
    bool pay_energy(sim::Unit& w, float amount) override {
        rec(25, 0, {0x41000000u | w.id, fbits(amount)});
        return pay_result != 0;
    }
    void stats_activity(i32 kind) override { rec(21, 0, {static_cast<u32>(kind)}); }
    void network_send(i32 net, const std::uint8_t* p, i32 size) override {
        u32 d[3] = {};
        std::memcpy(d, p, static_cast<std::size_t>(size < 12 ? size : 12));
        rec(16, 0, {static_cast<u32>(net), 0, static_cast<u32>(size), d[0], d[1], d[2]});
    }
    void reveal_dead_unit(sim::Unit& u) override {
        const auto height = static_cast<std::int16_t>(static_cast<u32>(u.type ? u.type->model_max_y : 0) >> 16);
        rec(26, 0, {0x42000000u | u.id, 0, static_cast<u32>(static_cast<i32>(height)), 60});
    }
    void victory_unit_destroyed(sim::Unit& u) override { rec(27, 0, {tok(u)}); }
    void clear_orders(sim::Unit& u) override { sim::clear_orders(*orders, u, true); }
    void forget_speech(sim::Unit& u) override { rec(29, 0, {tok(u)}); }
    void leave_group(sim::Unit& u) override { rec(30, 0, {tok(u), 0xFFFFFFFFu}); }
    void remove_projectiles(sim::Unit& u) override { rec(31, 0, {tok(u)}); }
    void detach_from_carrier(sim::Unit& u) override { rec(32, 0, {tok(u), 0, 0xFFFFFFFFu, 1}); }
    void clear_footprint(sim::Unit& u) override { rec(33, 0, {tok(u)}); }
    void forget_fog(sim::Unit& u) override { rec(34, 0, {tok(u)}); }
    void kill_counted(std::uint8_t slot, sim::Unit& v) override {
        if (kill_flash) rec(35, 0, {slot, owner_of(v).state});
    }
    void detonate(sim::Unit& u, bool self_destruct) override { rec(36, 0, {tok(u), self_destruct ? 1u : 0u}); }
    void spawn_wreck(sim::Unit& u, i32 level, bool burn) override { rec(37, 0, {tok(u), static_cast<u32>(level), burn ? 1u : 0u}); }
    void release_unit(sim::Unit& u) override { rec(38, 0, {tok(u)}); }
    void release_motion(sim::Unit&) override {}
    void player_out_of_units(int) override {}
    void commander_died(int player) override {
        rec(41, 0, {1});
        rec(42, 0, {static_cast<u32>(player)});
    }
    // OrderSystems
    std::int32_t run_mission(sim::Unit& u, sim::MissionOrder& o, std::uint32_t& event) override {
        const std::uint8_t kind = o.kind;
        if (++behaviour_calls > 100000) {  // an order that never sleeps: the original hangs too
            if (missions->fault.empty()) missions->fault = "endless behaviour loop";
            return 7;
        }
        const std::uint32_t event_in = event;
        const std::uint8_t step_in = o.step;
        const std::int32_t res = sim::run_mission_behaviour(*missions, u, o, event);
        if (std::getenv("OH_TRACE"))
            std::printf("    unit %u order %08x kind %u step %u event %x -> %d (wait %x events %x)\n", u.id, o.address, kind, step_in, event_in, res,
                        o.wait, o.events);
        if (event != event_in && sim::mission_implemented(kind)) ++g_event_slot_writes;
        if (sim::mission_implemented(kind)) ++g_real_runs[kind][static_cast<u32>(res) < 10 ? static_cast<std::size_t>(res) : 10];
        return res;
    }
    // MissionSystems
    std::int32_t run_other(sim::Unit& u, sim::MissionOrder& o, std::uint32_t& event) override {
        log.insert(log.end(), {static_cast<u32>(kHandlerId), tok(u), o.address, event, o.kind});
        const Step& s = step_at < steps.size() ? steps[step_at++] : fallback;
        o.wait |= s.wait_or;
        if (s.wait_or) o.wake_tick = s.wake;
        if (s.slot_mode) event = s.slot_value;
        return static_cast<std::int32_t>(s.result);
    }
    // The original's weapon-target routines run for real (0x48A0F0, 0x489800(3)).
    void clear_weapon_target(sim::Unit& u, int slot) override { sim::clear_weapon_target(u, slot); }
    void release_weapon_targets(sim::Unit& u) override { sim::release_weapon_targets(u, 3); }
    void release_attached(sim::Unit& u, sim::MissionOrder& o) override {
        const MotionInfo& m = motions[u.id];
        if (m.goal_current == 0 || m.goal_current != o.attached) {
            if (o.attached) rec(kAttachedDtorId, o.attached, {1});
        } else {
            rec(kGoalDetachId, 0x7D000000u | u.id, {0});
            rec(kAttachedDtorId, o.attached, {1});
        }
        o.attached = 0;
    }
    void broadcast_script_call(sim::Unit& u, std::int16_t fn) override {
        rec(62, 0, {tok(u), static_cast<u32>(static_cast<i32>(fn))});
    }
    sim::MissionOrder* allocate_order() override {
        auto* o = new sim::MissionOrder;
        o->address = next_address;
        next_address += kOrderBlock;
        live[o->address] = o;
        all_orders.insert(o->address);
        return o;
    }
    void free_order(sim::MissionOrder* o) override {
        rec(53, 0, {o->address});
        live.erase(o->address);
        delete o;
    }
    u32 next_answer(int q) {
        const auto& v = queue[static_cast<std::size_t>(q)];
        std::size_t& at = queue_at[static_cast<std::size_t>(q)];
        return at < v.size() ? v[at++] : 0u;
    }
    sim::Unit* unit_at(u32 address) {
        if (!address) return nullptr;
        return &(*units)[(address - units_base) / kUnitSize];
    }
    void speech(sim::Unit& u, std::uint32_t id, sim::SpeechText text) override {
        static constexpr u32 kTexts[] = {0,          0x005012DC, 0x005016AC, 0x005012CC, 0x00501340, 0x00501310, 0x005012F8,
                                         0x005015A4, 0x00501580, 0x0050155C, 0x00501544, 0x00501530, 0x005015BC, 0x005010CC,
                                         0x00501638, 0x0050164C, 0x00501388, 0x005015D0, 0x00501618, 0x005015E0, 0x005013B8};
        rec(14, 0, {tok(u), id, kTexts[static_cast<int>(text)]});
    }
    void enemies_in_radius(int player, const std::array<i32, 3>& pos, i32 radius, std::vector<sim::Unit*>& out) override {
        rec(kEnemiesId, 0, {static_cast<u32>(player), 0, static_cast<u32>(radius), 0, 0, static_cast<u32>(pos[0]),
                            static_cast<u32>(pos[1]), static_cast<u32>(pos[2])});
        const auto& v = queue[qEnemies];
        std::size_t& at = queue_at[qEnemies];
        if (at >= v.size()) return;
        const u32 n = v[at++];
        for (u32 k = 0; k < n; ++k) out.push_back(unit_at(v[at++]));
    }
    sim::Unit* fire_at_will_target(sim::Unit& u) override {
        rec(81, 0, {tok(u)});
        return unit_at(next_answer(qFireAtWill));
    }
    bool issue_attack_order(sim::Unit& u, sim::Unit& t, bool forced) override {
        rec(82, 0, {tok(u), tok(t), forced ? 1u : 0u});
        return next_answer(qIssueAttack) != 0;
    }
    std::uint8_t resolve_mission(sim::OrderIntent intent, sim::Unit& u, sim::Unit* t, const std::array<i32, 3>*) override {
        rec(kResolveId, 0, {0, static_cast<u32>(intent), tok(u), t ? tok(*t) : 0u, 0});
        return static_cast<std::uint8_t>(next_answer(qResolve));
    }
    bool use_resources_if_not_stalled(sim::Unit& u, float e, float m) override {
        rec(84, 0x41000000u | u.id, {fbits(e), fbits(m)});
        return next_answer(qResources) != 0;
    }
    void build_counts_changed(sim::Unit& u) override { rec(85, 0, {tok(u)}); }
    void unfinished_decay(sim::Unit& u, i32 n) override { sim::unfinished_decay(*missions, u, n); }
    void display_changed(sim::Unit& u) override { rec(15, 0, {tok(u)}); }
    void nano_beam(const std::array<i32, 3>& from, const std::array<i32, 6>& box) override {
        log.push_back(static_cast<u32>(kNanoId));
        log.push_back(0);
        for (i32 v : from) log.push_back(static_cast<u32>(v));
        for (i32 v : box) log.push_back(static_cast<u32>(v));
        log.push_back(6);
    }
    void attach_circle_goal(sim::Unit&, sim::MissionOrder& o, const std::array<i32, 3>&, i32 radius) override {
        rec(88, o.address, {o.address + 0x22, static_cast<u32>(radius)});  // (always the order's own position)
    }
    void drop_goal_object(sim::Unit& u, sim::MissionOrder& o) override {
        rec(kGoalDetachId, 0x7D000000u | u.id, {0});
        rec(kAttachedDtorId, o.attached, {1});
        o.attached = 0;
    }
    // ConstructionSystems (the original's matching routines are logging stubs)
    u32 defs_base = 0, gs_base = 0, bucket_base = 0;
    u32 bucket_addr(int k) const { return bucket_base + static_cast<u32>(k) * 16; }
    bool tick_has_motion(int k) const { return (*ticks)[static_cast<std::size_t>(k)].has_motion; }
    sim::Unit* create_unit(int player, std::uint16_t type, const std::array<i32, 3>& pos, bool built, u32 layer) override {
        rec(100, 0, {static_cast<u32>(player) & 0xFF, type, static_cast<u32>(pos[0]), static_cast<u32>(pos[1]), static_cast<u32>(pos[2]),
                     built ? 1u : 0u, layer, 0});
        return unit_at(next_answer(qCreate));
    }
    bool can_place(std::uint16_t type, std::uint16_t exclude, std::int16_t x, std::int16_t z, u32 mode) override {
        rec(101, 0, {defs_base + type * static_cast<u32>(kDefSize), exclude, static_cast<std::uint16_t>(x) | static_cast<u32>(static_cast<std::uint16_t>(z)) << 16, mode});
        return next_answer(qPlace) != 0;
    }
    void transport_attach(sim::Unit& cargo, sim::Unit* t, std::uint8_t piece, std::uint8_t layer) override {
        rec(32, 0, {tok(cargo), t ? tok(*t) : 0u, piece == 0xFF ? 0xFFFFFFFFu : piece, layer});
    }
    void attach_edge_goal(sim::Unit&, sim::MissionOrder& o, u32 cell, u32 size) override { rec(102, o.address, {cell, size}); }
    void attach_ring_goal(sim::Unit&, sim::MissionOrder& o, const std::array<i32, 3>& pos, i32 outer, i32 inner) override {
        u32 at = 0;
        for (const sim::Unit& u : *units)
            if (&u.pos == &pos) at = 0x42000000u | u.id;
        rec(103, o.address, {at, static_cast<u32>(outer), static_cast<u32>(inner)});
    }
    void nano_beam_reverse(const std::array<i32, 6>& box, const std::array<i32, 3>& to) override {
        log.push_back(static_cast<u32>(kNanoReverseId));
        log.push_back(0);
        for (i32 v : box) log.push_back(static_cast<u32>(v));
        for (i32 v : to) log.push_back(static_cast<u32>(v));
        log.push_back(6);
    }
    void kill_feature(i32 x, i32 z, bool reclaimed) override { rec(108, 0, {static_cast<u32>(x), static_cast<u32>(z), reclaimed ? 1u : 0u}); }
    void victory_unit_transferred(sim::Unit& u) override { rec(110, 0, {tok(u)}); }
    void broadcast_script_call_args(sim::Unit& u, std::int16_t fn, i32 argc, i32 a) override {
        rec(105, 0, {tok(u), static_cast<u32>(static_cast<i32>(fn)), static_cast<u32>(argc), static_cast<u32>(a), 0, 0, 0});
    }
    void builder_menu(sim::Unit&) override { rec(106, 0, {gs_base + kGs519, kBuilderGuiName}); }
    void build_menu_refresh(sim::Unit& u) override { rec(107, 0, {gs_base + kGs519, tok(u)}); }
    void orders_panel_changed() override {}
    i32 network_provider() override { return 0; }
    // Transport attach applied directly: one spatial bucket list per unit (Unit::bucket = its index).
    struct Links final : sim::TransportLinks {
        World& w;
        std::vector<sim::Unit*> heads;
        explicit Links(World& world) : w(world) {}
        void bucket_unlink(sim::Unit& u) override {
            sim::Unit** link = &heads[static_cast<std::size_t>(u.bucket)];
            while (*link && *link != &u) link = &(*link)->cargo_next;
            if (*link) *link = u.cargo_next;
            u.cargo_next = nullptr;
        }
        void bucket_prepend(sim::Unit& u) override {
            u.cargo_next = heads[static_cast<std::size_t>(u.bucket)];
            heads[static_cast<std::size_t>(u.bucket)] = &u;
        }
        std::uint32_t type_flags(const sim::Unit& u) override {
            const sim::UnitTickTypeInfo* t = (*w.ticks)[u.id].tick_type;
            return t ? t->type_flags : 0;
        }
        void selection_changed() override {}
    } links{*this};
    // GroundSettleSystems
    std::int32_t motion_speed(sim::Unit& u) override { return motions[u.id].speed; }
    std::uint32_t motion_start_tick(sim::Unit& u) override { return motions[u.id].start; }
    std::uint32_t clock_ticks() override {
        const u32 v = clock;
        clock += 7;
        return v;
    }
};

struct Stats {
    std::size_t scenarios = 0, ticks = 0, ops = 0, handler_calls = 0, orders_made = 0, orders_freed = 0, settles = 0,
                deaths = 0, paralysis = 0, mismatches = 0, failures = 0, faults = 0, placements = 0, placements_ok = 0,
                reclaimed_features = 0, nano_streams = 0, nano_steps = 0, transports = 0;
    std::map<std::string, std::size_t> by_op, secondary_stale, pass_stale;
    std::map<u32, std::size_t> settle_paths;
};

std::string random_case(std::string s, std::mt19937& g) {
    for (char& ch : s)
        if (g() % 2) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : (ch >= 'A' && ch <= 'Z' ? ch + 32 : ch));
    return s;
}

bool run_scenario(harness::Remote& r, const Layout& L, std::vector<LoadedType>& types, const sim::HeightMap& hm, int ticks_to_run,
                  unsigned seed, Stats& st) {
    std::mt19937 g(seed);
    r.w32(L.bump_ptr, L.scenario_heap);
    std::vector<std::uint8_t> zero(0x40000, 0);
    r.write(L.gs + 0x1B63, zero.data(), 10 * kPlayerSize);
    r.write(L.units, zero.data(), (kMaxUnits + 2) * kUnitSize);
    r.write(L.gs + gCommanderNames, zero.data(), 4 * kSideSize);
    r.write(L.motion, zero.data(), 0x2000);
    r.w32(L.gs + 0x14353, 0);

    // Unit types (random settle-related values; models and scripts stay real).
    const int ntypes = 3;
    std::vector<int> pick;
    std::vector<sim::GroundSettleType> settle_types(ntypes + 1);
    std::vector<sim::MissionTypeInfo> mission_types(ntypes + 1);
    std::vector<sim::ConstructionTypeInfo> con_types(ntypes + 1);
    u32 file_at = L.files;
    for (int k = 0; k < ntypes; ++k) {
        std::size_t ti = g() % types.size();
        while (std::find(pick.begin(), pick.end(), static_cast<int>(ti)) != pick.end()) ti = g() % types.size();  // distinct types
        LoadedType& t = types[ti];
        pick.push_back(static_cast<int>(&t - &types[0]));
        t.remote_model = file_at;
        file_at += (static_cast<u32>(t.model_file.size()) + 15) & ~15u;
        r.write(t.remote_model, t.model_file.data(), t.model_file.size());
        if (!r.call(kRelocate, 0, {t.remote_model, t.remote_model}) || !r.call(kNegateXZ, t.remote_model, {t.remote_model})) {
            std::printf("relocate failed\n");
            return false;
        }
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
        // Random values for the settle variants and the default mission.
        t.tick_info.type_flags = t.def->type_flags;
        const u32 variants[] = {0, 0x1000, 0x80000, 0x100000, 0x101000, 0x81000, 0x180000};
        if (g() % 4) t.tick_info.type_flags = (t.tick_info.type_flags & ~0x181000u) | variants[g() % 7];
        if (g() % 3 == 0) t.tick_info.type_flags ^= 0x40u;   // builder
        if (g() % 3 == 0) t.tick_info.type_flags ^= 0x800u;  // can fly
        // Mission abilities (on/off, attack, cloak, self-destruct countdown) and worker time.
        sim::MissionTypeInfo& mt = mission_types[static_cast<std::size_t>(k + 1)];
        const u32 countdowns[] = {5, 5, 5, 0, 1, 3, 6, 7};
        mt.abilities = (g() % 2 ? 1u << 2 : 0u) | (g() % 2 ? 1u << 4 : 0u) | (g() % 2 ? 1u << 13 : 0u) |
                       (countdowns[g() % 8] << 20) | static_cast<u32>(g() & 0x9F03u);
        mt.worker_time = static_cast<std::uint16_t>(g() % 4 == 0 ? g() : g() % 400);
        t.info.model_min_x = -static_cast<i32>(g() % 0x200000);
        t.info.model_min_z = -static_cast<i32>(g() % 0x200000);
        t.info.model_max_x = static_cast<i32>(g() % 0x200000);
        t.info.model_max_z = static_cast<i32>(g() % 0x200000);
        t.settle.type_flags = t.tick_info.type_flags;
        t.settle.waterline = static_cast<std::uint8_t>(g() % 3 == 0 ? t.def->waterline : g() % 256);
        t.settle.max_velocity = g() % 3 == 0 ? t.def->max_velocity : static_cast<i32>(g() % 0x80000);
        if (t.settle.max_velocity >= -1 && t.settle.max_velocity <= 1) t.settle.max_velocity = 0x20000;  // the original divides by zero
        if (g() % 10 == 0) t.settle.max_velocity = -t.settle.max_velocity;
        t.tick_info.default_mission = static_cast<std::uint8_t>(g() % 2 ? 0 : g() % 3 == 0 ? t.def->default_mission : 1 + g() % 67);
        settle_types[static_cast<std::size_t>(k + 1)] = t.settle;
        t.remote_def = L.defs + static_cast<u32>(k + 1) * kDefSize;
        std::vector<std::uint8_t> d(kDefSize, 0);
        auto put = [&](u32 off, const void* p, std::size_t n) { std::memcpy(&d[off], p, n); };
        put(dName, t.tick_info.name.c_str(), (std::min)(t.tick_info.name.size() + 1, std::size_t{32}));
        put(dCob, &t.remote_cob, 4);
        put(dModelMaxY, &t.info.model_max_y, 4);
        put(dEnergyCost, &t.tick_info.build_cost_energy, 4);
        put(dMetalCost, &t.tick_info.build_cost_metal, 4);
        put(dMaxVelocity, &t.settle.max_velocity, 4);
        put(dDamageModifier, &t.tick_info.damage_modifier, 4);
        put(dWindGen, &t.tick_info.wind_generator, 4);
        put(dBuildTime, &t.tick_info.build_time, 4);
        put(dMaxDamage, &t.info.max_damage, 4);
        put(dHealTime, &t.tick_info.heal_time, 2);
        put(dWaterline, &t.settle.waterline, 1);
        put(dDefaultMission, &t.tick_info.default_mission, 1);
        put(dTypeFlags, &t.tick_info.type_flags, 4);
        put(dCanDo, &mt.abilities, 4);
        put(dWorkerTime, &mt.worker_time, 2);
        put(dModelMinX, &t.info.model_min_x, 4);
        put(dModelMinZ, &t.info.model_min_z, 4);
        put(dModelMaxX, &t.info.model_max_x, 4);
        put(dModelMaxZ, &t.info.model_max_z, 4);
        // Construction data: footprint, yard map, build distance, placement limits, costs.
        sim::ConstructionTypeInfo& ct = con_types[static_cast<std::size_t>(k + 1)];
        ct.footprint_x = static_cast<std::int16_t>(1 + g() % 4);
        ct.footprint_z = static_cast<std::int16_t>(1 + g() % 4);
        ct.movement = static_cast<std::uint8_t>(g() % 2);
        ct.build_distance = static_cast<std::uint16_t>(g() % 4 == 0 ? g() % 40 : 60 + g() % 400);
        const i32 avg = static_cast<i32>(g() % 0x400000);
        ct.reclaim_reach = static_cast<std::int16_t>(static_cast<u32>(avg) >> 16);
        ct.has_build_options = g() % 4 != 0;
        ct.waterline = t.settle.waterline;
        ct.max_slope = static_cast<std::uint8_t>(g() % 3 == 0 ? g() : g() % 30);
        ct.max_water_depth = static_cast<std::int16_t>(g() % 3 == 0 ? 0 : g() % 200);
        ct.min_water_depth = static_cast<std::int16_t>(g() % 3 == 0 ? -10000 : static_cast<i32>(g() % 60) - 30);
        ct.yard_map.resize(static_cast<std::size_t>(ct.footprint_x * ct.footprint_z));
        static const std::uint8_t kYard[] = {0x00, 0x35, 0x8F, 0x2B, 0x31, 0x2D, 0x6F, 0x2F, 0x37, 0x29};
        for (auto& y : ct.yard_map) y = g() % 6 == 0 ? static_cast<std::uint8_t>(g()) : kYard[g() % 10];
        const u32 yard_at = L.defs + 0x8000 + static_cast<u32>(k) * 0x100;
        r.write(yard_at, ct.yard_map.data(), ct.yard_map.size());
        const u32 options = ct.has_build_options ? L.defs + 0xA000 : 0u;
        const u32 fp = static_cast<std::uint16_t>(ct.footprint_x) | static_cast<u32>(static_cast<std::uint16_t>(ct.footprint_z)) << 16;
        const std::uint16_t bd = ct.build_distance;
        t.info.model_min_y = -static_cast<i32>(g() % 0x100000);
        t.tick_info.build_time = static_cast<i32>(g() % 8 == 0 ? 0 : 1 + g() % 20000);
        t.tick_info.build_cost_energy = g() % 8 == 0 ? 0.0f : static_cast<float>(g() % 200000) / 7.0f;
        t.tick_info.build_cost_metal = g() % 8 == 0 ? 0.0f : static_cast<float>(g() % 20000) / 3.0f;
        put(dEnergyCost, &t.tick_info.build_cost_energy, 4);
        put(dMetalCost, &t.tick_info.build_cost_metal, 4);
        put(dBuildTime, &t.tick_info.build_time, 4);
        put(dFootprint, &fp, 4);
        put(dYardMap, &yard_at, 4);
        put(dBuildOptions, &options, 4);
        put(dModelMinY, &t.info.model_min_y, 4);
        put(dAvgSize, &avg, 4);
        put(dBuildDistance, &bd, 2);
        put(dMaxSlope, &ct.max_slope, 1);
        put(dMaxWaterDepth, &ct.max_water_depth, 2);
        put(dMinWaterDepth, &ct.min_water_depth, 2);
        put(dWaterline, &ct.waterline, 1);
        put(dBmCode, &ct.movement, 1);
        r.write(t.remote_def, d.data(), d.size());
        r.w32(L.models + static_cast<u32>(k + 1) * 4, t.remote_model);
    }
    std::vector<std::uint8_t> ed(kDefSize, 0);
    r.write(L.empty_def, ed.data(), ed.size());
    sim::UnitTypeInfo empty_type;
    empty_type.max_damage = 0;
    sim::UnitTickTypeInfo empty_tick;
    empty_tick.damage_modifier = 0;

    // Players: slots 0..2 playing (controllers 1, 2, 3), a few idle slots.
    std::vector<sim::PlayerSlot> players(10);
    std::vector<std::array<std::uint8_t, 16>> allied(10);
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
        r.write(at + pFlags, &s.flags, 2);
        r.write(at + pSharesVision, s.shares_vision.data(), 10);
        // Alliance bytes by the other player's state index (states 0..3 are used).
        auto& al = allied[static_cast<std::size_t>(p)];
        for (int k = 0; k < 4; ++k) al[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(g() % 3 == 0 ? 1 + g() % 255 : 0);
        r.write(at + pAllied, al.data(), 4);
        r.w32(at + pFirst, L.units + static_cast<u32>(playing ? s.first_unit : 1) * kUnitSize);
        r.w32(at + pLast, L.units + static_cast<u32>(playing ? s.last_unit : 0) * kUnitSize);
    }

    // Weapon types for the slots (range checks and stockpiles read them).
    std::vector<sim::WeaponType> wtypes(4);
    for (int w = 0; w < 4; ++w) {
        sim::WeaponType& wt = wtypes[static_cast<std::size_t>(w)];
        wt.flags = (g() % 3 == 0 ? sim::weapon_flags::kBallistic : 0u) | (g() % 4 == 0 ? sim::weapon_flags::kWaterWeapon : 0u) |
                   (g() % 4 == 0 ? 1u << 17 : 0u) | static_cast<u32>(g() & 0x00080001u);
        wt.velocity = static_cast<i32>(g() % 0x1000000);
        wt.min_barrel_angle = static_cast<float>(g() % 1000) / 1000.0f - 0.3f;
        wt.range = static_cast<i32>(g() % 8 == 0 ? g() % 0x10000 : g() % 1500);
        wt.reload_time = static_cast<std::uint16_t>(g() % 8 == 0 ? 0 : g() % 4 == 0 ? g() : g() % 300);
        wt.energy_per_shot = g() % 5 == 0 ? 0.0f : static_cast<float>(g() % 100000) / 7.0f;
        wt.metal_per_shot = g() % 5 == 0 ? 0.0f : static_cast<float>(g() % 10000) / 3.0f;
        const u32 at = L.weapons + static_cast<u32>(w) * 0x200;
        std::vector<std::uint8_t> wd(0x200, 0);
        auto put = [&](u32 off, const void* p, std::size_t n) { std::memcpy(&wd[off], p, n); };
        put(wFlags, &wt.flags, 4);
        put(wVelocity, &wt.velocity, 4);
        put(wMinAngle, &wt.min_barrel_angle, 4);
        put(wRange, &wt.range, 4);
        put(wReload, &wt.reload_time, 2);
        put(wEnergy, &wt.energy_per_shot, 4);
        put(wMetal, &wt.metal_per_shot, 4);
        r.write(at, wd.data(), wd.size());
    }

    // Units.
    std::vector<sim::Unit> units(kMaxUnits + 2);
    std::vector<sim::UnitTickState> tstate(kMaxUnits + 2);
    sim::CombatEnvironment cenv;
    cenv.empty_type = &empty_type;
    cenv.empty_tick_type = &empty_tick;
    World world;
    sim::Combat combat{units, tstate, players, cenv, world};
    sim::UnitTickEnvironment env;
    sim::OrderContext octx{tstate, players, world.rng, world};
    sim::MissionEnvironment menv;
    menv.gravity = static_cast<i32>(g() % 3 == 0 ? 1 + g() % 4000 : 50 + g() % 200);  // 0 reaches the C runtime's error path
    menv.addresses.game_state = L.gs;
    menv.addresses.unit_table = L.units;
    r.w32(L.gs + gGravity, static_cast<u32>(menv.gravity));
    r.w32(L.gs + gUnitsEnd, L.units + static_cast<u32>(units.size() - 1) * kUnitSize);
    sim::Missions missions{octx, units, combat, world, world, mission_types, allied, menv};
    world.missions = &missions;
    // Construction: the map cells (heights, height ranges, metal, features, occupants, marks)
    // and a few feature types, the same on both sides.
    sim::MapGrid grid;
    grid.width = hm.width();
    grid.height = hm.height();
    grid.cells.resize(static_cast<std::size_t>(grid.width) * grid.height);
    for (i32 z = 0; z < grid.height; ++z)
        for (i32 x = 0; x < grid.width; ++x) {
            sim::MapCell& c = grid.cells[static_cast<std::size_t>(z) * grid.width + x];
            c.height = hm.at(x, z);
            std::uint8_t lo = c.height, hi = c.height;
            for (int dz = 0; dz < 2; ++dz)
                for (int dx = 0; dx < 2; ++dx)
                    if (x + dx < grid.width && z + dz < grid.height) {
                        const std::uint8_t h = hm.at(x + dx, z + dz);
                        lo = (std::min)(lo, h);
                        hi = (std::max)(hi, h);
                    }
            c.height_min = lo;
            c.height_max = hi;
        }
    const int nfeatures = 6;
    std::vector<sim::FeatureTypeInfo> feature_types(static_cast<std::size_t>(nfeatures));
    {
        std::vector<std::uint8_t> fd(static_cast<std::size_t>(nfeatures) * 0x100, 0);
        for (int f = 0; f < nfeatures; ++f) {
            sim::FeatureTypeInfo& ft = feature_types[static_cast<std::size_t>(f)];
            ft.footprint_x = static_cast<std::int16_t>(1 + g() % 3);
            ft.footprint_z = static_cast<std::int16_t>(1 + g() % 3);
            ft.height = static_cast<std::uint8_t>(g() % 4 == 0 ? 0 : g() % 60);
            ft.flags = static_cast<std::uint16_t>((g() % 4 ? 0x80u : 0u) | (g() % 2 ? 0x40u : 0u) | (g() % 3 == 0 ? 1u : 0u) |
                                                  (g() % 5 == 0 ? 0x200u : 0u) | (g() % 5 == 0 ? 0x20u : 0u));
            ft.metal = static_cast<float>(g() % 3000) / 7.0f;
            ft.energy = g() % 3 == 0 ? 0.0f : static_cast<float>(g() % 9000) / 3.0f;
            std::uint8_t* d = &fd[static_cast<std::size_t>(f) * 0x100];
            std::memcpy(d + 0x94, &ft.footprint_x, 2);
            std::memcpy(d + 0x96, &ft.footprint_z, 2);
            std::memcpy(d + 0xEC, &ft.energy, 4);
            std::memcpy(d + 0xF0, &ft.metal, 4);
            std::memcpy(d + 0xFA, &ft.height, 1);
            std::memcpy(d + 0xFE, &ft.flags, 2);
        }
        r.write(L.features, fd.data(), fd.size());
        // Sometimes the last type lies past the table's count.
        r.w32(L.gs + 0x14253, static_cast<u32>(g() % 4 == 0 ? nfeatures - 1 : nfeatures));
        if (r.r32(L.gs + 0x14253) != static_cast<u32>(nfeatures)) feature_types.pop_back();
    }
    std::vector<int> feature_cells;
    auto put_features_around = [&](i32 wx, i32 wz) {
        const i32 cx = wx >> 4, cz = wz >> 4;
        const int n = static_cast<int>(g() % 30);
        for (int k = 0; k < n; ++k) {
            const i32 x = cx + static_cast<i32>(g() % 24) - 12, z = cz + static_cast<i32>(g() % 24) - 12;
            if (x < 0 || z < 0 || x >= grid.width || z >= grid.height) continue;
            sim::MapCell& c = grid.cells[static_cast<std::size_t>(z) * grid.width + x];
            const u32 roll = g() % 10;
            if (roll < 5) {
                c.feature = static_cast<std::uint16_t>(g() % nfeatures);
                feature_cells.push_back(z * grid.width + x);
            } else if (roll < 7) {  // a part of a bigger feature: back to a cell up to 2 rows / columns back
                c.feature = 0xFFFE;  // a part of a bigger feature
                c.feature_aux = static_cast<std::uint16_t>((g() % 3) | (g() % 3) << 8);
                feature_cells.push_back(z * grid.width + x);
            } else if (roll < 8) {
                c.feature = static_cast<std::uint16_t>(0xFFFB + g() % 3);
            }
            if (g() % 4 == 0) c.unit = static_cast<std::uint16_t>(g() % 3 == 0 ? 1 + g() % kMaxUnits : 0);
            if (g() % 6 == 0) c.flags = static_cast<std::uint8_t>(c.flags | (g() % 2 ? 2 : 1));
            c.metal = static_cast<std::uint8_t>(g());
        }
    };
    sim::ConstructionEnvironment conv;
    sim::Construction con{grid, feature_types, con_types, hm, world, conv};
    missions.construction = &con;
    world.defs_base = L.defs;
    world.gs_base = L.gs;
    world.bucket_base = L.attached + 0x30000;
    world.links.heads.assign(units.size(), nullptr);
    world.crt.set_state(static_cast<u32>(g()));
    r.w32(L.rand_state, world.crt.state());
    world.units_base = L.units;
    world.units = &units;
    world.ticks = &tstate;
    world.players = &players;
    world.combat = &combat;
    world.orders = &octx;
    world.env = &env;
    world.settle_types = &settle_types;
    world.hm = &hm;
    world.motions.resize(units.size());
    const u32 rng0 = static_cast<u32>(g()) | 1u;
    r.w32(kBattleRngState, rng0);
    world.rng.set_state(rng0);
    world.clock = static_cast<u32>(g());
    r.w32(L.clock, world.clock);
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
    if (present.empty()) return true;
    // The last two units stand in for units being created (the creation stub hands them out).
    std::vector<int> newborn;
    if (present.size() > 4) newborn.assign(present.end() - 2, present.end());
    for (int p = 0; p < 3; ++p)
        r.write(L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize + pAlive, &players[static_cast<std::size_t>(p)].alive_count, 2);
    u32 attached_next = L.attached + 0x10;
    auto new_attached = [&]() {
        const u32 a = attached_next;
        attached_next += 8;
        r.w32(a, L.vtables + 0x40);
        return a;
    };
    const i32 map_w = hm.width() * 16, map_h = hm.height() * 16;
    for (int i : present) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
        const int k = static_cast<int>(g() % ntypes);
        LoadedType& lt = types[static_cast<std::size_t>(pick[static_cast<std::size_t>(k)])];
        t.type_index = static_cast<std::uint16_t>(k + 1);
        t.tick_type = &lt.tick_info;
        u.type = &lt.info;
        auto coord = [&](i32 size) -> i32 {
            const int m = static_cast<int>(g() % 20);
            if (m == 0) return static_cast<i32>(g());                                    // anywhere
            if (m == 1) return static_cast<i32>(static_cast<u32>(g() % 200) << 16) - (100 << 16);  // around the edge
            if (m == 2) return static_cast<i32>(static_cast<u32>(size - 100 + static_cast<i32>(g() % 200)) << 16);
            return static_cast<i32>((static_cast<u32>(g() % static_cast<u32>(size - 200)) + 100) << 16 | (g() & 0xFFFF));
        };
        u.pos = {coord(map_w), static_cast<i32>(g() % 3 == 0 ? (g() % 255) << 16 : g()), coord(map_h)};
        u.heading = static_cast<std::uint16_t>(g() % 4 == 0 ? 0 : g());
        u.pitch = static_cast<std::int16_t>(g());
        u.bank = static_cast<std::int16_t>(g());
        u.hover_phase = static_cast<std::uint16_t>(g());
        u.hit_points = static_cast<std::int16_t>(g() % (lt.info.max_damage + 50));
        u.flags = sim::unit_flags::kAlive | (g() % 2 ? 0x30u : 0u) | (g() % 16 == 0 ? 0x4000u : 0u) | (g() % 2 ? 1u : g() % 4) |
                  (g() % 2 ? 0x10000u : 0u);
        // Behaviour inputs: building/static (0x20000000), standing orders, cloak wish, activation.
        u.flags |= (g() % 3 == 0 ? 0x20000000u : 0u) | (static_cast<u32>(g() % 16) << 18) | (g() % 4 == 0 ? 0x800u : 0u) |
                   (g() % 8 == 0 ? 0x8000u : 0u);
        u.activate_flags = static_cast<std::uint8_t>((g() % 2 ? 1u : 0u) | (g() % 4 == 0 ? g() & 0x1Eu : 0u));
        u.net_dirty = static_cast<std::uint16_t>(g() % 3 == 0 ? g() : 0);
        u.build_fraction_left = g() % 6 == 0 ? 0.5f : 0.0f;
        t.has_motion = g() % 5 != 0;
        t.last_attacker_slot = 10;
        t.prev_health_percent = static_cast<std::uint8_t>(g() % 101);
        t.health_percent = t.prev_health_percent;
        const u32 at = unit_addr(i);
        const u32 owner_at = L.gs + 0x1B63 + static_cast<u32>(t.owner) * kPlayerSize;
        const std::uint8_t owner_slot = static_cast<std::uint8_t>(t.owner);
        MotionInfo& m = world.motions[static_cast<std::size_t>(i)];
        if (t.has_motion) {
            m.block = L.motion + static_cast<u32>(i) * 0x40;
            m.speed = g() % 4 == 0 ? static_cast<i32>(g()) : static_cast<i32>(g() % 0x60000);
            m.start = env.game_tick - g() % 100;
            r.w32(m.block + mGoal, m.block + mGoalObject);
            r.w32(m.block + mGoalObject, L.vtables);
            r.w32(m.block + mGoalObject + 4, 0);
            r.w32(m.block + mSpeed, static_cast<u32>(m.speed));
            r.w32(m.block + mStart, m.start);
        }
        r.w32(at + uMotion, t.has_motion ? m.block : 0u);
        r.write(at + uPitch, &u.pitch, 2);
        r.write(at + uHeading, &u.heading, 2);
        r.write(at + uBank, &u.bank, 2);
        r.write(at + uPosX, &u.pos[0], 4);
        r.write(at + uPosY, &u.pos[1], 4);
        r.write(at + uPosZ, &u.pos[2], 4);
        r.w32(at + uDef, lt.remote_def);
        r.w32(at + uOwner, owner_at);
        r.write(at + uOwnerSlot, &owner_slot, 1);
        r.w32(at + uResourcePlayer, owner_at);
        r.write(at + uDefIndex, &t.type_index, 2);
        r.write(at + uId, &u.id, 2);
        r.write(at + uHoverPhase, &u.hover_phase, 2);
        r.write(at + uNetDirty, &u.net_dirty, 2);
        r.write(at + uLastAttackerSlot, &t.last_attacker_slot, 1);
        r.write(at + uHealth, &t.health_percent, 1);
        r.write(at + uPrevHealth, &t.prev_health_percent, 1);
        r.write(at + uBuildLeft, &u.build_fraction_left, 4);
        r.write(at + uHp, &u.hit_points, 2);
        r.w32(at + uFlags, u.flags);
        r.write(at + uActivate, &u.activate_flags, 1);
        u.group = g() % 2 ? 0u : static_cast<u32>(g() % 10);
        r.w32(at + uGroup, u.group);
        // Construction inputs: footprint and cells, kills, the resource slot's incomes, build stance.
        u.footprint_x = static_cast<std::int16_t>(1 + g() % 4);
        u.footprint_z = static_cast<std::int16_t>(1 + g() % 4);
        u.cell_x = static_cast<std::int16_t>((u.pos[0] >> 20) - u.footprint_x / 2);
        u.cell_z = static_cast<std::int16_t>((u.pos[2] >> 20) - u.footprint_z / 2);
        t.kill_count = static_cast<std::uint16_t>(g() % 4 == 0 ? g() % 40 : 0);
        t.metal_make = static_cast<float>(g() % 1000) / 9.0f;
        t.energy_make = static_cast<float>(g() % 1000) / 7.0f;
        u.script_flags = static_cast<std::uint8_t>(g() % 2);
        r.write(at + uFootprint, &u.footprint_x, 2);
        r.write(at + uFootprint + 2, &u.footprint_z, 2);
        r.write(at + uCell, &u.cell_x, 2);
        r.write(at + uCell + 2, &u.cell_z, 2);
        r.write(at + uKills, &t.kill_count, 2);
        r.write(at + uMetalMake, &t.metal_make, 4);
        r.write(at + uEnergyMake, &t.energy_make, 4);
        r.write(at + uScriptFlags, &u.script_flags, 1);
        put_features_around(u.pos[0] >> 16, u.pos[2] >> 16);
        // Each unit alone in a spatial bucket list of its own; motion layer bits on the ground.
        r.w32(at + 0x82, world.bucket_addr(i));
        r.w32(world.bucket_addr(i) + 6, at);
        r.w32(at + 0x8E, 0);
        u.bucket = i;
        world.links.heads[static_cast<std::size_t>(i)] = &u;
        if (t.has_motion) {
            const std::uint8_t one = 1;
            r.write(m.block + 0x2E, &one, 1);
        }
        // Weapon slot targets and flags (the weapon-target routines run for real).
        for (int s = 0; s < 3; ++s) {
            sim::WeaponSlot& ws = u.weapons[static_cast<std::size_t>(s)];
            const u32 kind = g() % 4;
            ws.target_x = kind == 0 ? 0 : static_cast<std::int16_t>(kind == 1 ? g() % 3000 : 1 + g() % kMaxUnits);
            ws.target_z = kind == 1 ? static_cast<std::int16_t>(g() % 3000) : sim::kUnitTarget;
            ws.flags = static_cast<std::uint8_t>(g() & 0x1F);
            const u32 wi = g() % 4;
            ws.type = &wtypes[wi];
            ws.stockpile = static_cast<std::uint8_t>(g() % 4 == 0 ? 195 + g() % 61 : g() % 10);
            const u32 sa = at + 4 + static_cast<u32>(s) * 0x1C;
            r.write(sa + 0x00, &ws.target_x, 2);
            r.write(sa + 0x02, &ws.target_z, 2);
            r.w32(sa + 0x0C, L.weapons + wi * 0x200);
            r.write(sa + 0x1A, &ws.stockpile, 1);
            r.write(sa + 0x1B, &ws.flags, 1);
        }
        u32 state = 0;
        if (!r.call(kBuildState, 0, {lt.remote_model, lt.remote_cob, at}, &state) || !state) {
            std::printf("build state failed\n");
            return false;
        }
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
    {
        std::vector<std::uint8_t> cb(grid.cells.size() * 13, 0);
        for (std::size_t k = 0; k < grid.cells.size(); ++k) {
            const sim::MapCell& c = grid.cells[k];
            std::uint8_t* d = &cb[k * 13];
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
        r.write(L.cells, cb.data(), cb.size());
    }
    ++st.scenarios;
    env.addresses.game_state = L.gs;
    env.addresses.unit_table = L.units;
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
        } else if (w >= L.motion && w < L.motion + 0x40u * (kMaxUnits + 2)) {
            const u32 off = (w - L.motion) % 0x40, idx = (w - L.motion) / 0x40;
            if (off == 0) w = 0x7E000000u | idx;
            else if (off == mGoalObject) w = 0x7D000000u | idx;
        }
    };
    // Log records: zero undefined ECX values and stack addresses; drop frees of
    // blocks that are not orders (script objects and the like).
    auto normalise = [&](const std::vector<u32>& in, bool remote) {
        std::vector<u32> out;
        for (std::size_t i = 0; i < in.size();) {
            const std::size_t n = record_size(in[i]);
            if (!n || i + n > in.size()) {
                out.push_back(0xDEADBEEFu);
                break;
            }
            std::vector<u32> rec(in.begin() + static_cast<std::ptrdiff_t>(i), in.begin() + static_cast<std::ptrdiff_t>(i + n));
            i += n;
            const u32 id = rec[0];
            if (remote && id != 6 && id != kHandlerId && (id < 10 || id > 13) && id != 61 && id != kGoalDetachId && id != kAttachedDtorId &&
                id != 84 && id != 88 && id != 102 && id != 103)
                rec[1] = 0;
            if (id == static_cast<u32>(kEnemiesId)) rec[3] = rec[6] = 0;  // stack addresses
            if (id == static_cast<u32>(kResolveId)) rec[2] = rec[6] = 0;  // out and position pointers
            if (id == 100) {  // the player and type arguments carry stale upper bits
                rec[2] &= 0xFF;
                rec[3] &= 0xFFFF;
            }
            if (id == 53 && !world.all_orders.count(rec[2])) continue;
            if (id == 16) {
                rec[3] = 0;
                const u32 size = rec[4];
                if (size == 4) rec[2] = rec[5] = rec[6] = rec[7] = 0;
                std::uint8_t bytes[12];
                std::memcpy(bytes, &rec[5], 12);
                for (u32 b = size < 12 ? size : 12; b < 12; ++b) bytes[b] = 0;
                std::memcpy(&rec[5], bytes, 12);
            }
            if (id == static_cast<u32>(kHandlerId)) ++st.handler_calls;
            if (id == 53 && remote) ++st.orders_freed;
            out.insert(out.end(), rec.begin(), rec.end());
        }
        return out;
    };
    auto link_value = [&](const sim::UnitLink* l) -> u32 { return l ? l->owner->address + oLink : 0u; };
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
        for (auto& w : world.log) norm_ptr(w);  // undefined reads can pass unit addresses
        const std::vector<u32> theirs = normalise(log, true);
        const std::vector<u32> mine = normalise(world.log, false);
        world.log.clear();
        if (theirs != mine) {
            bad("subsystem call log differs");
            std::size_t first = 0;
            while (first < theirs.size() && first < mine.size() && theirs[first] == mine[first]) ++first;
            const std::size_t from = first > 12 ? first - 12 : 0;
            for (std::size_t i = from; i < first + 12 && i < (std::max)(theirs.size(), mine.size()); ++i) {
                const long long a = i < theirs.size() ? static_cast<long long>(theirs[i]) : -1;
                const long long b = i < mine.size() ? static_cast<long long>(mine[i]) : -1;
                std::printf("    log[%zu] orig %llx ours %llx%s\n", i, a, b, a != b ? "   <--" : "");
            }
        }
        if (static_cast<i32>(r.r32(L.gs + 0x14353)) != env_live) bad("live unit count");
        if (r.r32(kBattleRngState) != world.rng.state()) bad("battle RNG state");
        if (r.r32(L.clock) != world.clock) bad("clock reads");
        if (r.r32(L.rand_state) != world.crt.state()) bad("rand() state");
        for (int p = 0; p < 10; ++p) {
            const sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
            std::vector<std::uint8_t> rp(kPlayerSize);
            r.read(L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize, rp.data(), rp.size());
            auto f16 = [&](u32 off) { std::uint16_t v; std::memcpy(&v, &rp[off], 2); return v; };
            if (f16(pUnitsKilled) != s.units_killed || f16(pUnitsLost) != s.units_lost || f16(pAlive) != s.alive_count || f16(pFlags) != s.flags)
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
            if (f32(uFlags) != u.flags) bad(un + "flags orig " + std::to_string(f32(uFlags)) + " ours " + std::to_string(u.flags));
            if (f16(uDefIndex) != t.type_index) bad(un + "type index");
            if (static_cast<std::int16_t>(f16(uHp)) != u.hit_points) bad(un + "hit points");
            if (f8(uLastDamageKind) != t.last_damage_kind) bad(un + "last damage kind");
            if (f8(uDamagedTimer) != t.damaged_timer || f32(uHoldoff) != t.transfer_holdoff) bad(un + "timers");
            if (f8(uActivate) != u.activate_flags || f8(uScriptFlags) != u.script_flags) bad(un + "activate/script flags");
            if (f16(uNetDirty) != u.net_dirty) bad(un + "pending events");
            if (f32(uCloakHoldoff) != u.cloak_holdoff_tick) bad(un + "decloak hold-off");
            if (f32(uBuildLeft) != fbits(u.build_fraction_left)) bad(un + "build fraction");
            if (f32(uMetalMake) != fbits(t.metal_make) || f32(uEnergyMake) != fbits(t.energy_make)) bad(un + "resource incomes");
            if (f32(uGroup) != u.group) bad(un + "group");
            if (static_cast<i32>(f32(uPosX)) != u.pos[0] || static_cast<i32>(f32(uPosY)) != u.pos[1] || static_cast<i32>(f32(uPosZ)) != u.pos[2])
                bad(un + "position");
            if (static_cast<std::int16_t>(f16(uPitch)) != u.pitch || f16(uHeading) != u.heading || static_cast<std::int16_t>(f16(uBank)) != u.bank)
                bad(un + "pitch/heading/bank");
            if ((f32(uScript) != 0) != (u.script != nullptr) || (f32(uMotion) != 0) != t.has_motion) bad(un + "script/motion presence");
            for (int s = 0; s < 3; ++s) {
                const sim::WeaponSlot& ws = u.weapons[static_cast<std::size_t>(s)];
                const u32 so = 4 + static_cast<u32>(s) * 0x1C;
                if (static_cast<std::int16_t>(f16(so)) != ws.target_x || static_cast<std::int16_t>(f16(so + 2)) != ws.target_z ||
                    f8(so + 0x1B) != ws.flags || f8(so + 0x1A) != ws.stockpile)
                    bad(un + "weapon slot " + std::to_string(s));
            }
            // Orders, both lists.
            for (int list = 0; list < 2; ++list) {
                u32 ra = f32(list ? uSecondary : uOrders);
                const sim::MissionOrder* o = list ? u.secondary_orders : u.orders;
                int n = 0;
                for (; (ra || o) && n < 1000; ++n) {
                    const std::string on = un + (list ? "secondary" : "primary") + " order " + std::to_string(n) + " ";
                    if (!ra || !o || ra != o->address) {
                        bad(on + "presence/address");
                        break;
                    }
                    std::vector<std::uint8_t> ro(0x56);
                    r.read(ra, ro.data(), ro.size());
                    auto o8 = [&](u32 off) { return ro[off]; };
                    auto o32 = [&](u32 off) { u32 v; std::memcpy(&v, &ro[off], 4); return v; };
                    u32 ounit = o32(oUnit), tgt = o32(oLink + 4);
                    norm_ptr(ounit);
                    norm_ptr(tgt);
                    if (o8(oKind) != o->kind || o8(oStep) != o->step) bad(on + "kind/step");
                    if (o32(oWait) != o->wait || o32(oWake) != o->wake_tick || o32(oEvents) != o->events) bad(on + "wait/wake/events");
                    if (ounit != (o->unit ? World::tok(*o->unit) : 0u)) bad(on + "unit");
                    if (tgt != (o->target.unit ? World::tok(*o->target.unit) : 0u) || o32(oLink + 8) != link_value(o->target.next) ||
                        o32(oLink + 0xC) != (o->target.owner ? o->target.owner->address : 0u))
                        bad(on + "target link");
                    for (int k = 0; k < 3; ++k)
                        if (static_cast<i32>(o32(oPos + 4u * static_cast<u32>(k))) != o->pos[static_cast<std::size_t>(k)]) bad(on + "position");
                    if (o32(oScratch) != o->scratch0 || o32(oScratch + 4) != o->scratch1) bad(on + "scratch");
                    if (static_cast<i32>(o32(oParams)) != o->param0 || static_cast<i32>(o32(oParams + 4)) != o->param1 ||
                        static_cast<i32>(o32(oParams + 8)) != o->param2)
                        bad(on + "params");
                    if (o32(oFlags) != o->flags) bad(on + "flags");
                    if (o32(oCreated) != o->created_tick) bad(on + "created");
                    if (o32(oAttached) != o->attached) bad(on + "attached");
                    ra = o32(oNext);
                    o = o->next;
                }
            }
            // Links pointing at the unit.
            {
                u32 ra = f32(uReferrers);
                const sim::UnitLink* l = u.referrers;
                for (int n = 0; (ra || l) && n < 1000; ++n) {
                    if (!ra || !l || ra != link_value(l)) {
                        bad(un + "referrer " + std::to_string(n));
                        break;
                    }
                    ra = r.r32(ra + 8);
                    l = l->next;
                }
            }
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
            if (!u.script->fault().empty()) bad(un + "script fault: " + u.script->fault());
        }
        if (!ok) ++st.mismatches;
        return ok;
    };
    auto engine_faulted = [&]() {
        if (!missions.fault.empty()) {
            ++st.faults;
            std::printf("scenario %u: stopped, the original would fail here: %s\n", seed, missions.fault.c_str());
            return true;
        }
        for (const auto& u : units)
            if (u.script && !u.script->fault().empty()) {
                ++st.faults;
                std::printf("scenario %u: stopped, engine script fault: %s\n", seed, u.script->fault().c_str());
                return true;
            }
        return false;
    };
    auto alive = [&](int i) {
        return (units[static_cast<std::size_t>(i)].flags & sim::unit_flags::kAlive) && tstate[static_cast<std::size_t>(i)].type_index != 0;
    };
    auto any_unit = [&]() { return present[g() % present.size()]; };
    auto random_stale = [&]() -> u32 { return g() % 3 == 0 ? g() % 16 : static_cast<u32>(g()); };
    auto check_stale = [&](u32 mine, const std::string& what) {
        const u32 theirs = r.r32(L.ret_val + kStaleOut);
        if (theirs == mine) return true;
        std::printf("scenario %u %s: stale stack word orig %08x ours %08x\n", seed, what.c_str(), theirs, mine);
        ++st.mismatches;
        return false;
    };
    auto fail = [&](const std::string& what) {
        std::printf("scenario %u: the original failed during %s\n", seed, what.c_str());
        if (std::getenv("OH_TRACE")) {  // the original's last stub calls before it failed
            const u32 end = r.r32(L.log_ptr);
            std::vector<u32> lg((end - L.log_base) / 4);
            if (!lg.empty()) r.read(L.log_base, lg.data(), lg.size() * 4);
            for (std::size_t k = lg.size() > 60 ? lg.size() - 60 : 0; k < lg.size(); ++k) std::printf("    orig log %zu: %x\n", k, lg[k]);
        }
        return false;
    };
    // A fresh script of behaviour steps for both sides.
    auto deal_steps = [&]() {
        world.steps.clear();
        world.step_at = 0;
        const int n = static_cast<int>(g() % 24);
        for (int k = 0; k < n; ++k) {
            Step s;
            const u32 roll = g() % 100;
            s.result = roll < 10 ? 0 : roll < 20 ? 1 : roll < 33 ? 2 : roll < 43 ? 3 : roll < 47 ? 4 : roll < 62 ? 5 : roll < 70 ? 6
                     : roll < 73 ? 7 : roll < 78 ? 8 : roll < 96 ? 9 : roll < 98 ? 10 : 0xFFFFFFFFu;
            const u32 waits[] = {1, 2, 3, 0x10000, 0x10001, 0x10, 4, 0x10002};
            s.wait_or = g() % 2 ? 0 : waits[g() % 8];
            s.wake = env.game_tick + g() % 60;
            s.slot_mode = g() % 8 == 0;
            s.slot_value = static_cast<u32>(g());
            world.steps.push_back(s);
        }
        world.fallback.wake = env.game_tick + 1 + g() % 30;
        std::vector<u32> blk = {0, 0, 2, 1, world.fallback.wake, 0, 0};
        for (const Step& s : world.steps) blk.insert(blk.end(), {s.result, s.wait_or, s.wake, s.slot_mode, s.slot_value});
        blk[0] = L.script + 7 * 4;
        blk[1] = L.script + static_cast<u32>(blk.size()) * 4;
        r.write(L.script, blk.data(), blk.size() * 4);
        // Answers of the systems the real behaviours call (same lists for both sides).
        std::vector<u32> targets;
        for (int i : present)
            if (alive(i) && !(units[static_cast<std::size_t>(i)].flags & 0x4000u)) targets.push_back(unit_addr(i));
        auto some_unit = [&]() { return targets.empty() ? 0u : targets[g() % targets.size()]; };
        for (int q = 0; q < kQueues; ++q) {
            std::vector<u32>& v = world.queue[static_cast<std::size_t>(q)];
            v.clear();
            world.queue_at[static_cast<std::size_t>(q)] = 0;
            world.behaviour_calls = 0;
            const int answers = static_cast<int>(g() % 6);
            for (int k = 0; k < answers; ++k) {
                switch (q) {
                case qFireAtWill: v.push_back(g() % 3 == 0 ? 0u : some_unit()); break;
                case qIssueAttack: v.push_back(g() % 2); break;
                case qResolve: {  // never AttackSpecial itself (it would resolve again at once, forever)
                    u32 kd = g() % 3 == 0 ? 0u : g() % sim::mission::kCount;
                    if (kd == sim::mission::kAttackSpecial) kd = sim::mission::kAttackNoMove;
                    // Nor AttackUType: the order it makes has type 0 to look for, which matches the empty
                    // table entries, and the original crashes on their missing owner.
                    if (kd == sim::mission::kAttackUnitType) kd = sim::mission::kAttackNoMove;
                    // An attack never resolves to a background kind; one would leave an order
                    // in a list its flags do not name, and clearing it would never end.
                    if (sim::mission::default_flags()[kd] & sim::order_flags::kSecondary) kd = sim::mission::kAttackChase;
                    v.push_back(kd);
                    break;
                }
                case qResources: v.push_back(g() % 4 ? 1u : 0u); break;
                case qCreate: {  // a live stand-in (a real creation always gives a live unit with a script)
                    const int nb = newborn.empty() ? 0 : newborn[g() % newborn.size()];
                    v.push_back(g() % 4 == 0 || !nb || !alive(nb) || !units[static_cast<std::size_t>(nb)].script ? 0u : unit_addr(nb));
                    break;
                }
                case qPlace: v.push_back(g() % 3 ? 1u : 0u); break;
                default: {
                    const u32 count = targets.empty() ? 0u : g() % 4;
                    v.push_back(count);
                    for (u32 c = 0; c < count; ++c) v.push_back(some_unit());
                }
                }
            }
            const u32 qa = L.queues + static_cast<u32>(q) * kQueueSize;
            std::vector<u32> img = {qa + 8, qa + 8 + static_cast<u32>(v.size()) * 4};
            img.insert(img.end(), v.begin(), v.end());
            r.write(qa, img.data(), img.size() * 4);
        }
        world.next_address = r.r32(L.bump_ptr);
        octx.game_tick = env.game_tick;
    };
    auto all_orders_of = [&](int i) {
        std::vector<sim::MissionOrder*> v;
        for (sim::MissionOrder* o = units[static_cast<std::size_t>(i)].orders; o; o = o->next) v.push_back(o);
        for (sim::MissionOrder* o = units[static_cast<std::size_t>(i)].secondary_orders; o; o = o->next) v.push_back(o);
        return v;
    };
    // Random changes to an order's state on both sides.
    auto tweak_order = [&](int i, sim::MissionOrder& o) {
        const u32 waits[] = {0, 0, 1, 2, 3, 0x10000, 0x10001, 0x10, 0x14};
        o.wait = waits[g() % 9];
        o.wake_tick = g() % 4 == 0 ? 0xFFFFFFFFu : env.game_tick + g() % 40 - 10;
        o.events = g() % 2 ? 0 : waits[g() % 9];
        o.step = static_cast<std::uint8_t>(g() % 4);
        if (o.kind == sim::mission::kGuardNoMove && o.step == 2) o.step = 1;  // step 2 needs the target found in step 1
        if (o.kind == sim::mission::kMobileBuild && o.step >= 2 && !o.target.unit) o.step = 1;  // steps 2-3 need the unit being built
        const u32 bits[] = {0x4, 0x4000, 0x10000, 0x400000, 0x800000, 0x200, 0x400};
        if (g() % 2) o.flags ^= bits[g() % 7];
        // (Not for real behaviours: one that always finishes would then run forever, as in the original.)
        if (g() % 40 == 0 && &o == units[static_cast<std::size_t>(i)].orders && !sim::mission_implemented(o.kind))
            o.flags |= sim::order_flags::kSecondary;
        if (g() % 3 == 0) o.attached = g() % 3 ? new_attached() : 0;
        r.write(o.address + oStep, &o.step, 1);
        r.w32(o.address + oWait, o.wait);
        r.w32(o.address + oWake, o.wake_tick);
        r.w32(o.address + oEvents, o.events);
        r.w32(o.address + oFlags, o.flags);
        r.w32(o.address + oAttached, o.attached);
        MotionInfo& m = world.motions[static_cast<std::size_t>(i)];
        if (m.block && g() % 2) {
            m.goal_current = g() % 3 == 0 ? 0 : g() % 3 ? o.attached : new_attached();
            r.w32(m.block + mGoalObject + 4, m.goal_current);
        }
    };
    // Builds a new order with the original constructor and insertion routines (and ours).
    auto create_order = [&](int i) -> bool {
        if (std::find(newborn.begin(), newborn.end(), i) != newborn.end()) return true;  // units the creation stub hands out get no orders
        auto kind = static_cast<std::uint8_t>(g() % 4 == 0 ? sim::mission::kParalyze : g() % sim::mission::kCount);
        // Half of the others are behaviours that run for real.
        if (g() % 2)
            while (!sim::mission_implemented(kind)) kind = static_cast<std::uint8_t>(g() % sim::mission::kCount);
        // Queued moves and patrols, which a finished unit copies from its builder (GetBuilt).
        if (g() % 10 == 0) kind = g() % 2 ? sim::mission::kQueuedMove : sim::mission::kQueuedPatrol;
        if (g() % 16 == 0) kind = sim::mission::kGetBuilt;
        // Construction behaviours a third of the time, mostly on live targets.
        static const std::uint8_t kCon[] = {sim::mission::kBuildingBuild, sim::mission::kMobileBuild, sim::mission::kHelpBuild,
                                            sim::mission::kRepairUnit,    sim::mission::kReclaim,     sim::mission::kReclaimUnit,
                                            sim::mission::kCapture,       sim::mission::kBeCarried,   sim::mission::kPark};
        if (g() % 3 == 0) kind = kCon[g() % 9];
        int ti = g() % 3 == 0 ? 0 : static_cast<int>(1 + g() % kMaxUnits);
        if (sim::construction_kind(kind) && g() % 4) ti = present[g() % present.size()];
        if (kind == sim::mission::kGetBuilt && ti == i) ti = 0;  // its builder's orders are walked while it gets new ones
        sim::Unit* target = ti ? &units[static_cast<std::size_t>(ti)] : nullptr;
        std::array<i32, 3> pos = {static_cast<i32>(g()), static_cast<i32>(g()), static_cast<i32>(g())};
        const bool has_pos = g() % 2;
        i32 p0 = static_cast<i32>(g() % 3 == 0 ? g() : g() % 100), p1 = static_cast<i32>(g()), p2 = static_cast<i32>(g());
        // Parameters the real behaviours can take without the original failing.
        switch (kind) {
        case sim::mission::kBuildWeapon:  // slot, rounds to build, progress
            p0 = static_cast<i32>(g() % 3);
            p1 = static_cast<i32>(g() % 8) - 2;
            p2 = static_cast<i32>(g() % 400);
            break;
        case sim::mission::kSelfDestruct:
        case sim::mission::kSelfDestructFg: {  // the countdown indexes a table on the stack
            p0 = g() % 4 == 0 ? 1 : 0;
            const u32 m = g() % 3;
            p1 = static_cast<i32>(m == 0 ? 0u : m == 1 ? 0xF0000000u | g() % 10 : static_cast<u32>(g()) & 0x0FFFFFFFu);
            break;
        }
        case sim::mission::kAttackUnitType: p0 = static_cast<i32>(1 + g() % 4); break;  // unit type index
        case sim::mission::kBuildingBuild:  // unit type, count
            p0 = static_cast<i32>(1 + g() % ntypes);
            p1 = static_cast<i32>(g() % 5) - 1;
            break;
        case sim::mission::kMobileBuild: {  // unit type, at a spot near the builder
            p0 = static_cast<i32>(1 + g() % ntypes);
            p2 = static_cast<i32>(g() % 13);
            const sim::Unit& b = units[static_cast<std::size_t>(i)];
            pos = {b.pos[0] + static_cast<i32>((g() % 400) << 16) - (200 << 16), b.pos[1], b.pos[2] + static_cast<i32>((g() % 400) << 16) - (200 << 16)};
            break;
        }
        case sim::mission::kRepairUnit: p2 = g() % 3 == 0 ? static_cast<i32>(g() % 600) : 0; break;  // leash
        case sim::mission::kReclaim:  // at a feature cell
            if (!feature_cells.empty() && g() % 4) {
                const int c = feature_cells[g() % feature_cells.size()];
                pos = {static_cast<i32>(((c % grid.width) << 20) | (g() & 0xFFFFF)), static_cast<i32>(g() % 255) << 16,
                       static_cast<i32>(((c / grid.width) << 20) | (g() & 0xFFFFF))};
            }
            break;
        case sim::mission::kWait:  // steps, radius
            p0 = static_cast<i32>(g() % 400) - 50;
            p1 = g() % 2 ? 0 : static_cast<i32>(g() % 2000);
            break;
        case sim::mission::kStandingFireOrder:
        case sim::mission::kStandingMoveOrder: p0 = static_cast<i32>(g() % 3 == 0 ? g() : g() % 4); break;
        default: break;
        }
        r.write(L.names + 0x100, pos.data(), 12);
        sim::MissionOrder* o = world.allocate_order();
        sim::construct_order(octx, *o, kind, target, has_pos ? pos.data() : nullptr, p0, p1, p2);
        u32 mem = 0;
        if (!r.call(kAlloc, 0, {0x56}, &mem) || mem != o->address) return fail("allocation");
        if (!r.call(kConstruct, mem, {kind, ti ? unit_addr(ti) : 0u, has_pos ? L.names + 0x100 : 0u, static_cast<u32>(p0),
                                      static_cast<u32>(p1), static_cast<u32>(p2)}))
            return fail("constructor");
        // A background order (as some behaviours make them). Not AttackSpecial: it takes the
        // flags of the kind it becomes, and the order would then sit in a list its flags do
        // not name (the original's clearing never ends then).
        // GetBuilt only for a finished unit (it then ends at once): an unfinished one waits on
        // an event, which only works in the primary list (in the secondary list it would run
        // again at once, forever).
        const bool unfinished = units[static_cast<std::size_t>(i)].build_fraction_left != 0.0f;
        if (g() % 3 == 0 && kind != sim::mission::kAttackSpecial && !(kind == sim::mission::kGetBuilt && unfinished) &&
            !sim::construction_kind(kind)) {  // (construction steps that restart at once would run forever there)
            o->flags |= sim::order_flags::kSecondary;
            r.w32(mem + oFlags, o->flags);
        }
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::MissionOrder* head = (o->flags & sim::order_flags::kSecondary) ? u.secondary_orders : u.orders;
        if (g() % 2 || !head) {
            sim::prepend_order(u, *o);
            if (!r.call(kPrepend, 0, {unit_addr(i), mem})) return fail("prepend");
        } else {
            std::vector<sim::MissionOrder*> list;
            for (sim::MissionOrder* p = head; p; p = p->next) list.push_back(p);
            list.push_back(nullptr);
            sim::MissionOrder* before = list[g() % list.size()];
            sim::insert_order(u, *o, before);
            if (!r.call(kInsert, 0, {unit_addr(i), mem, before ? before->address : 0u})) return fail("insert");
        }
        ++st.orders_made;
        if (g() % 2) tweak_order(i, *o);
        return true;
    };
    // Calls a one-argument routine through the stale-word caller.
    auto call_one = [&](u32 fn, u32 arg, u32 edi, u32 stale) {
        r.w32(L.ret_val + kOneFn, fn);
        r.w32(L.ret_val + kOneArg, arg);
        r.w32(L.ret_val + kOneEdi, edi);
        r.w32(L.ret_val + kStaleIn, stale);
        return r.call(L.caller_one, 0, {});
    };
    auto set_env = [&](int tick) {
        env.game_tick = g() % 4 == 0 ? static_cast<u32>(g() % 0x70000000u) : static_cast<u32>(1000 + tick);
        env.wind_changed = false;
        env.water_damage_enabled = g() % 4 == 0;
        env.water_damage = static_cast<i32>(g() % 3) * 50;
        env.sea_level = static_cast<std::uint8_t>(g() % 3 == 0 ? g() : g() % 40);
        env.broadcast_unit_states = g() % 2;
        cenv.local_player = static_cast<std::uint8_t>(g() % 4);
        cenv.local_view = static_cast<std::uint8_t>(g() % 4);
        cenv.game_mode = 0;
        world.kill_flash = false;
        world.pay_result = g() % 4 ? 1u : 0u;
        octx.game_tick = env.game_tick;
        menv.sea_level = env.sea_level;
        r.w32(L.gs + 0x38A47, env.game_tick);
        r.w32(L.gs + 0x37EE2, 0);
        r.w32(L.schema + 0xD4C, env.water_damage_enabled ? 1u : 0u);
        r.w32(L.schema + 0xD50, static_cast<u32>(env.water_damage));
        r.write(L.gs + 0x1427F, &env.sea_level, 1);
        r.w32(L.gs + 0x2A44, env.broadcast_unit_states ? 1u : 0u);
        r.w32(L.gs + 0x14373, 0);
        r.write(L.gs + gLocalPlayer, &cenv.local_player, 1);
        r.write(L.gs + gLocalView, &cenv.local_view, 1);
        r.w32(L.gs + gGameMode, 0);
        cenv.ai_difficulty = static_cast<i32>(g() % 3);
        conv.ai_difficulty = cenv.ai_difficulty;
        conv.sea_level = env.sea_level;
        r.w32(L.gs + gDifficulty, static_cast<u32>(cenv.ai_difficulty));
        r.w32(L.gs + 0x37E9C, 0);  // no build menu shown
        const std::uint8_t zero8 = 0;
        r.write(L.gs + gMapFlags, &zero8, 1);
        r.write(L.gs + gFlash, &zero8, 1);
        r.w32(L.ret_val, world.pay_result);
    };

    // Construction routines called directly on both sides (sim/construction.h, sim/nano_spray.h).
    // Returns false when the original failed; `what` names the operation ("" = skipped).
    auto construction_op = [&](int i, std::string& what) -> bool {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        const int j = any_unit();
        sim::Unit& v = units[static_cast<std::size_t>(j)];
        auto set_fraction = [&](sim::Unit& x) {
            const float fr[] = {0.0f, 1.0f, 0.5f, 0.001f, 0.999f, static_cast<float>(g() % 1000) / 1000.0f};
            x.build_fraction_left = fr[g() % 6];
            r.write(unit_addr(x.id) + uBuildLeft, &x.build_fraction_left, 4);
            if (g() % 3 == 0) {
                x.hit_points = static_cast<std::int16_t>(g() % 3 == 0 ? g() : g() % 3000);
                r.write(unit_addr(x.id) + uHp, &x.hit_points, 2);
            }
        };
        const int op = static_cast<int>(g() % 12);
        if (std::getenv("OH_TRACE")) std::printf("  construction op %d (units %d, %d)\n", op, i, j);
        u32 res = 0;
        switch (op) {
        case 0: {  // one slice of building or unbuilding
            set_fraction(v);
            const float works[] = {static_cast<float>(g() % 40), -static_cast<float>(g() % 40), 0.0f,
                                   static_cast<float>(g() % 100000) / 7.0f, -static_cast<float>(g() % 3000) / 3.0f};
            const float w = works[g() % 5];
            const bool mine = sim::apply_build_progress(missions, u, v, w);
            if (!r.call(kBuildProgress, 0, {unit_addr(i), unit_addr(j), fbits(w)}, &res)) return fail("build progress");
            if ((res != 0) != mine) { std::printf("scenario %u: build progress result orig %u ours %d\n", seed, res, mine); ++st.mismatches; return true; }
            what = "build progress";
            break;
        }
        case 1: {  // decay of an unfinished unit
            set_fraction(u);
            const i32 n = static_cast<i32>(g() % 40);
            sim::unfinished_decay(missions, u, n);
            if (!r.call(kDecay, 0, {unit_addr(i), static_cast<u32>(n)})) return fail("decay");
            what = "unfinished decay";
            break;
        }
        case 2: {  // finishing
            set_fraction(v);
            const bool with = g() % 8 != 0;
            sim::finalize_built_unit(missions, &u, with ? &v : nullptr);
            if (!r.call(kFinalize, 0, {unit_addr(i), with ? unit_addr(j) : 0u})) return fail("finalize");
            what = "finalize";
            break;
        }
        case 3: {  // reclaim pulse and reclaim permission
            const i32 n = static_cast<i32>(g() % 30);
            const i32 mine = sim::reclaim_pulse(missions, u, v, n);
            if (!r.call(kPulse, 0, {unit_addr(i), unit_addr(j), static_cast<u32>(n)}, &res)) return fail("reclaim pulse");
            u32 res2 = 0;
            const bool can = sim::can_reclaim(missions, u, v);
            if (!r.call(kCanReclaim, unit_addr(i), {unit_addr(j)}, &res2)) return fail("can reclaim");
            if (static_cast<i32>(res) != mine || (res2 != 0) != can) {
                std::printf("scenario %u: reclaim pulse orig %d ours %d, can reclaim orig %u ours %d\n", seed, static_cast<i32>(res), mine, res2, can);
                ++st.mismatches;
                return true;
            }
            what = "reclaim pulse";
            break;
        }
        case 4:
        case 5: {  // yard map placement and the snapped position
            const std::uint16_t type = static_cast<std::uint16_t>(1 + g() % ntypes);
            const std::int16_t x = static_cast<std::int16_t>((u.pos[0] >> 20) + static_cast<i32>(g() % 16) - 8);
            const std::int16_t z = static_cast<std::int16_t>((u.pos[2] >> 20) + static_cast<i32>(g() % 16) - 8);
            const bool mine = sim::structure_fits(con, type, x, z);
            const u32 cell = static_cast<std::uint16_t>(x) | static_cast<u32>(static_cast<std::uint16_t>(z)) << 16;
            if (!r.call(kYardFits, 0, {L.defs + type * static_cast<u32>(kDefSize), cell, 0, 0}, &res)) return fail("yard placement");
            std::array<i32, 3> pos = {static_cast<i32>(static_cast<u32>(x) << 20) | static_cast<i32>(g() & 0xFFFFF), static_cast<i32>(g()),
                                      static_cast<i32>(static_cast<u32>(z) << 20) | static_cast<i32>(g() & 0xFFFFF)};
            r.write(L.names + 0x200, pos.data(), 12);
            sim::snap_to_footprint(con, type, pos);
            if (!r.call(kSnap, 0, {L.defs + type * static_cast<u32>(kDefSize), L.names + 0x200})) return fail("snap");
            std::array<i32, 3> theirs{};
            r.read(L.names + 0x200, theirs.data(), 12);
            ++st.placements;
            if (res) ++st.placements_ok;
            if ((res != 0) != mine || theirs != pos) {
                std::printf("scenario %u: placement of type %u at (%d, %d): orig %u ours %d, snapped orig (%x %x %x) ours (%x %x %x)\n", seed, type, x, z,
                            res, mine, theirs[0], theirs[1], theirs[2], pos[0], pos[1], pos[2]);
                ++st.mismatches;
                return true;
            }
            what = "placement";
            break;
        }
        case 6: {  // reclaiming a feature
            std::array<i32, 3> pos = {static_cast<i32>(g()), 0, static_cast<i32>(g())};
            if (!feature_cells.empty() && g() % 6) {
                const int c = feature_cells[g() % feature_cells.size()];
                pos = {static_cast<i32>(((c % grid.width) << 20) | (g() & 0xFFFFF)), 0, static_cast<i32>(((c / grid.width) << 20) | (g() & 0xFFFFF))};
            }
            r.write(L.names + 0x200, pos.data(), 12);
            const bool mine = sim::reclaim_feature(missions, u, pos);
            if (!missions.fault.empty()) return true;
            if (!r.call(kReclaimYield, 0, {unit_addr(i), L.names + 0x200}, &res)) return fail("feature reclaim");
            if ((res != 0) != mine) { std::printf("scenario %u: feature reclaim orig %u ours %d\n", seed, res, mine); ++st.mismatches; return true; }
            if (res) ++st.reclaimed_features;
            what = "feature reclaim";
            break;
        }
        case 7: {  // ownership transfer
            const int to = static_cast<int>(g() % 3);
            if (j == i) return true;
            sim::transfer_ownership(missions, u, to);
            if (!r.call(kTransfer, 0, {unit_addr(i), L.gs + 0x1B63 + static_cast<u32>(to) * kPlayerSize, 0})) return fail("transfer");
            what = "transfer ownership";
            break;
        }
        case 10: {  // transport attach / detach packet applied (the factory pad, finishing)
            if (!world.tick_has_motion(i)) return true;  // the original writes through the motion pointer
            // Attach (or detach), then always detach again so the other systems find no cargo lists.
            for (int pass = 0; pass < 2; ++pass) {
            sim::Unit* t = pass || g() % 3 == 0 ? nullptr : &v;
            const std::uint8_t piece = static_cast<std::uint8_t>(pass || g() % 4 == 0 ? 0xFF : g() % 6);
            const std::uint8_t layer = static_cast<std::uint8_t>(pass ? 1 : g() % 4);
            std::uint8_t pk[8] = {0x0A, static_cast<std::uint8_t>(u.id), static_cast<std::uint8_t>(u.id >> 8),
                                  static_cast<std::uint8_t>(t ? t->id : 0), static_cast<std::uint8_t>(t ? t->id >> 8 : 0), piece, layer, 0};
            r.write(L.names + 0x400, pk, 8);
            sim::apply_transport_attach(missions, world.links, u, t, piece, layer);
            if (!missions.fault.empty()) return true;
            if (!r.call(0x0048AB70, 0, {L.names + 0x400})) return fail("transport attach");
            for (int k : present) {
                const sim::Unit& x = units[static_cast<std::size_t>(k)];
                const u32 at = unit_addr(k);
                u32 carrier = r.r32(at + uCarrier), cargo = r.r32(at + uCargoHead), next = r.r32(at + 0x8E);
                norm_ptr(carrier);
                norm_ptr(cargo);
                norm_ptr(next);
                const std::uint8_t tp = r.rd<std::uint8_t>(at + 0xF9);
                const u32 head = r.r32(world.bucket_addr(k) + 6);
                u32 head_n = head;
                norm_ptr(head_n);
                const sim::Unit* mine_head = world.links.heads[static_cast<std::size_t>(k)];
                bool ok = carrier == (x.carrier ? World::tok(*x.carrier) : 0u) && cargo == (x.cargo_head ? World::tok(*x.cargo_head) : 0u) &&
                          next == (x.cargo_next ? World::tok(*x.cargo_next) : 0u) && tp == x.transport_piece &&
                          head_n == (mine_head ? World::tok(*mine_head) : 0u);
                if (world.tick_has_motion(k)) ok = ok && r.rd<std::uint8_t>(world.motions[static_cast<std::size_t>(k)].block + 0x2E) == tstate[static_cast<std::size_t>(k)].motion.flags;
                if (!ok) {
                    std::printf("scenario %u: transport links of unit %d differ\n", seed, k);
                    ++st.mismatches;
                    return true;
                }
            }
            }
            ++st.transports;
            what = "transport attach";
            break;
        }
        case 11: {  // a factory's build queue changes (command input)
            static const std::uint8_t kinds[] = {sim::mission::kBuildingBuild, sim::mission::kMobileBuild, sim::mission::kBuildWeapon};
            const std::uint8_t kind = kinds[g() % 3];
            const i32 type = static_cast<i32>(g() % 3);
            const i32 delta = static_cast<i32>(g() % 13) - 6;
            sim::adjust_build_count(missions, u, kind, type, delta);
            if (!missions.fault.empty()) return true;
            if (!r.call(0x0043B0B0, 0, {kind, unit_addr(i), static_cast<u32>(type), static_cast<u32>(delta)})) return fail("build count");
            what = "build count";
            break;
        }
        default: {  // a nano stream: start, then the effect sweep for a few steps
            std::array<i32, 6> a{}, b{};
            for (int k = 0; k < 3; ++k) {
                a[static_cast<std::size_t>(k)] = static_cast<i32>(g() % 0x10000000);
                a[static_cast<std::size_t>(k + 3)] = a[static_cast<std::size_t>(k)] + (g() % 3 ? static_cast<i32>(g() % 0x400000) : 0);
                b[static_cast<std::size_t>(k)] = a[static_cast<std::size_t>(k)] + static_cast<i32>(g() % 0x2000000) - 0x1000000;
                b[static_cast<std::size_t>(k + 3)] = b[static_cast<std::size_t>(k)] + static_cast<i32>(g() % 0x800000);
            }
            if (g() % 2) std::swap(a, b);
            const u32 obj = L.nano;
            std::vector<std::uint8_t> z(0x4C, 0);
            r.write(obj, z.data(), z.size());
            r.w32(obj, kNanoVtable);
            r.write(L.names + 0x300, a.data(), 24);
            r.write(L.names + 0x320, b.data(), 24);
            u32 tick = env.game_tick;
            sim::NanoSprayState ns;
            sim::spawn_nano_stream(ns, world.crt, tick, a, b, 1);
            if (!r.call(kNanoInit, obj, {L.names + 0x300, L.names + 0x320, 1})) return fail("nano start");
            auto check = [&](const char* when) {
                bool ok = r.r32(L.rand_state) == world.crt.state();
                const u32 begin = r.r32(obj + 0x10), end = r.r32(obj + 0x14);
                const std::size_t n = begin ? (end - begin) / 0x30 : 0;
                const sim::NanoStream* sm = ns.streams.empty() ? nullptr : &ns.streams[0];
                const std::size_t mine_n = sm ? sm->particles.size() : 0;
                ok = ok && n == mine_n;
                for (std::size_t k = 0; ok && k < n; ++k) {
                    u32 w[12];
                    r.read(begin + static_cast<u32>(k) * 0x30, w, 48);
                    const sim::NanoParticle& q = sm->particles[k];
                    for (int c = 0; c < 3; ++c)
                        ok = ok && w[c] == static_cast<u32>(q.pos[static_cast<std::size_t>(c)]) && w[3 + c] == static_cast<u32>(q.target[static_cast<std::size_t>(c)]) &&
                             w[6 + c] == static_cast<u32>(q.vel[static_cast<std::size_t>(c)]);
                    ok = ok && w[9] == q.flags && w[10] == q.colour && static_cast<i32>(w[11]) == q.expire;
                }
                if (sm) ok = ok && static_cast<i32>(r.r32(obj + 4)) == sm->deadline && r.r32(obj + 8) == sm->next_emit;
                if (!ok) {
                    std::printf("scenario %u: nano stream differs %s (particles orig %zu ours %zu, rand orig %08x ours %08x)\n", seed, when, n, mine_n,
                                r.r32(L.rand_state), world.crt.state());
                    ++st.mismatches;
                }
                return ok;
            };
            if (!check("after start")) return true;
            ++st.nano_streams;
            for (int step = 0; step < 40; ++step) {
                tick += 1;
                r.w32(L.gs + 0x38A47, tick);
                const u32 begin = r.r32(obj + 0x10), end = r.r32(obj + 0x14);
                const bool done = !begin || end == begin;
                sim::step_nano_streams(ns, world.crt, tick);
                if (done) break;
                if (!r.call(kNanoAdvance, obj, {})) return fail("nano sweep");
                if (!check("in the sweep")) return true;
                ++st.nano_steps;
            }
            r.w32(L.gs + 0x38A47, env.game_tick);
            what = "nano stream";
            break;
        }
        }
        return true;
    };

    set_env(0);
    for (int i : present) {
        r.write(L.names, "Create", 7);
        if (!r.call(kCallNamed, remote_script[static_cast<std::size_t>(i)], {L.names, 0, 1})) return fail("Create");
        units[static_cast<std::size_t>(i)].script->call("Create", nullptr, true);
    }
    for (int i : present) {
        const int n = static_cast<int>(1 + g() % 6);
        for (int k = 0; k < n; ++k) {
            deal_steps();
            if (!create_order(i)) return false;
        }
    }
    if (engine_faulted()) return true;
    if (!compare("after setup")) return true;

    for (int tick = 0; tick < ticks_to_run; ++tick) {
        set_env(tick);
        const int nops = static_cast<int>(g() % 6);
        for (int op = 0; op < nops; ++op) {
            const int i = any_unit();
            if (!alive(i)) continue;
            sim::Unit& u = units[static_cast<std::size_t>(i)];
            deal_steps();
            std::string what;
            int kind_of_op = static_cast<int>(g() % 17);
            if (kind_of_op == 12) kind_of_op = 0;
            else if (kind_of_op == 13) kind_of_op = 3;
            else if (kind_of_op >= 14) kind_of_op = 20;  // a construction routine called directly
            if (std::getenv("OH_TRACE")) std::printf("  op %d on unit %d\n", kind_of_op, i);
            if (kind_of_op < 2) {
                if (!create_order(i)) return false;
                what = "create order";
            } else if (kind_of_op == 2) {  // primary orders, called directly
                u32 stale = random_stale();
                const u32 edi = static_cast<u32>(g());
                if (u.orders && g() % 2) {  // an event arrives for the head
                    sim::MissionOrder* o = u.orders;
                    o->events |= g() % 3 ? o->wait : 0x10000u;
                    r.w32(o->address + oEvents, o->events);
                }
                missions.caller = sim::MissionCaller::Primary;
                if (std::getenv("OH_TRACE")) {
                    std::printf("  direct primary orders of unit %d (remote list before):\n", i);
                    for (u32 a = r.r32(unit_addr(i) + uOrders), n = 0; a && n < 40; a = r.r32(a + oNext), ++n)
                        std::printf("    %08x kind %u step %u wait %x wake %u events %x flags %x\n", a, r.rd<std::uint8_t>(a + oKind),
                                    r.rd<std::uint8_t>(a + oStep), r.r32(a + oWait), r.r32(a + oWake), r.r32(a + oEvents), r.r32(a + oFlags));
                }
                sim::run_primary_orders(octx, u);
                if (engine_faulted()) return true;
                if (!call_one(kPrimary, unit_addr(i), edi, stale)) return fail("primary orders");
                if (!check_stale(edi, "primary orders")) return true;
                what = "primary orders";
            } else if (kind_of_op == 3) {  // secondary orders
                for (sim::MissionOrder* o = u.secondary_orders; o; o = o->next)
                    if (g() % 2) {  // due now
                        o->wait = g() % 4 ? 0u : 1u;
                        o->wake_tick = env.game_tick - g() % 3;
                        r.w32(o->address + oWait, o->wait);
                        r.w32(o->address + oWake, o->wake_tick);
                    }
                u32 stale = random_stale();
                const u32 before = stale;
                missions.caller = sim::MissionCaller::Secondary;
                sim::run_secondary_orders(octx, u, stale);
                if (engine_faulted()) return true;
                if (!call_one(kSecondary, unit_addr(i), 0, before)) return fail("secondary orders");
                if (!check_stale(stale, "secondary orders")) return true;
                ++st.secondary_stale[stale == before ? "unchanged" : stale == 0 ? "event 0" : stale == 15 ? "15 (random draw)"
                                     : world.all_orders.count(stale) ? "order address" : "behaviour's event slot"];
                what = "secondary orders";
            } else if (kind_of_op == 4 || kind_of_op == 11) {  // ground settle
                if (g() % 3) {
                    u.flags = (u.flags & ~3u) | 0x10000u | (g() % 5 ? 1u : g() % 4);
                    r.w32(unit_addr(i) + uFlags, u.flags);
                }
                if (g() % 4 == 0) {  // a fresh unit position
                    const i32 x = static_cast<i32>((g() % static_cast<u32>(hm.width() * 16 - 64) + 32) << 16 | (g() & 0xFFFF));
                    const i32 z = static_cast<i32>((g() % static_cast<u32>(hm.height() * 16 - 64) + 32) << 16 | (g() & 0xFFFF));
                    u.pos[0] = x;
                    u.pos[2] = z;
                    u.heading = static_cast<std::uint16_t>(g());
                    r.write(unit_addr(i) + uPosX, &u.pos[0], 4);
                    r.write(unit_addr(i) + uPosZ, &u.pos[2], 4);
                    r.write(unit_addr(i) + uHeading, &u.heading, 2);
                }
                u32 stale = random_stale();
                const u32 before = stale;
                world.settle(u, stale);
                if (!call_one(kSettle, unit_addr(i), 0, before)) return fail("settle");
                if (!check_stale(stale, "settle")) return true;
                ++st.settles;
                ++st.settle_paths[stale == before ? 0u : stale];
                what = "settle";
            } else if (kind_of_op == 20) {
                if (!construction_op(i, what)) return what.empty() ? true : false;
                if (what.empty()) continue;
            } else if (kind_of_op == 5) {  // clear orders
                const bool all = g() % 2;
                sim::clear_orders(octx, u, all);
                if (engine_faulted()) return true;
                if (!r.call(kClear, 0, {unit_addr(i), all ? 1u : 0u})) return fail("clear orders");
                what = all ? "clear all orders" : "clear orders";
            } else if (kind_of_op == 6) {  // remove one order
                const auto v = all_orders_of(i);
                if (v.empty()) continue;
                sim::MissionOrder* o = v[g() % v.size()];
                const u32 addr = o->address;
                sim::remove_order(octx, u, o);
                if (engine_faulted()) return true;
                if (!r.call(kRemove, 0, {unit_addr(i), addr})) return fail("remove order");
                what = "remove order";
            } else if (kind_of_op == 7) {  // paralysis through the damage routine
                const int ai = g() % 3 == 0 ? 0 : any_unit();
                const i32 amount = static_cast<i32>(g() % 600);
                const i32 heading = static_cast<i32>(g());
                sim::apply_damage(combat, ai ? &units[static_cast<std::size_t>(ai)] : nullptr, u, amount, sim::damage_kind::kParalyze, heading);
                if (!r.call(kApplyDamage, 0, {ai ? unit_addr(ai) : 0u, unit_addr(i), static_cast<u32>(amount), 2u, static_cast<u32>(heading)}))
                    return fail("paralysis");
                ++st.paralysis;
                what = "paralysis";
            } else if (kind_of_op == 8) {  // kill
                const i32 kind = static_cast<i32>(g() % 12);
                u32 stale = random_stale();
                r.w32(L.ret_val + kStaleIn, stale);
                r.w32(L.ret_val + kKillKind, static_cast<u32>(kind));
                r.w32(L.ret_val + kKillUnit2, unit_addr(i));
                sim::kill_unit(combat, u, kind, stale);
                if (engine_faulted()) return true;
                if (!r.call(L.caller_kill, 0, {})) return fail("kill");
                if (!check_stale(stale, "kill")) return true;
                ++st.deaths;
                what = "kill";
            } else {  // order state changes, pending events, motion state
                const auto v = all_orders_of(i);
                if (!v.empty()) tweak_order(i, *v[g() % v.size()]);
                u.net_dirty = static_cast<std::uint16_t>(g() % 2 ? g() : 0);
                r.write(unit_addr(i) + uNetDirty, &u.net_dirty, 2);
                if (g() % 3 == 0) {
                    u.flags ^= g() % 2 ? 0x10000u : static_cast<u32>(g() % 4);
                    r.w32(unit_addr(i) + uFlags, u.flags);
                }
                MotionInfo& m = world.motions[static_cast<std::size_t>(i)];
                if (m.block) {
                    m.speed = static_cast<i32>(g() % 0x60000) - (g() % 8 == 0 ? 0x30000 : 0);
                    m.start = env.game_tick - g() % 80;
                    r.w32(m.block + mSpeed, static_cast<u32>(m.speed));
                    r.w32(m.block + mStart, m.start);
                }
                what = "tweak";
            }
            ++st.ops;
            ++st.by_op[what];
            if (engine_faulted()) return true;
            if (!compare(what + " on unit " + std::to_string(i) + " tick " + std::to_string(tick))) return true;
        }

        // One unit pass.
        deal_steps();
        env.stale_word = random_stale();
        r.w32(L.ret_val + kStaleIn, env.stale_word);
        env_live = sim::run_unit_pass(units, tstate, players, env, world, camera);
        if (engine_faulted()) return true;
        if (!r.call(L.caller_pass, 0, {})) return fail("unit pass");
        if (!check_stale(env.stale_word, "unit pass tick " + std::to_string(tick))) return true;
        {
            const u32 w = env.stale_word;
            const bool player_rec = w >= L.gs + 0x1B63 && w < L.gs + 0x1B63 + 10 * kPlayerSize && (w - L.gs - 0x1B63) % kPlayerSize == 0;
            ++st.pass_stale[player_rec ? "player record (primary orders)"
                            : (w >= 0x0048A8FF && w <= 0x0048A96F) ? "settle return address"
                            : world.all_orders.count(w) ? "order address (secondary orders)"
                            : w == 15 ? "15" : "other (wreck level, script, behaviour slot...)"];
        }
        ++st.ticks;
        if (!compare("unit pass tick " + std::to_string(tick))) return true;
    }
    return true;
}

} // namespace

LONG WINAPI report_crash(EXCEPTION_POINTERS* e) {
    std::printf("harness crash %08lx at %p (module base %p)\n", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress,
                static_cast<void*>(GetModuleHandleA(nullptr)));
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(report_crash);
    if (argc < 3) {
        std::fprintf(stderr, "usage: orders_harness <TotalA.exe> <game_dir> [scenarios] [ticks] [first scenario]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int scenarios = argc > 3 ? std::atoi(argv[3]) : 50;
    const int ticks = argc > 4 ? std::atoi(argv[4]) : 100;
    const int first = argc > 5 ? std::atoi(argv[5]) : 0;  // lets large runs be split into shards
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
    int with_footprint = 0;
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
        t.settle.has_footprint = ta::selection_footprint(t.model_file, m.root.get(), t.settle.footprint);
        with_footprint += t.settle.has_footprint;
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
    std::string report;
    if (!r.start(argv[1]) || !setup(r, L, hm, report)) {
        std::fprintf(stderr, "cannot start the original (%s)\n", report.c_str());
        return 1;
    }
    std::printf("%s\n", report.c_str());
    Stats st;
    for (int s = first; s < first + scenarios; ++s) {
        if (!run_scenario(r, L, types, hm, ticks, static_cast<unsigned>(s * 7 + 11), st)) {
            ++st.failures;
            std::printf("scenario %d: original call failed - restarting\n", s);
            r.stop();
            if (!r.start(argv[1]) || !setup(r, L, hm, report)) return 1;
        }
    }
    std::printf("unit types %zu (%d with a footprint)\n", types.size(), with_footprint);
    std::printf("scenarios %zu, unit passes %zu, operations %zu, behaviour steps %zu, orders made %zu, orders freed %zu, direct settles %zu, "
                "paralysis hits %zu, kills %zu\n",
                st.scenarios, st.ticks, st.ops, st.handler_calls, st.orders_made, st.orders_freed, st.settles, st.paralysis, st.deaths);
    std::printf("operations:\n");
    for (const auto& [k, v] : st.by_op) std::printf("  %s: %zu\n", k.c_str(), v);
    std::printf("direct settle paths (stale word left):\n");
    for (const auto& [k, v] : st.settle_paths) std::printf("  %08x: %zu\n", k, v);
    std::printf("direct secondary orders, stale word left:\n");
    for (const auto& [k, v] : st.secondary_stale) std::printf("  %s: %zu\n", k.c_str(), v);
    std::printf("unit pass, stale word left:\n");
    for (const auto& [k, v] : st.pass_stale) std::printf("  %s: %zu\n", k.c_str(), v);
    std::printf("real behaviour steps by kind (results 0..9, other):\n");
    const auto names = ta::UnitDefContext::default_mission_names();
    for (int k = 0; k < sim::mission::kCount; ++k) {
        std::size_t total = 0;
        for (std::size_t v : g_real_runs[static_cast<std::size_t>(k)]) total += v;
        if (!total) continue;
        std::printf("  %-20s %7zu:", std::string(names[static_cast<std::size_t>(k)]).c_str(), total);
        for (std::size_t v : g_real_runs[static_cast<std::size_t>(k)]) std::printf(" %zu", v);
        std::printf("\n");
    }
    std::printf("real behaviour steps that changed their event argument: %zu\n", g_event_slot_writes);
    std::printf("construction: placements %zu (%zu fit), features reclaimed %zu, nano streams %zu (%zu sweep steps)\n", st.placements,
                st.placements_ok, st.reclaimed_features, st.nano_streams, st.nano_steps);
    std::printf("transport packets applied %zu\n", st.transports);
    std::printf("mismatching scenarios %zu, engine faults %zu, original failures %zu\n", st.mismatches, st.faults, st.failures);
    return st.mismatches || st.failures ? 1 : 0;
}
