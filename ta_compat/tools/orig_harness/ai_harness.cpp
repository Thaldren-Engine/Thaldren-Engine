// Differential tester for the computer player (sim/ai.h). Inside a suspended
// TotalA.exe it builds a small game state: the real unit and weapon definition
// tables (so build lists, sides, costs and weapons are the game's), three players
// with units of random real types, a map grid with metal and features, fog grids,
// the players' groups, controllers and unit tables made by the original routines.
// The original routines then run for real: the controller step (0x408C40, its
// weapon-target pass stubbed), every role's timer action (through the behaviour
// objects' vtables), group assignment (0x408830), unit table refresh (0x40B2C0)
// and rescoring (0x409730), the build pick (0x40BDB0) and score (0x40BB00), the
// site search (0x40BFE0), the nearest enemy (0x4071F0), the group routine
// (0x480250), the profile commands (0x406C90, 0x406DB0, 0x406E40, 0x409DC0,
// 0x409E90) and the metal spot collection (0x40B370). Stubs, answering from queues
// shared with the engine side and logging their arguments: mission resolution
// (0x43F0E0), the general order routine (0x43ADC0), the build menu command
// (0x419B00), activation (0x48B090), the placement checks (0x47DB70, 0x47D2E0, with
// the metal sum they leave), the weapon range check (0x49AA80), the explored-map
// test (0x408090) and the line-of-sight test (0x465AC0). The constructors' site
// height (a stale stack word in the original) is planted in the original's stack
// and given to the engine as its input. After every operation it compares the
// stub logs, the battle RNG, the tables, controllers, roles, groups and unit flags.
//
// Usage: ai_harness <TotalA.exe> <game_dir> [scenarios] [steps] [first scenario]

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/ai.h"
#include "sim/combat.h"
#include "sim/missions.h"
#include "sim/orders.h"
#include "sim/rng.h"
#include "sim/targeting.h"
#include "sim/unit.h"
#include "sim/unit_missions.h"
#include "sim/unit_tick.h"
#include "sim/weapons.h"
#include "ta/ai_profile.h"
#include "ta/battle_types.h"
#include "ta/crt_strtod.h"
#include "ta/hpi.h"
#include "ta/unitdefs.h"
#include "ta/weapondefs.h"

namespace {

using harness::u32;
using i32 = std::int32_t;
using u16 = std::uint16_t;
using u8 = std::uint8_t;
using Pos = std::array<i32, 3>;

constexpr u32 kGameStatePtr = 0x00511DE8, kBattleRngState = 0x0051FC88, kTablePtrs = 0x005119C0;
constexpr u32 kPlaceMetal = 0x0051E688, kGate = 0x00501774;
constexpr u32 kGroupsInit = 0x00480190, kControllerCtor = 0x00408CB0, kTableCreate = 0x0040B320, kController = 0x00408C40;
constexpr u32 kAssign = 0x00408830, kRefresh = 0x0040B2C0, kRescore = 0x00409730, kPick = 0x0040BDB0, kScore = 0x0040BB00;
constexpr u32 kAirBases = 0x0040B530, kSeek = 0x0040BFE0, kNearest = 0x004071F0, kSetGroup = 0x00480250, kPlan = 0x00406C90, kWeight = 0x00406DB0;
constexpr u32 kLimit = 0x00406E40, kWeightMask = 0x00409DC0, kLimitMask = 0x00409E90, kMetalSites = 0x0040B370;
constexpr u32 kSwitch = 0x00408670, kScoutTick = 0x00408100, kScoutDtor = 0x00408600;
constexpr u32 kUnitSize = 0x118, kPlayerSize = 0x14B, kDefSize = 0x249, kWeaponSize = 0x115;
constexpr int kMaxUnits = 30, kPlayers = 3, kPer = kMaxUnits / kPlayers;
constexpr int kMapW = 64, kMapH = 64;

// Remote offsets.
enum : u32 {
    uMotion = 0x00, uOrders = 0x5C, uPosX = 0x6A, uPosY = 0x6E, uPosZ = 0x72, uDef = 0x92, uOwner = 0x96, uDefIndex = 0xA6, uId = 0xA8,
    uGroup = 0xAC, uOwnerSlot = 0xFF, uBuildLeft = 0x104, uActivate = 0x10E, uFlags = 0x110,
    pActive = 0x00, pFirst = 0x67, pLast = 0x6B, pController = 0x73, pControllerObj = 0x74, pGroups = 0x78, pFog = 0x7C, pFogW = 0x80,
    pFogH = 0x84, pEnergy = 0x8C, pEIncome = 0x90, pEUsage = 0x94, pMetal = 0x98, pMIncome = 0x9C, pMUsage = 0xA0, pEStorage = 0xA4,
    pMStorage = 0xA8, pAllied = 0x108, pAlive = 0x144, pState = 0x146,
    gWorldW = 0x14223, gWorldH = 0x14227, gPixW = 0x1422B, gPixH = 0x1422F, gMapW = 0x14233, gMapH = 0x14237, gWindMax = 0x1425F,
    gTidal = 0x14267, gFeatures = 0x1426F, gFeatureCount = 0x14253, gCells = 0x14287, gSightFlags = 0x14281, gUnitsPerPlayer = 0x1434F,
    gUnits = 0x14357, gUnitsEnd = 0x1435B, gDefCount = 0x1438F, gDefs = 0x1439B, gWindDenom = 0x37EC8, gWindFraction = 0x37EDE,
    gDifficulty = 0x37EEE, gTick = 0x38A47, gMapInfo = 0x391E9, gLosPlayer = 0x2A43, gLocalPlayer = 0x2A42,
    cHold = 0x0D, cCountdown = 0x05, cSlots = 0x11,
    bNext = 0x0C, bMin = 0x14, bMax = 0x18, bDist = 0x1C, bPeer = 0x20, bSeek = 0x24, bFollow = 0x14, hBest = 0x14, hProbe = 0x20,
    hStep = 0x2C, hThreat = 0x38,
    tAnchor = 0x35, tAirBegin = 0x29, tAirEnd = 0x2D, tSitesBegin = 0x51, tSitesEnd = 0x55, tScores = 0x69, tBuilders = 0x75, tCounts = 0x81,
    tThreat = 0x91, tAnchorW = 0xA1, tWeight = 0xB1, tWeightFixed = 0xC1, tLimit = 0xD1, tLimitFixed = 0xE1, tRefresh = 0xED,
    tGrid = 0xF1, tRadius = 0x109,
};

// --- Stubs -------------------------------------------------------------------

enum : int { qResolve, qPlace, qFits, qMetal, qRange, qExplored, qVisible, kQueues };
constexpr u32 kQueueSize = 0x4000;
enum : int { kResolveId = 80, kEnqueueId, kMenuId, kActivateId, kPlaceId, kFitsId, kRangeId, kExploredId, kVisibleId };

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8v(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void bytes(std::initializer_list<std::uint8_t> l) { for (auto v : l) b.push_back(v); }
    void align() { while (b.size() % 16) b.push_back(0xCC); }
};

// What a stub logs: stack argument i, ECX, a word through pointer argument i (0 for null), or
// whether pointer argument i is set.
struct Item {
    enum Kind { Arg, Ecx, Deref, Set, ArgByte } kind;
    int arg = 0;
    std::uint8_t off = 0;
};
struct StubSpec {
    u32 address;
    int id;
    int args;           // stack arguments popped on return
    std::vector<Item> items;
    int queue = -1;     // answer queue (-1: returns 0)
    int metal_queue = -1;  // queue whose answer goes to the placement metal sum first
    bool answer_through_arg0 = false;  // the answer byte is written to *arg0 and arg0 returned
    bool silent = false;
};

void emit_queue_read(Emitter& e, u32 q) {
    e.u8v(0x8B); e.u8v(0x0D); e.u32le(q);      // mov ecx,[q]
    e.u8v(0x3B); e.u8v(0x0D); e.u32le(q + 4);  // cmp ecx,[q+4]
    e.bytes({0x72, 0x04});                     // jb have
    e.bytes({0x33, 0xC0});                     // xor eax,eax
    e.bytes({0xEB, 0x09});                     // jmp done
    e.bytes({0x8B, 0x01});                     // have: mov eax,[ecx]
    e.u8v(0x83); e.u8v(0x05); e.u32le(q); e.u8v(0x04);  // add dword [q],4
}

void emit_stub(Emitter& e, const StubSpec& s, u32 log_ptr, u32 queues) {
    if (!s.silent) {
        e.u8v(0xA1); e.u32le(log_ptr);                                    // mov eax,[log]
        e.u8v(0xC7); e.u8v(0x00); e.u32le(static_cast<u32>(s.id));       // mov [eax],id
        std::uint8_t at = 4;
        for (const Item& it : s.items) {
            const std::uint8_t sp = static_cast<std::uint8_t>(4 + 4 * it.arg);
            switch (it.kind) {
            case Item::Ecx: e.bytes({0x89, 0x48, at}); break;  // mov [eax+at],ecx
            case Item::Arg: e.bytes({0x8B, 0x54, 0x24, sp, 0x89, 0x50, at}); break;
            case Item::ArgByte: e.bytes({0x0F, 0xB6, 0x54, 0x24, sp, 0x89, 0x50, at}); break;  // movzx edx,byte [esp+sp]
            case Item::Deref:
                e.bytes({0x8B, 0x54, 0x24, sp, 0x85, 0xD2, 0x74, 0x03, 0x8B, 0x52, it.off, 0x89, 0x50, at});
                break;
            case Item::Set: e.bytes({0x8B, 0x54, 0x24, sp, 0xF7, 0xDA, 0x1B, 0xD2, 0xF7, 0xDA, 0x89, 0x50, at}); break;
            }
            at = static_cast<std::uint8_t>(at + 4);
        }
        e.u8v(0x83); e.u8v(0x05); e.u32le(log_ptr); e.u8v(at);  // add dword [log],at
    }
    if (s.metal_queue >= 0) {
        emit_queue_read(e, queues + static_cast<u32>(s.metal_queue) * kQueueSize);
        e.u8v(0xA3); e.u32le(kPlaceMetal);  // mov [metal],eax
    }
    if (s.queue >= 0) emit_queue_read(e, queues + static_cast<u32>(s.queue) * kQueueSize);
    else e.bytes({0x33, 0xC0});
    if (s.answer_through_arg0) e.bytes({0x8B, 0x54, 0x24, 0x04, 0x88, 0x02, 0x8B, 0xC2});  // mov edx,[esp+4]; mov [edx],al; mov eax,edx
    if (s.args == 0) e.u8v(0xC3);
    else { e.u8v(0xC2); e.u8v(static_cast<std::uint8_t>(4 * s.args)); e.u8v(0x00); }
}

std::vector<StubSpec> stub_specs() {
    using K = Item;
    return {
        // resolve(out, intent, unit, target, pos) -> out
        {0x0043F0E0, kResolveId, 5, {{K::ArgByte, 1}, {K::Arg, 2}, {K::Arg, 3}, {K::Set, 4}, {K::Deref, 4, 0}, {K::Deref, 4, 4}, {K::Deref, 4, 8}},
         qResolve, -1, true},
        // enqueue(kind, queued, unit, target, pos, p0, p1)
        {0x0043ADC0, kEnqueueId, 7, {{K::ArgByte, 0}, {K::Arg, 1}, {K::Arg, 2}, {K::Arg, 3}, {K::Set, 4}, {K::Deref, 4, 0}, {K::Deref, 4, 4}, {K::Deref, 4, 8}, {K::Arg, 5}, {K::Arg, 6}}},
        // build menu(name, builder, delta)
        {0x00419B00, kMenuId, 3, {{K::Arg, 0}, {K::Arg, 1}, {K::Arg, 2}}},
        // activate [thiscall unit](mask, on)
        {0x0048B090, kActivateId, 2, {{K::Ecx}, {K::Arg, 0}, {K::Arg, 1}}},
        // can place(def, exclude, cell, mode)
        {0x0047DB70, kPlaceId, 4, {{K::Arg, 0}, {K::Arg, 1}, {K::Arg, 2}, {K::Arg, 3}}, qPlace, qMetal},
        // structure fits(def, cell, exclude, view)
        {0x0047D2E0, kFitsId, 4, {{K::Arg, 0}, {K::Arg, 1}, {K::Arg, 2}, {K::Arg, 3}}, qFits, qMetal},
        // weapon range(unit, from, target, slot)
        {0x0049AA80, kRangeId, 4, {{K::Arg, 0}, {K::Deref, 2, 0}, {K::Deref, 2, 4}, {K::Deref, 2, 8}, {K::ArgByte, 3}}, qRange},
        // explored(player, pos)
        {0x00408090, kExploredId, 2, {{K::Arg, 0}, {K::Deref, 1, 0}, {K::Deref, 1, 4}, {K::Deref, 1, 8}}, qExplored},
        // visible(player, unit)
        {0x00465AC0, kVisibleId, 2, {{K::Arg, 0}, {K::Arg, 1}}, qVisible},
        // the weapon-target pass at the end of the controller step: not part of this test
        {0x004089A0, 0, 1, {}, -1, -1, false, true},
        // free and the C runtime's operator delete (cdecl)
        {0x004B4F20, 0, 0, {}, -1, -1, false, true},
        {0x004E8820, 0, 0, {}, -1, -1, false, true},
    };
}

std::size_t record_words(u32 id) {
    for (const StubSpec& s : stub_specs())
        if (!s.silent && static_cast<u32>(s.id) == id) return 1 + s.items.size();
    return 0;
}

struct Layout {
    u32 heap = 0, scenario_heap = 0, bump_ptr = 0, stubs = 0, log_ptr = 0, log_base = 0, gs = 0, units = 0, defs = 0, options = 0, weapons = 0,
        cells = 0, features = 0, fog = 0, mapinfo = 0, queues = 0, out = 0, orders = 0, scout_stub = 0, scout_vtable = 0, plant = 0, line = 0,
        text = 0;
};

// The profile commands in the original's command registry (0x406F00) and every category of
// the definition table in its category registry (0x488C50), with the table's masks.
bool register_commands(harness::Remote& r, Layout& L, const ta::UnitDefTable& defs, std::string& report) {
    if (!r.call(0x00406F00, 0, {})) {
        report = "command registration failed";
        return false;
    }
    for (const auto& [key, entry] : defs.categories.all()) {
        const std::string& name = entry.first;
        r.write(L.line + 0x400, name.c_str(), name.size() + 1);
        u32 mask = 0;
        if (!r.call(0x00488C50, 0, {L.line + 0x400}, &mask) || !mask) {
            report = "category registration failed";
            return false;
        }
        r.write(mask, entry.second.data(), 64);
    }
    // The registrations' memory stays: scenarios start their heap after it.
    L.scenario_heap = (r.r32(L.bump_ptr) + 0xFFF) & ~0xFFFu;
    return true;
}

bool setup(harness::Remote& r, Layout& L, std::string& report) {
    L.heap = r.alloc(64u << 20);
    L.bump_ptr = r.alloc(0x1000);
    L.stubs = r.alloc(0x2000, PAGE_EXECUTE_READWRITE);
    L.log_ptr = r.alloc(0x1000);
    L.log_base = r.alloc(4u << 20);
    L.gs = r.alloc(0x40000);
    L.units = r.alloc((kMaxUnits + 2) * kUnitSize);
    L.defs = r.alloc(1024 * kDefSize);
    L.options = r.alloc(1024 * 64);
    L.weapons = r.alloc(256 * kWeaponSize);
    L.cells = r.alloc(kMapW * kMapH * 13);
    L.features = r.alloc(0x10000);
    L.fog = r.alloc(0x10000);
    L.mapinfo = r.alloc(0x1000);
    L.queues = r.alloc(kQueues * kQueueSize);
    L.out = r.alloc(0x1000);
    L.orders = r.alloc(0x2000);
    L.line = r.alloc(0x1000);
    L.text = r.alloc(0x10000);
    if (!L.heap || !L.stubs || !L.log_base || !L.gs || !L.units || !L.defs || !L.queues) {
        report = "allocation failed";
        return false;
    }
    Emitter e;
    auto here = [&]() { return L.stubs + static_cast<u32>(e.b.size()); };
    // Bump allocators in place of the heap routines.
    // (and the C runtime's operator new, which the command and category registries use)
    const int size_at[4] = {8, 4, 12, 4};
    const u32 targets[4] = {0x004D83B0, 0x004B4F10, 0x004D84A0, 0x004E8890};
    for (int k = 0; k < 4; ++k) {
        const u32 at = here();
        e.u8v(0xA1); e.u32le(L.bump_ptr);
        e.u8v(0x8B); e.u8v(0x4C); e.u8v(0x24); e.u8v(static_cast<std::uint8_t>(size_at[k]));
        e.u8v(0x83); e.u8v(0xC1); e.u8v(0x03); e.u8v(0x83); e.u8v(0xE1); e.u8v(0xFC);
        e.u8v(0x01); e.u8v(0x0D); e.u32le(L.bump_ptr);
        e.u8v(0xC3);
        e.align();
        if (!r.patch_jump(targets[k], at)) return false;
    }
    for (const StubSpec& s : stub_specs()) {
        const u32 at = here();
        emit_stub(e, s, L.log_ptr, L.queues);
        e.align();
        if (!r.patch_jump(s.address, at)) {
            report = "patch failed";
            return false;
        }
    }
    // The constructors' tick through a stub that plants the site height word where the
    // original's stack holds it (its entry stack pointer - 0x2C), then jumps to the tick.
    L.plant = L.out + 0x800;
    L.scout_stub = here();
    e.u8v(0xA1); e.u32le(L.plant);                  // mov eax,[plant]
    e.bytes({0x89, 0x44, 0x24, 0xD4});              // mov [esp-0x2C],eax
    e.u8v(0xE9);
    e.u32le(kScoutTick - (here() + 4));             // jmp tick
    e.align();
    if (e.b.size() > 0x2000 || !r.write(L.stubs, e.b.data(), e.b.size())) {
        report = "stub write failed";
        return false;
    }
    L.scout_vtable = L.out + 0x900;
    r.w32(L.scout_vtable, L.scout_stub);
    r.w32(L.scout_vtable + 4, kScoutDtor);
    r.w32(L.bump_ptr, L.heap);
    r.w32(L.log_ptr, L.log_base);
    r.w32(kGameStatePtr, L.gs);
    L.scenario_heap = (r.r32(L.bump_ptr) + 0xFFF) & ~0xFFFu;
    report = "stubs ready";
    return true;
}

// --- The engine side's world --------------------------------------------------

struct World : sim::UnitWorld, sim::CombatSystems, sim::OrderSystems, sim::MissionSystems, sim::TargetingSystems, sim::AiSystems {
    std::vector<sim::Unit>* units = nullptr;
    sim::BattleRng rng;
    std::vector<u32> log;
    std::array<std::vector<u32>, kQueues> queue;
    std::array<std::size_t, kQueues> queue_at{};
    i32 metal_sum = 0;
    bool fog = false;
    std::array<u32, 10> fog_w{}, fog_h{};
    std::vector<std::vector<u8>> fog_counts;
    sim::Ai* ai = nullptr;
    std::vector<u16> def_of_name;  // unused
    u32 defs_base = 0;
    std::map<u32, sim::MissionOrder*> live;

    static u32 tok(const sim::Unit& u) { return 0x40000000u | u.id; }
    static u32 tok(const sim::Unit* u) { return u ? tok(*u) : 0u; }
    u32 next_answer(int q) {
        const auto& v = queue[static_cast<std::size_t>(q)];
        std::size_t& at = queue_at[static_cast<std::size_t>(q)];
        return at < v.size() ? v[at++] : 0u;
    }
    void rec(std::initializer_list<u32> words) { log.insert(log.end(), words); }
    static u32 w(i32 v) { return static_cast<u32>(v); }
    // UnitWorld
    sim::Unit* unit_by_id(std::uint16_t id) override { return id < units->size() ? &(*units)[id] : nullptr; }
    const sim::HeightMap& heightmap() override {
        static sim::HeightMap hm;
        return hm;
    }
    bool yard_cells_free(sim::Unit&, i32) override { return false; }
    void claim_yard_cells(sim::Unit&) override {}
    void on_activate_flags_changed(sim::Unit&, std::uint8_t, std::uint8_t) override {}
    std::uint32_t random_mod(std::uint32_t n) override { return rng.next_mod(n); }
    void emit_sfx(sim::Unit&, i32, i32) override {}
    void explode_piece(sim::Unit&, i32, i32) override {}
    void attach_unit(sim::Unit&, i32, i32, i32) override {}
    void drop_unit(sim::Unit&, i32) override {}
    // CombatSystems (not reached)
    void retarget(sim::Unit*, sim::Unit&, std::uint16_t) override {}
    void add_paralysis(sim::Unit&, std::uint16_t) override {}
    bool pay_energy(sim::Unit&, float) override { return true; }
    void stats_activity(i32) override {}
    void network_send(i32, const std::uint8_t*, i32) override {}
    void reveal_dead_unit(sim::Unit&) override {}
    void victory_unit_destroyed(sim::Unit&) override {}
    void clear_orders(sim::Unit&) override {}
    void forget_speech(sim::Unit&) override {}
    void leave_group(sim::Unit&) override {}
    void remove_projectiles(sim::Unit&) override {}
    void detach_from_carrier(sim::Unit&) override {}
    void clear_footprint(sim::Unit&) override {}
    void forget_fog(sim::Unit&) override {}
    void kill_counted(std::uint8_t, sim::Unit&) override {}
    void detonate(sim::Unit&, bool) override {}
    void spawn_wreck(sim::Unit&, i32, bool) override {}
    void release_unit(sim::Unit&) override {}
    void release_motion(sim::Unit&) override {}
    void player_out_of_units(int) override {}
    void commander_died(int) override {}
    // OrderSystems (not reached)
    std::int32_t run_mission(sim::Unit&, sim::MissionOrder&, std::uint32_t&) override { return 2; }
    void clear_weapon_target(sim::Unit& u, int slot) override { sim::clear_weapon_target(u, slot); }
    void release_weapon_targets(sim::Unit& u) override { sim::release_weapon_targets(u, 3); }
    void release_attached(sim::Unit&, sim::MissionOrder& o) override { o.attached = 0; }
    void broadcast_script_call(sim::Unit&, std::int16_t) override {}
    sim::MissionOrder* allocate_order() override { return new sim::MissionOrder; }
    void free_order(sim::MissionOrder* o) override { delete o; }
    // MissionSystems (not reached)
    std::int32_t run_other(sim::Unit&, sim::MissionOrder&, std::uint32_t&) override { return 2; }
    void speech(sim::Unit&, std::uint32_t, sim::SpeechText) override {}
    void enemies_in_radius(int, const std::array<i32, 3>&, i32, std::vector<sim::Unit*>&) override {}
    sim::Unit* fire_at_will_target(sim::Unit&) override { return nullptr; }
    bool issue_attack_order(sim::Unit&, sim::Unit&, bool) override { return false; }
    std::uint8_t resolve_mission(sim::OrderIntent, sim::Unit&, sim::Unit*, const std::array<i32, 3>*) override { return 0; }
    void unfinished_decay(sim::Unit&, i32) override {}
    void display_changed(sim::Unit&) override {}
    bool use_resources_if_not_stalled(sim::Unit&, float, float) override { return true; }
    void build_counts_changed(sim::Unit&) override {}
    void nano_beam(const std::array<i32, 3>&, const std::array<i32, 6>&) override {}
    void drop_goal_object(sim::Unit&, sim::MissionOrder& o) override { o.attached = 0; }
    void attach_circle_goal(sim::Unit&, sim::MissionOrder&, const std::array<i32, 3>&, i32) override {}
    // TargetingSystems
    bool unit_visible(int player, sim::Unit& u) override {
        rec({static_cast<u32>(kVisibleId), 0x20000000u | static_cast<u32>(player), tok(u)});
        return next_answer(qVisible) != 0;
    }
    bool position_explored(int, const std::array<i32, 3>&) override { return false; }
    std::uint8_t feature_flags(std::uint16_t) override { return 0; }
    std::int32_t intercept_target(sim::Unit&, int, std::array<i32, 3>&) override { return -1; }
    void ai_own_unit(int table, sim::Unit& u) override { sim::ai_table_own_unit(*ai, table, u); }
    void ai_rescore(int table) override { sim::ai_rescore(*ai, table); }
    void ai_table_begin(int table) override { sim::ai_table_begin(*ai, table); }
    void ai_table_end(int table) override { sim::ai_table_end(*ai, table); }
    void under_attack(sim::Unit&) override {}
    // AiSystems
    std::uint8_t resolve(std::uint8_t intent, sim::Unit& u, sim::Unit* t, const Pos* pos) override {
        rec({static_cast<u32>(kResolveId), intent, tok(u), tok(t), pos ? 1u : 0u, pos ? w((*pos)[0]) : 0u, pos ? w((*pos)[1]) : 0u,
             pos ? w((*pos)[2]) : 0u});
        return static_cast<std::uint8_t>(next_answer(qResolve));
    }
    void enqueue(std::uint8_t kind, bool queued, sim::Unit& u, sim::Unit* t, const Pos* pos, i32 p0, i32 p1) override {
        rec({static_cast<u32>(kEnqueueId), kind, queued ? 1u : 0u, tok(u), tok(t), pos ? 1u : 0u, pos ? w((*pos)[0]) : 0u,
             pos ? w((*pos)[1]) : 0u, pos ? w((*pos)[2]) : 0u, w(p0), w(p1)});
    }
    void build_menu(sim::Unit& u, std::uint16_t type, bool) override {
        rec({static_cast<u32>(kMenuId), 0x50000000u | type, tok(u), 1u});
    }
    void set_activated(sim::Unit& u, bool on) override { rec({static_cast<u32>(kActivateId), tok(u), 1u, on ? 1u : 0u}); }
    bool can_place(std::uint16_t type, std::int16_t x, std::int16_t z) override {
        rec({static_cast<u32>(kPlaceId), 0x50000000u | type, 0u, static_cast<u16>(x) | static_cast<u32>(static_cast<u16>(z)) << 16, 1u});
        metal_sum = static_cast<i32>(next_answer(qMetal));
        return next_answer(qPlace) != 0;
    }
    bool structure_fits(std::uint16_t type, std::int16_t x, std::int16_t z) override {
        rec({static_cast<u32>(kFitsId), 0x50000000u | type, static_cast<u16>(x) | static_cast<u32>(static_cast<u16>(z)) << 16, 0u, 0u});
        metal_sum = static_cast<i32>(next_answer(qMetal));
        return next_answer(qFits) != 0;
    }
    std::int32_t last_place_metal() override { return metal_sum; }
    bool point_in_sight(int player, const Pos& pos) override {
        const std::size_t p = static_cast<std::size_t>(player);
        if (fog) {
            const u32 x = static_cast<u32>(static_cast<i32>(static_cast<std::int16_t>(static_cast<u32>(pos[0]) >> 16)) >> 5);
            const i32 yz = static_cast<std::int16_t>(static_cast<u32>(pos[2]) >> 16) - (static_cast<std::int16_t>(static_cast<u32>(pos[1]) >> 16) >> 1);
            const u32 z = static_cast<u32>(yz >> 5);
            if (x >= fog_w[p] || z >= fog_h[p]) return false;
            const std::size_t at = static_cast<std::size_t>(fog_w[p]) * z + x;
            return at < fog_counts[p].size() && fog_counts[p][at] != 0;
        }
        rec({static_cast<u32>(kExploredId), 0x20000000u | static_cast<u32>(player), w(pos[0]), w(pos[1]), w(pos[2])});
        return next_answer(qExplored) != 0;
    }
    bool weapon_in_range(sim::Unit& u, const Pos&, const Pos& t, int slot) override {
        rec({static_cast<u32>(kRangeId), tok(u), w(t[0]), w(t[1]), w(t[2]), static_cast<u32>(slot)});
        return next_answer(qRange) != 0;
    }
};

struct Stats {
    std::size_t scenarios = 0, ops = 0, mismatches = 0, failures = 0, faults = 0, confirmed = 0;
    bool restart = false;  // the scenario ended at a confirmed crash of the original
    std::map<std::string, std::size_t> by_op;
    std::size_t enqueues = 0, menus = 0, activations = 0, sites = 0, sites_found = 0, picks = 0, picks_made = 0, regroups = 0, refreshes = 0,
                rescores = 0, nearest = 0, profile = 0, air_base_searches = 0, air_bases_found = 0;
    std::map<int, std::size_t> calls, intents;  // stub calls by kind, resolutions by intent
};

bool run_scenario(harness::Remote& r, const Layout& L, ta::Vfs& vfs, const ta::UnitDefTable& defs, const std::vector<std::string>& profile_lines,
                  const std::vector<sim::AiTypeInfo>& ai_types, const std::vector<u16>& usable, int steps, unsigned seed, Stats& st) {
    std::mt19937 g(seed);
    r.w32(L.bump_ptr, L.scenario_heap);
    std::vector<std::uint8_t> zero(0x40000, 0);
    r.write(L.gs, zero.data(), 0x40000);
    r.write(L.units, zero.data(), (kMaxUnits + 2) * kUnitSize);
    for (int k = 0; k < 10; ++k) r.w32(kTablePtrs + static_cast<u32>(k) * 4, 0);
    r.w32(L.log_ptr, L.log_base);
    r.w32(kGate, 1);  // the profile gate's value at program start (the engine's AiState default)

    // Game state basics.
    const std::size_t ndefs = ai_types.size();
    r.w32(L.gs + gDefs, L.defs);
    r.w32(L.gs + gDefCount, static_cast<u32>(ndefs));
    r.w32(L.gs + gUnits, L.units);
    r.w32(L.gs + gUnitsEnd, L.units + static_cast<u32>(kMaxUnits + 1) * kUnitSize);
    r.w32(L.gs + gMapW, kMapW);
    r.w32(L.gs + gMapH, kMapH);
    r.w32(L.gs + gWorldW, kMapW * 16);
    r.w32(L.gs + gWorldH, kMapH * 16);
    r.w32(L.gs + gPixW, kMapW * 16 - 0x20);
    r.w32(L.gs + gPixH, kMapH * 16 - 0x80);
    r.w32(L.gs + gCells, L.cells);
    r.w32(L.gs + gFeatures, L.features);
    r.w32(L.gs + gMapInfo, L.mapinfo);

    sim::AiEnvironment env;
    env.map_width = kMapW;
    env.map_height = kMapH;
    env.difficulty = static_cast<i32>(g() % 3);
    r.w32(L.gs + gDifficulty, static_cast<u32>(env.difficulty));
    env.provider = g() % 3 == 0 ? 1 : 2;
    r.w32(L.mapinfo, static_cast<u32>(env.provider));
    env.surface_metal = static_cast<i32>(g() % 4 == 0 ? g() % 300 : g() % 40);
    r.w32(L.mapinfo + 0xD30, static_cast<u32>(env.surface_metal));
    env.units_per_player = static_cast<u16>(g() % 3 == 0 ? 2 + g() % 20 : 250);
    r.write(L.gs + gUnitsPerPlayer, &env.units_per_player, 2);
    env.wind_fraction = static_cast<float>(g() % 1000) / 1000.0f;
    std::memcpy(&zero[0], &env.wind_fraction, 4);
    r.write(L.gs + gWindFraction, &env.wind_fraction, 4);
    env.wind_max_speed = static_cast<i32>(g() % 4000);
    env.wind_denominator = 5000;
    r.w32(L.gs + gWindMax, static_cast<u32>(env.wind_max_speed));
    r.w32(L.gs + gWindDenom, static_cast<u32>(env.wind_denominator));
    env.tidal_strength = static_cast<float>(g() % 100) / 10.0f;
    r.write(L.gs + gTidal, &env.tidal_strength, 4);
    std::fill(zero.begin(), zero.end(), 0);

    // Map: cells with metal and features; metal-spot features.
    sim::MapGrid map;
    map.width = kMapW;
    map.height = kMapH;
    map.cells.assign(kMapW * kMapH, sim::MapCell{});
    std::vector<sim::FeatureTypeInfo> features(6);
    for (std::size_t f = 0; f < features.size(); ++f) {
        features[f].metal = g() % 3 ? static_cast<float>(g() % 50) : 0.0f;
        features[f].flags = static_cast<u16>(g() % 2 ? 0x200 : 0) | static_cast<u16>(g() & 0x1F);
        std::vector<std::uint8_t> fd(0x100, 0);
        std::memcpy(&fd[0xF0], &features[f].metal, 4);
        std::memcpy(&fd[0xFE], &features[f].flags, 2);
        r.write(L.features + static_cast<u32>(f) * 0x100, fd.data(), fd.size());
    }
    r.w32(L.gs + gFeatureCount, static_cast<u32>(features.size()));
    std::vector<std::uint8_t> cells(map.cells.size() * 13, 0);
    for (std::size_t c = 0; c < map.cells.size(); ++c) {
        sim::MapCell& mc = map.cells[c];
        mc.metal = static_cast<u8>(g() % 4 == 0 ? g() % 60 : 0);
        mc.feature = g() % 25 == 0 ? static_cast<u16>(g() % features.size()) : (g() % 40 == 0 ? 0xFFFE : 0xFFFF);
        cells[c * 13 + 7] = mc.metal;
        std::memcpy(&cells[c * 13 + 8], &mc.feature, 2);
    }
    r.write(L.cells, cells.data(), cells.size());

    // Players: 0..2 playing (at least one computer player), the rest empty.
    std::vector<sim::PlayerSlot> players(11);
    std::vector<sim::PlayerEconomy> economy(11);
    std::vector<std::array<std::uint8_t, 16>> allied(11);
    World world;
    world.fog = g() % 2 == 0;
    {
        const u8 sf = world.fog ? 2 : 0;
        r.write(L.gs + gSightFlags, &sf, 1);
    }
    world.fog_counts.resize(10);
    for (int p = 0; p < 10; ++p) {
        sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        const bool playing = p < kPlayers;
        s.active = playing;
        s.controller = static_cast<std::uint8_t>(playing ? (p < 2 ? 2 : 1 + g() % 2) : 0);
        s.state = static_cast<std::uint8_t>(p);
        if (playing) {
            s.first_unit = 1 + p * kPer;
            s.last_unit = s.first_unit + kPer - 1;
        }
        auto& al = allied[static_cast<std::size_t>(p)];
        for (int k = 0; k < kPlayers; ++k) al[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(k == p || g() % 6 == 0 ? 1 : 0);
        s.energy_stored = static_cast<float>(g() % 6 == 0 ? g() % 60 : 50 + g() % 3000);
        s.metal_stored = static_cast<float>(g() % 6 == 0 ? g() % 30 : 25 + g() % 2000);
        s.alive_count = static_cast<u16>(g() % 30);
        sim::PlayerEconomy& e = economy[static_cast<std::size_t>(p)];
        e.energy_income = static_cast<float>(g() % 400) / (g() % 2 ? 1.0f : 3.0f);
        e.energy_usage = static_cast<float>(g() % 400) / (g() % 2 ? 1.0f : 7.0f);
        e.metal_income = static_cast<float>(g() % 12) / (g() % 2 ? 1.0f : 3.0f);
        e.metal_usage = static_cast<float>(g() % 12) / (g() % 2 ? 1.0f : 3.0f);
        e.energy_storage = static_cast<float>(g() % 3000);
        e.metal_storage = static_cast<float>(g() % 2000);
        const u32 at = L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize;
        r.w32(at + pActive, s.active ? 1u : 0u);
        r.write(at + pController, &s.controller, 1);
        r.write(at + pState, &s.state, 1);
        r.write(at + pAllied, al.data(), 16);
        r.w32(at + pFirst, L.units + static_cast<u32>(playing ? s.first_unit : 1) * kUnitSize);
        r.w32(at + pLast, L.units + static_cast<u32>(playing ? s.last_unit : 0) * kUnitSize);
        r.write(at + pEnergy, &s.energy_stored, 4);
        r.write(at + pMetal, &s.metal_stored, 4);
        r.write(at + pEIncome, &e.energy_income, 4);
        r.write(at + pEUsage, &e.energy_usage, 4);
        r.write(at + pMIncome, &e.metal_income, 4);
        r.write(at + pMUsage, &e.metal_usage, 4);
        r.write(at + pEStorage, &e.energy_storage, 4);
        r.write(at + pMStorage, &e.metal_storage, 4);
        r.write(at + pAlive, &s.alive_count, 2);
        // Fog grid (blocks of 2 x 2 cells).
        world.fog_w[static_cast<std::size_t>(p)] = kMapW / 2;
        world.fog_h[static_cast<std::size_t>(p)] = kMapH / 2;
        auto& fc = world.fog_counts[static_cast<std::size_t>(p)];
        fc.resize((kMapW / 2) * (kMapH / 2));
        for (auto& b : fc) b = static_cast<u8>(g() % 3 == 0 ? 1 + g() % 3 : 0);
        const u32 fa = L.fog + static_cast<u32>(p) * 0x1000;
        r.write(fa, fc.data(), fc.size());
        r.w32(at + pFog, fa);
        r.w32(at + pFogW, kMapW / 2);
        r.w32(at + pFogH, kMapH / 2);
    }

    // Units.
    std::vector<sim::Unit> units(kMaxUnits + 2);
    std::vector<sim::UnitTickState> ticks(kMaxUnits + 2);
    std::vector<sim::UnitTickTypeInfo> tick_types(ndefs);
    for (std::size_t k = 0; k < ndefs; ++k) tick_types[k].type_flags = ai_types[k].type_flags;
    world.units = &units;
    auto unit_addr = [&](int i) { return L.units + static_cast<u32>(i) * kUnitSize; };
    auto def_addr = [&](std::size_t k) { return L.defs + static_cast<u32>(k) * kDefSize; };
    std::vector<int> present;
    std::vector<std::unique_ptr<sim::MissionOrder>> heads;
    const bool army = g() % 3 == 0;
    for (int i = 1; i <= kMaxUnits; ++i) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = ticks[static_cast<std::size_t>(i)];
        u.id = static_cast<u16>(i);
        t.owner = (i - 1) / kPer;
        const u32 at = unit_addr(i);
        r.w32(at + uOwner, L.gs + 0x1B63 + static_cast<u32>(t.owner) * kPlayerSize);
        const u8 os = static_cast<u8>(t.owner);
        r.write(at + uOwnerSlot, &os, 1);
        r.write(at + uId, &u.id, 2);
        r.w32(at + uDef, def_addr(0));
        t.tick_type = &tick_types[0];
        if (g() % 5 == 0) continue;
        present.push_back(i);
        // Builders (with build lists) more often, to reach the construction roles; in "army"
        // scenarios mostly armed mobile units, to fill the strike groups and the hunter.
        u16 type = usable[g() % usable.size()];
        if (army && g() % 4 != 0) {
            for (int tries = 0; tries < 40; ++tries) {
                const u16 c = usable[g() % usable.size()];
                if (!ai_types[c].structure && (ai_types[c].type_flags & 0x10000u) && (ai_types[c].abilities & 0x10u) &&
                    !(ai_types[c].type_flags & 0x840u)) {
                    type = c;
                    break;
                }
            }
        } else if (g() % 12 == 0) {  // air base builders (pads), for the air base search
            for (int tries = 0; tries < 200; ++tries) {
                const u16 c = usable[g() % usable.size()];
                if ((ai_types[c].type_flags & 0x240u) == 0x240u) {
                    type = c;
                    break;
                }
            }
        } else if (g() % 3 == 0)
            for (int tries = 0; tries < 20; ++tries) {
                const u16 c = usable[g() % usable.size()];
                if (!ai_types[c].build_options.empty()) {
                    type = c;
                    break;
                }
            }
        const sim::AiTypeInfo& ai = ai_types[type];
        t.type_index = type;
        t.tick_type = &tick_types[type];
        t.has_motion = !ai.structure;
        auto coord = [&](int cells_n) -> i32 { return static_cast<i32>((static_cast<u32>(16 + g() % ((cells_n - 2) * 16)) << 16) | (g() & 0xFFFF)); };
        u.pos = {coord(kMapW), static_cast<i32>((g() % 120) << 16), coord(kMapH)};
        u.flags = sim::unit_flags::kAlive | 0x20u | (g() % 10 == 0 ? 0x4000u : 0u) | (g() % 8 == 0 ? 2u : 1u) | (ai.structure ? 0x20000000u : 0u) |
                  ((ai.type_flags & 0x10000u) ? 0x80000000u : 0u) | (static_cast<u32>(g() % 16) << 18) | (g() % 10 == 0 ? 0x8000u : 0u);
        u.activate_flags = static_cast<u8>((g() % 2 ? 1u : 0u) | (g() % 8 == 0 ? 4u : 0u));
        u.build_fraction_left = g() % 6 == 0 ? 0.5f : 0.0f;
        if (g() % 3 == 0) {
            auto o = std::make_unique<sim::MissionOrder>();
            o->flags = (g() % 2 ? 8u : 0u) | (g() % 2 ? 0x4000u : 0u) | (static_cast<u32>(g()) & 0x10u);
            u.orders = o.get();
            const u32 oa = L.orders + static_cast<u32>(i) * 0x60;
            r.w32(oa + 0x42, o->flags);
            r.w32(at + uOrders, oa);
            heads.push_back(std::move(o));
        }
        r.w32(at + uMotion, t.has_motion ? L.out + 0x400 : 0u);
        r.write(at + uPosX, &u.pos[0], 4);
        r.write(at + uPosY, &u.pos[1], 4);
        r.write(at + uPosZ, &u.pos[2], 4);
        r.w32(at + uDef, def_addr(type));
        r.write(at + uDefIndex, &t.type_index, 2);
        r.write(at + uBuildLeft, &u.build_fraction_left, 4);
        r.write(at + uActivate, &u.activate_flags, 1);
        r.w32(at + uFlags, u.flags);
    }
    if (present.empty()) return true;

    // Engine contexts.
    sim::CombatEnvironment cenv;
    sim::UnitTypeInfo empty_type;
    sim::UnitTickTypeInfo empty_tick;
    cenv.empty_type = &empty_type;
    cenv.empty_tick_type = &empty_tick;
    sim::Combat combat{units, ticks, players, cenv, world};
    sim::OrderContext octx{ticks, players, world.rng, world};
    sim::MissionEnvironment menv;
    std::vector<sim::MissionTypeInfo> mission_types(ndefs);
    sim::Missions missions{octx, units, combat, world, world, mission_types, allied, menv};
    std::vector<sim::TargetingTypeInfo> ttypes(ndefs);
    sim::TargetingEnvironment tenv;
    sim::TargetingState tstate;
    sim::Targeting targeting{units, ticks, players, allied, ttypes, mission_types, map, static_cast<u32>(features.size()), missions, tenv, world, tstate};
    sim::AiState state;
    sim::Ai ai{units, ticks, players, economy, allied, ai_types, tstate, map, features, world.rng, env, world, state, {}};
    world.ai = &ai;
    const u32 rng0 = static_cast<u32>(g()) | 1u;
    r.w32(kBattleRngState, rng0);
    world.rng.set_state(rng0);

    auto set_tick = [&](u32 tick) {
        env.game_tick = tick;
        tenv.game_tick = tick;
        octx.game_tick = tick;
        r.w32(L.gs + gTick, tick);
    };
    set_tick(1000 + g() % 1000);

    // Answers for the stubs (same lists on both sides).
    auto deal = [&]() {
        for (int q = 0; q < kQueues; ++q) {
            std::vector<u32>& v = world.queue[static_cast<std::size_t>(q)];
            v.clear();
            world.queue_at[static_cast<std::size_t>(q)] = 0;
            const u32 bias = g() % 4;
            for (int k = 0; k < 600; ++k) {
                u32 a = 0;
                switch (q) {
                case qResolve: a = g() % 5 == 0 ? 0u : static_cast<u32>(g() % 68); break;
                case qMetal: a = g() % 3 == 0 ? 0u : static_cast<u32>(g() % (g() % 2 ? 400 : 4000)); break;
                default: a = g() % 4 < bias ? 0u : 1u; break;
                }
                v.push_back(a);
            }
            const u32 qa = L.queues + static_cast<u32>(q) * kQueueSize;
            std::vector<u32> img = {qa + 8, qa + 8 + static_cast<u32>(v.size()) * 4};
            img.insert(img.end(), v.begin(), v.end());
            r.write(qa, img.data(), img.size() * 4);
        }
    };
    auto fail = [&](const std::string& what) {
        std::printf("scenario %u: the original failed during %s\n", seed, what.c_str());
        return false;
    };
    auto engine_faulted = [&]() {
        std::string f = !ai.fault.empty() ? ai.fault : targeting.fault;
        if (f.empty()) return false;
        ++st.faults;
        std::printf("scenario %u: stopped, the original would fail here: %s\n", seed, f.c_str());
        return true;
    };

    // Player set-up: groups, controllers and tables by the original routines.
    deal();
    std::array<u32, 10> controller_addr{}, table_addr{};
    for (int p = 0; p < 10; ++p) {
        const sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        if (s.controller == 0) continue;
        const u32 pa = L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize;
        if (!r.call(kGroupsInit, 0, {pa})) return fail("groups init");
        sim::ai_create_player(ai, p);
        if (s.active && s.controller == 3) continue;
        u32 obj = 0;
        if (!r.call(0x004B4F10, 0, {0x3D}, &obj) || !obj) return fail("controller alloc");
        if (!r.call(kControllerCtor, obj, {pa})) return fail("controller ctor");
        r.w32(pa + pControllerObj, obj);
        controller_addr[static_cast<std::size_t>(p)] = obj;
        if (!r.call(kTableCreate, 0, {s.state})) return fail("table create");
        table_addr[s.state] = r.r32(kTablePtrs + static_cast<u32>(s.state) * 4);
        tstate.controllers[static_cast<std::size_t>(p)].exists = true;
        sim::UnitTable& tb = tstate.tables[s.state];
        tb.exists = true;
        tb.player = s.state;
        // The constructors' role through the planting stub.
        const u32 scout = r.r32(obj + cSlots + 4 * 4);
        r.w32(scout, L.scout_vtable);
    }
    if (engine_faulted()) return true;
    // Units join group 0, as at their creation.
    for (int i : present) {
        sim::set_unit_group(ai, units[static_cast<std::size_t>(i)], 0);
        if (!r.call(kSetGroup, 0, {unit_addr(i), 0})) return fail("join group");
    }
    // Metal spots.
    sim::ai_collect_metal_sites(ai);
    for (int p = 0; p < 10; ++p)
        if (controller_addr[static_cast<std::size_t>(p)] && !r.call(kMetalSites, 0, {static_cast<u32>(p)})) return fail("metal sites");
    ++st.scenarios;

    // --- Comparison -----------------------------------------------------------
    auto norm = [&](u32& w) {
        if (w >= L.units && w < L.units + (kMaxUnits + 2) * kUnitSize && (w - L.units) % kUnitSize == 0) w = 0x40000000u | ((w - L.units) / kUnitSize);
        else if (w >= L.gs + 0x1B63 && w < L.gs + 0x1B63 + 10 * kPlayerSize && (w - L.gs - 0x1B63) % kPlayerSize == 0)
            w = 0x20000000u | ((w - L.gs - 0x1B63) / kPlayerSize);
        else if (w >= L.defs && w < L.defs + static_cast<u32>(ndefs) * kDefSize && (w - L.defs) % kDefSize == 0)
            w = 0x50000000u | ((w - L.defs) / kDefSize);
        else if (w >= L.defs + 0x20 && w < L.defs + static_cast<u32>(ndefs) * kDefSize && (w - L.defs - 0x20) % kDefSize == 0)
            w = 0x50000000u | ((w - L.defs - 0x20) / kDefSize);
    };
    auto records = [&](std::vector<u32> in, bool remote) {
        std::vector<u32> out;
        for (std::size_t i = 0; i < in.size();) {
            const std::size_t n = record_words(in[i]);
            if (!n || i + n > in.size()) {
                out.push_back(0xDEADBEEFu);
                break;
            }
            std::vector<u32> rec(in.begin() + static_cast<std::ptrdiff_t>(i), in.begin() + static_cast<std::ptrdiff_t>(i + n));
            for (std::size_t k = 1; k < rec.size(); ++k) norm(rec[k]);
            // Engine-side placement records carry the type; the original's the definition pointer.
            (void)remote;
            out.insert(out.end(), rec.begin(), rec.end());
            i += n;
        }
        return out;
    };
    auto vec_words = [&](u32 begin, u32 end, std::size_t elem) {
        std::vector<std::uint8_t> v;
        if (begin && end >= begin && (end - begin) < 0x100000) {
            v.resize(end - begin);
            if (!v.empty()) r.read(begin, v.data(), v.size());
        }
        (void)elem;
        return v;
    };
    auto compare = [&](const std::string& what) -> bool {
        bool ok = true;
        auto bad = [&](const std::string& m) {
            if (ok) std::printf("scenario %u %s: %s\n", seed, what.c_str(), m.c_str());
            ok = false;
        };
        const u32 end = r.r32(L.log_ptr);
        std::vector<u32> rlog((end - L.log_base) / 4);
        if (!rlog.empty()) r.read(L.log_base, rlog.data(), rlog.size() * 4);
        r.w32(L.log_ptr, L.log_base);
        const std::vector<u32> theirs = records(rlog, true);
        const std::vector<u32> mine = records(world.log, false);
        world.log.clear();
        if (theirs != mine) {
            bad("stub call log differs");
            std::size_t first = 0;
            while (first < theirs.size() && first < mine.size() && theirs[first] == mine[first]) ++first;
            const std::size_t from = first > 12 ? first - 12 : 0;
            for (std::size_t i = from; i < first + 12 && i < (std::max)(theirs.size(), mine.size()); ++i) {
                const long long a = i < theirs.size() ? static_cast<long long>(theirs[i]) : -1;
                const long long b = i < mine.size() ? static_cast<long long>(mine[i]) : -1;
                std::printf("    log[%zu] orig %llx ours %llx%s\n", i, a, b, a != b ? "   <--" : "");
            }
        }
        for (std::size_t i = 0; i < mine.size();) {
            const std::size_t n = record_words(mine[i]);
            if (!n) break;
            const u32 id = mine[i];
            ++st.calls[static_cast<int>(id)];
            if (id == static_cast<u32>(kEnqueueId)) ++st.enqueues;
            if (id == static_cast<u32>(kResolveId)) ++st.intents[static_cast<int>(mine[i + 1])];
            i += n;
        }
        if (r.r32(kBattleRngState) != world.rng.state()) bad("battle RNG state");
        // Tables.
        for (int k = 0; k < 10; ++k) {
            const sim::AiTable& tb = state.tables[static_cast<std::size_t>(k)];
            const u32 ta = table_addr[static_cast<std::size_t>(k)];
            if (tb.exists != (ta != 0)) {
                bad("table " + std::to_string(k) + " presence");
                continue;
            }
            if (!ta) continue;
            const std::string tn = "table " + std::to_string(k) + " ";
            auto cmp_bytes = [&](u32 begin_off, const void* data, std::size_t bytes, const char* name) {
                const std::vector<std::uint8_t> v = vec_words(r.r32(ta + begin_off), r.r32(ta + begin_off + 4), 1);
                if (v.size() != bytes || (bytes && std::memcmp(v.data(), data, bytes) != 0)) {
                    std::size_t at = 0;
                    const auto* d8 = static_cast<const std::uint8_t*>(data);
                    while (at < v.size() && at < bytes && v[at] == d8[at]) ++at;
                    bad(tn + name + " (sizes " + std::to_string(v.size()) + "/" + std::to_string(bytes) + ", first byte difference at " +
                        std::to_string(at) + ")");
                }
            };
            cmp_bytes(tCounts, tb.counts.data(), tb.counts.size() * 2, "counts");
            cmp_bytes(tThreat, tb.threat.data(), tb.threat.size(), "threat");
            cmp_bytes(tScores, tb.build_scores.data(), tb.build_scores.size() * 3, "build scores");
            cmp_bytes(tAnchorW, tb.anchor_weight.data(), tb.anchor_weight.size(), "anchor weights");
            cmp_bytes(tWeight, tb.weight.data(), tb.weight.size(), "weights");
            cmp_bytes(tWeightFixed, tb.weight_fixed.data(), tb.weight_fixed.size() * 4, "weight fixed");
            cmp_bytes(tLimit, tb.limit.data(), tb.limit.size() * 4, "limits");
            cmp_bytes(tLimitFixed, tb.limit_fixed.data(), tb.limit_fixed.size() * 4, "limit fixed");
            std::vector<u32> air;
            for (const sim::Unit* u : tb.air_bases) air.push_back(unit_addr(u->id));
            cmp_bytes(tAirBegin, air.data(), air.size() * 4, "air bases");
            std::vector<std::uint8_t> sites;
            for (const sim::AiMetalSite& s : tb.metal_sites) {
                std::uint8_t b[8];
                std::memcpy(b, &s.cell_x, 2);
                std::memcpy(b + 2, &s.cell_z, 2);
                std::memcpy(b + 4, &s.score, 4);
                sites.insert(sites.end(), b, b + 8);
            }
            cmp_bytes(tSitesBegin, sites.data(), sites.size(), "metal sites");
            if (static_cast<i32>(r.r32(ta + tBuilders)) != tb.builders) bad(tn + "builders");
            for (int c = 0; c < 3; ++c)
                if (static_cast<i32>(r.r32(ta + tAnchor + 4u * static_cast<u32>(c))) != tb.anchor[static_cast<std::size_t>(c)]) bad(tn + "anchor");
            if (static_cast<i32>(r.r32(ta + tRadius)) != tb.search_radius) bad(tn + "search radius");
            std::array<std::int16_t, 4> lg{}, wg{};
            r.read(ta + tGrid, lg.data(), 8);
            r.read(ta + tGrid + 0xC, wg.data(), 8);
            if (lg != std::array<std::int16_t, 4>{tb.land_w, tb.land_h, tb.land_dx, tb.land_dz} ||
                wg != std::array<std::int16_t, 4>{tb.water_w, tb.water_h, tb.water_dx, tb.water_dz} ||
                static_cast<i32>(r.r32(ta + tGrid + 8)) != tb.land_margin || static_cast<i32>(r.r32(ta + tGrid + 0x14)) != tb.water_margin)
                bad(tn + "site grids");
        }
        // Controllers and roles.
        for (int p = 0; p < 10; ++p) {
            const u32 ca = controller_addr[static_cast<std::size_t>(p)];
            const sim::AiController& c = state.controllers[static_cast<std::size_t>(p)];
            if (c.exists != (ca != 0)) {
                bad("controller " + std::to_string(p) + " presence");
                continue;
            }
            if (!ca) continue;
            const std::string cn = "controller " + std::to_string(p) + " ";
            if (static_cast<i32>(r.r32(ca + cCountdown)) != c.regroup_countdown) bad(cn + "countdown");
            for (int k = 0; k < 10; ++k) {
                const u32 b = r.r32(ca + cSlots + 4u * static_cast<u32>(k));
                const sim::AiBehaviour& rb = c.roles[static_cast<std::size_t>(k)];
                if ((b != 0) != (rb.role != sim::AiRole::None)) {
                    bad(cn + "role " + std::to_string(k) + " presence");
                    continue;
                }
                if (!b) continue;
                const std::string bn = cn + "role " + std::to_string(k) + " ";
                if (r.r32(b + bNext) != rb.next_tick) bad(bn + "timer");
                if (rb.role == sim::AiRole::Strike) {
                    if (static_cast<i32>(r.r32(b + bMin)) != rb.min_size || static_cast<i32>(r.r32(b + bMax)) != rb.max_size ||
                        static_cast<i32>(r.r32(b + bDist)) != rb.peer_distance || static_cast<i32>(r.r32(b + bPeer)) != rb.peer ||
                        (r.r32(b + bSeek) != 0) != rb.attacking)
                        bad(bn + "strike fields");
                } else if (rb.role == sim::AiRole::Reserve) {
                    if (static_cast<i32>(r.r32(b + bFollow)) != rb.follows) bad(bn + "follows");
                } else if (rb.role == sim::AiRole::Hunter) {
                    for (int q = 0; q < 3; ++q)
                        if (static_cast<i32>(r.r32(b + hBest + 4u * static_cast<u32>(q))) != rb.best[static_cast<std::size_t>(q)] ||
                            static_cast<i32>(r.r32(b + hProbe + 4u * static_cast<u32>(q))) != rb.probe[static_cast<std::size_t>(q)] ||
                            static_cast<i32>(r.r32(b + hStep + 4u * static_cast<u32>(q))) != rb.step[static_cast<std::size_t>(q)])
                            bad(bn + "hunter positions");
                    if (static_cast<i32>(r.r32(b + hThreat)) != rb.best_threat) bad(bn + "hunter threat");
                }
            }
        }
        // Groups.
        for (int p = 0; p < 10; ++p) {
            if (!players[static_cast<std::size_t>(p)].controller) continue;
            const u32 ga = r.r32(L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize + pGroups);
            for (int k = 0; k < 10; ++k) {
                std::vector<u32> mine_ids;
                for (const sim::Unit* u : state.groups[static_cast<std::size_t>(p)][static_cast<std::size_t>(k)]) mine_ids.push_back(unit_addr(u->id));
                const u32 gb = ga + 0x20u * static_cast<u32>(k);
                const std::vector<std::uint8_t> v = vec_words(r.r32(gb + 0x14), r.r32(gb + 0x18), 4);
                if (v.size() != mine_ids.size() * 4 || (!v.empty() && std::memcmp(v.data(), mine_ids.data(), v.size()) != 0))
                    bad("player " + std::to_string(p) + " group " + std::to_string(k));
            }
        }
        for (int i : present) {
            const sim::Unit& u = units[static_cast<std::size_t>(i)];
            const u32 at = unit_addr(i);
            if (r.r32(at + uFlags) != u.flags) bad("unit " + std::to_string(i) + " flags");
            if (r.r32(at + uGroup) != u.group) bad("unit " + std::to_string(i) + " group");
        }
        if (!ok) ++st.mismatches;
        return ok;
    };
    // Groups filled at once for most players (else by the controller's countdown), which starts
    // anywhere.
    for (int p = 0; p < 10; ++p) {
        const u32 ca = controller_addr[static_cast<std::size_t>(p)];
        if (!ca) continue;
        const i32 cd = 1 + static_cast<i32>(g() % 30);
        state.controllers[static_cast<std::size_t>(p)].regroup_countdown = cd;
        r.w32(ca + cCountdown, static_cast<u32>(cd));
        if (g() % 4 == 0) continue;
        sim::ai_assign_groups(ai, p);
        if (!r.call(kAssign, ca, {})) return fail("assign");
    }
    if (!compare("after setup")) return true;

    auto any_unit = [&]() { return present[g() % present.size()]; };
    auto ai_player = [&]() {
        for (int t = 0; t < 8; ++t) {
            const int p = static_cast<int>(g() % kPlayers);
            if (controller_addr[static_cast<std::size_t>(p)]) return p;
        }
        return 0;
    };
    auto sync_unit = [&](int i) {
        const sim::Unit& u = units[static_cast<std::size_t>(i)];
        const u32 at = unit_addr(i);
        r.w32(at + uFlags, u.flags);
        r.write(at + uActivate, &u.activate_flags, 1);
        r.write(at + uBuildLeft, &u.build_fraction_left, 4);
        r.write(at + uPosX, &u.pos[0], 4);
        r.write(at + uPosY, &u.pos[1], 4);
        r.write(at + uPosZ, &u.pos[2], 4);
    };
    auto sync_player = [&](int p) {
        const sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        const sim::PlayerEconomy& e = economy[static_cast<std::size_t>(p)];
        const u32 at = L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize;
        r.write(at + pEnergy, &s.energy_stored, 4);
        r.write(at + pMetal, &s.metal_stored, 4);
        r.write(at + pEIncome, &e.energy_income, 4);
        r.write(at + pEUsage, &e.energy_usage, 4);
        r.write(at + pMIncome, &e.metal_income, 4);
        r.write(at + pEStorage, &e.energy_storage, 4);
        r.write(at + pAlive, &s.alive_count, 2);
    };
    auto plant = [&]() {
        env.stale_site_height = static_cast<i32>(g() % 3 == 0 ? g() : (g() % 200) << 16);
        r.w32(L.plant, static_cast<u32>(env.stale_site_height));
    };
    // A token line (argv pointers at +0, words at +0x50, count at +0xD0) for the profile commands.
    auto token_line = [&](const std::vector<std::string>& words) {
        std::vector<std::uint8_t> img(0xD4, 0);
        u32 at = 0x50;
        for (std::size_t k = 0; k < words.size() && k < 20; ++k) {
            const u32 p = L.line + at;
            std::memcpy(&img[k * 4], &p, 4);
            std::memcpy(&img[at], words[k].c_str(), words[k].size() + 1);
            at += static_cast<u32>(words[k].size() + 1);
        }
        const u32 n = static_cast<u32>(words.size());
        std::memcpy(&img[0xD0], &n, 4);
        r.write(L.line, img.data(), img.size());
        return L.line;
    };

    for (int step = 0; step < steps; ++step) {
        set_tick(env.game_tick + 1 + (g() % 4 == 0 ? g() % 400 : 0));
        deal();
        plant();
        // Mix: controller step x6, a role directly x3, regroup, refresh x2, rescore, pick, score,
        // site, nearest enemy, group move, profile x2, tweaks x2.
        static constexpr int kMix[] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 3, 3, 4, 5, 6, 7, 8, 9, 10, 10, 11, 11, 12, 13, 14};
        int op = kMix[g() % (sizeof kMix / sizeof kMix[0])];
        if (const char* only = std::getenv("AI_HARNESS_ONLY_OP")) op = std::atoi(only);
        std::string what;
        // A crash the engine predicts is checked against the original: its call must fail.
        auto run_checked = [&](u32 fn, u32 self, const char* name) -> int {
            const bool crash = ai.fault.find("original crash") != std::string::npos;
            if (!ai.fault.empty() && !crash) {
                engine_faulted();
                return 0;
            }
            const bool ran = r.call(fn, self, {});
            if (crash) {
                if (ran) {
                    std::printf("scenario %u step %d: the engine predicts a crash in %s, the original ran on (%s)\n", seed, step, name, ai.fault.c_str());
                    ++st.mismatches;
                    return 0;
                }
                ++st.confirmed;
                st.restart = true;
                std::printf("scenario %u step %d: predicted crash confirmed in %s (%s)\n", seed, step, name, ai.fault.c_str());
                return -1;
            }
            if (!ran) {
                fail(name);
                return -1;
            }
            return 1;
        };
        if (op == 0) {  // controller step
            const int p = ai_player();
            sim::ai_controller_step(ai, p);
            const int rc = run_checked(kController, controller_addr[static_cast<std::size_t>(p)], "the controller step");
            if (rc <= 0) return rc == 0;
            what = "controller step";
        } else if (op == 1) {  // one role's action
            const int p = ai_player();
            const int k = 1 + static_cast<int>(g() % 9);
            sim::ai_run_role(ai, p, k);
            const u32 b = r.r32(controller_addr[static_cast<std::size_t>(p)] + cSlots + 4u * static_cast<u32>(k));
            const u32 fn = r.r32(r.r32(b));
            const int rc = run_checked(fn, b, "a role");
            if (rc <= 0) return rc == 0;
            what = "role " + std::to_string(k);
        } else if (op == 2) {  // group assignment
            const int p = ai_player();
            sim::ai_assign_groups(ai, p);
            if (engine_faulted()) return true;
            if (!r.call(kAssign, controller_addr[static_cast<std::size_t>(p)], {})) return fail("assign");
            ++st.regroups;
            what = "assign groups";
        } else if (op == 3) {  // table refresh (due)
            const int p = ai_player();
            sim::UnitTable& tb = tstate.tables[static_cast<std::size_t>(p)];
            tb.refresh_tick = env.game_tick - 30;
            r.w32(table_addr[static_cast<std::size_t>(p)] + tRefresh, tb.refresh_tick);
            sim::refresh_unit_table(targeting, p);
            if (engine_faulted()) return true;
            if (!r.call(kRefresh, 0, {static_cast<u32>(p)})) return fail("refresh");
            ++st.refreshes;
            what = "refresh";
        } else if (op == 4) {
            const int p = ai_player();
            sim::ai_rescore(ai, p);
            if (engine_faulted()) return true;
            if (!r.call(kRescore, table_addr[static_cast<std::size_t>(p)], {})) return fail("rescore");
            ++st.rescores;
            what = "rescore";
        } else if (op == 5) {  // build pick
            const int p = ai_player();
            const int i = any_unit();
            const u16 mine = sim::ai_pick_build_type(ai, p, units[static_cast<std::size_t>(i)]);
            if (engine_faulted()) return true;
            u32 res = 0;
            if (!r.call(kPick, 0, {static_cast<u32>(p), unit_addr(i)}, &res)) return fail("pick");
            if (static_cast<u16>(res) != mine) {
                std::printf("scenario %u step %d: pick differs (orig %u ours %u)\n", seed, step, res & 0xFFFF, mine);
                ++st.mismatches;
                return true;
            }
            ++st.picks;
            st.picks_made += mine != 0;
            what = "pick";
        } else if (op == 6) {  // build score
            const int p = ai_player();
            const u16 type = usable[g() % usable.size()];
            const i32 mine = sim::ai_build_score(ai, p, type);
            if (engine_faulted()) return true;
            u32 res = 0;
            if (!r.call(kScore, 0, {static_cast<u32>(p), type}, &res)) return fail("score");
            if (static_cast<i32>(res) != mine) {
                std::printf("scenario %u step %d: score of %u differs (orig %d ours %d)\n", seed, step, type, static_cast<i32>(res), mine);
                ++st.mismatches;
                return true;
            }
            what = "score";
        } else if (op == 7) {  // site search
            const int p = ai_player();
            std::vector<u16> candidates;
            for (u16 t : usable)
                if (ai_types[t].extracts_metal > 0.0f || g() % 8 == 0) candidates.push_back(t);
            const u16 type = candidates.empty() ? usable[0] : candidates[g() % candidates.size()];
            const int i = any_unit();
            Pos out{0x11111111, env.stale_site_height, 0x22222222};
            const bool found = sim::ai_find_site(ai, p, units[static_cast<std::size_t>(i)].pos, type, out);
            if (engine_faulted()) return true;
            const std::array<u32, 3> init = {0x11111111u, static_cast<u32>(env.stale_site_height), 0x22222222u};
            r.write(L.out, init.data(), 12);
            r.write(L.out + 0x20, &units[static_cast<std::size_t>(i)].pos, 12);
            u32 res = 0;
            if (!r.call(kSeek, 0, {static_cast<u32>(p), L.out + 0x20, def_addr(type), L.out}, &res)) return fail("site");
            std::array<i32, 3> theirs{};
            r.read(L.out, theirs.data(), 12);
            if ((res & 0xFF) != (found ? 1u : 0u) || theirs != out) {
                std::printf("scenario %u step %d: site differs (orig %u %d,%d ours %d %d,%d)\n", seed, step, res & 0xFF, theirs[0], theirs[2], found,
                            out[0], out[2]);
                ++st.mismatches;
                return true;
            }
            ++st.sites;
            st.sites_found += found;
            what = "site";
        } else if (op == 8) {  // nearest enemy
            const int p = ai_player();
            const Pos at = units[static_cast<std::size_t>(any_unit())].pos;
            sim::Unit* mine = sim::ai_nearest_enemy(ai, p, at[0], at[2]);
            u32 res = 0;
            if (!r.call(kNearest, controller_addr[static_cast<std::size_t>(p)], {static_cast<u32>(at[0]), static_cast<u32>(at[1]), static_cast<u32>(at[2])},
                        &res))
                return fail("nearest");
            if (res != (mine ? unit_addr(mine->id) : 0u)) {
                std::printf("scenario %u step %d: nearest enemy differs\n", seed, step);
                ++st.mismatches;
                return true;
            }
            ++st.nearest;
            what = "nearest enemy";
        } else if (op == 9) {  // a unit changes group
            const int i = any_unit();
            static constexpr i32 kGroups[] = {9, 9, 9, 2, 3, 6, 7, 8, 4, 1, 5, 0, -1};
            const i32 grp = kGroups[g() % (sizeof kGroups / sizeof kGroups[0])];
            sim::set_unit_group(ai, units[static_cast<std::size_t>(i)], grp);
            if (engine_faulted()) return true;
            if (!r.call(kSetGroup, 0, {unit_addr(i), static_cast<u32>(grp)})) return fail("set group");
            what = "set group";
        } else if (op == 10) {  // profile command
            const int kind = static_cast<int>(g() % 5);
            static const char* kWords[] = {"any", "easy", "medium", "hard", "EASY", "Hard", "x", "anything"};
            if (kind == 0) {
                std::vector<std::string> w = {"plan"};
                for (int n = static_cast<int>(g() % 4); n > 0; --n) w.push_back(kWords[g() % 8]);
                sim::ai_profile_plan(ai, w);
                if (!r.call(kPlan, 0, {token_line(w)})) return fail("plan");
                what = "plan";
            } else if (kind == 1 || kind == 2) {
                const u16 t = usable[g() % usable.size()];
                std::string name = defs.units[t].unit_name;
                if (g() % 2) for (char& c : name) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
                char num[32];
                if (kind == 1) std::snprintf(num, sizeof num, "%s%d.%d", g() % 4 == 0 ? "-" : "", static_cast<int>(g() % 4), static_cast<int>(g() % 100));
                else std::snprintf(num, sizeof num, "%d", static_cast<int>(g() % 30) - 3);
                std::vector<std::string> w = {kind == 1 ? "weight" : "limit", name, num};
                if (g() % 8 == 0) w.pop_back();
                sim::CategoryMask m{};
                m[(t >> 5) & 15] |= 1u << (t & 31);
                if (kind == 1) sim::ai_profile_weight(ai, m, w.size() > 2 ? static_cast<float>(ta::crt_atof(w[2])) : 0.0f, true);
                else sim::ai_profile_limit(ai, m, w.size() > 2 ? std::atoi(w[2].c_str()) : 0, true);
                if (!r.call(kind == 1 ? kWeight : kLimit, 0, {token_line(w)})) return fail("weight/limit");
                what = kind == 1 ? "weight line" : "limit line";
            } else {
                // A category-style mask straight to the table routines.
                sim::CategoryMask m{};
                for (int n = 0; n < 6; ++n) {
                    const u16 t = usable[g() % usable.size()];
                    m[(t >> 5) & 15] |= 1u << (t & 31);
                }
                r.write(L.out + 0x100, m.data(), 64);
                const bool fixed = g() % 4 == 0;
                const int side = ai_player();
                if (kind == 3) {
                    const float f = static_cast<float>(g() % 300) / 100.0f;
                    // One table only, as the routine takes it (the command loops the players).
                    sim::AiState saved = state;
                    for (int s = 0; s < 10; ++s)
                        if (s != side) state.controllers[static_cast<std::size_t>(s)].exists = false;
                    const bool gate = state.profile_gate;
                    state.profile_gate = true;
                    sim::ai_profile_weight(ai, m, f, fixed);
                    state.profile_gate = gate;
                    for (int s = 0; s < 10; ++s) state.controllers[static_cast<std::size_t>(s)].exists = saved.controllers[static_cast<std::size_t>(s)].exists;
                    u32 fb;
                    std::memcpy(&fb, &f, 4);
                    if (!r.call(kWeightMask, 0, {static_cast<u32>(side), L.out + 0x100, fb, fixed ? 1u : 0u})) return fail("weight mask");
                    what = "weight mask";
                } else {
                    const i32 lim = static_cast<i32>(g() % 20) - 2;
                    sim::AiState saved = state;
                    std::vector<sim::PlayerSlot> saved_players = players;
                    for (int s = 0; s < 10; ++s)
                        if (s != side) players[static_cast<std::size_t>(s)].controller = 0;
                    players[static_cast<std::size_t>(side)].controller = 2;
                    players[static_cast<std::size_t>(side)].active = true;
                    const bool gate = state.profile_gate;
                    state.profile_gate = true;
                    sim::ai_profile_limit(ai, m, lim, fixed);
                    state.profile_gate = gate;
                    players = saved_players;
                    if (!r.call(kLimitMask, 0, {static_cast<u32>(side), L.out + 0x100, static_cast<u32>(lim), fixed ? 1u : 0u})) return fail("limit mask");
                    what = "limit mask";
                }
            }
            if (engine_faulted()) return true;
            const bool rg = r.r32(kGate) != 0;
            if (rg != state.profile_gate) {
                std::printf("scenario %u step %d: profile gate differs after %s (orig %d ours %d)\n", seed, step, what.c_str(), rg,
                            state.profile_gate);
                ++st.mismatches;
                return true;
            }
            ++st.profile;
        } else if (op == 11) {  // tweaks: a unit, a player's resources, a hold tick
            const int i = any_unit();
            sim::Unit& u = units[static_cast<std::size_t>(i)];
            if (g() % 2) u.pos = {static_cast<i32>((16 + g() % (kMapW * 16 - 32)) << 16), static_cast<i32>((g() % 120) << 16),
                                  static_cast<i32>((16 + g() % (kMapH * 16 - 32)) << 16)};
            if (g() % 4 == 0) u.flags ^= 0x4000u;
            if (g() % 4 == 0) u.activate_flags ^= 1u;
            if (g() % 6 == 0) u.build_fraction_left = u.build_fraction_left == 0.0f ? 0.25f : 0.0f;
            sync_unit(i);
            const int p = static_cast<int>(g() % kPlayers);
            sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
            sim::PlayerEconomy& e = economy[static_cast<std::size_t>(p)];
            s.energy_stored = static_cast<float>(g() % 3000);
            s.metal_stored = static_cast<float>(g() % 2000);
            e.energy_income = static_cast<float>(g() % 400);
            e.metal_income = static_cast<float>(g() % 9);
            s.alive_count = static_cast<u16>(g() % 300);
            sync_player(p);
            if (g() % 3 == 0) {
                const int c = ai_player();
                tstate.controllers[static_cast<std::size_t>(c)].ai_hold_tick = env.game_tick + (g() % 2 ? 50u : 0u) - 25u;
                r.w32(controller_addr[static_cast<std::size_t>(c)] + cHold, tstate.controllers[static_cast<std::size_t>(c)].ai_hold_tick);
            }
            what = "tweak";
        } else if (op == 12) {  // the economy switch of a metal maker
            const int i = any_unit();
            sim::ai_switch_by_economy(ai, units[static_cast<std::size_t>(i)]);
            if (!r.call(kSwitch, 0, {unit_addr(i)})) return fail("switch");
            what = "economy switch";
        } else if (op == 14) {  // the table's activated air bases within a radius (0x40B530)
            const int p = ai_player();
            const Pos at = units[static_cast<std::size_t>(any_unit())].pos;
            const i32 radius = g() % 3 == 0 ? 0xF00 : static_cast<i32>(g() % 0x1800);
            std::vector<sim::Unit*> mine;
            const bool ok = sim::air_bases_in_radius(tstate.tables[static_cast<std::size_t>(p)], ticks, at, radius, mine);
            const u32 vec = L.out + 0x60;
            const std::array<u32, 4> empty{};
            r.write(vec, empty.data(), 16);
            r.write(L.out + 0x80, &at, 12);
            if (!r.call(kAirBases, 0, {static_cast<u32>(p), L.out + 0x80, static_cast<u32>(radius), vec})) return fail("air bases");
            const u32 begin = r.r32(vec + 4), end = r.r32(vec + 8);
            std::vector<u32> theirs;
            for (u32 a = begin; a && a < end; a += 4) theirs.push_back(r.r32(a));
            std::vector<u32> ours;
            for (sim::Unit* u : mine) ours.push_back(unit_addr(u->id));
            if (!ok || theirs != ours) {
                std::printf("scenario %u step %d: air bases differ (orig %zu ours %zu)\n", seed, step, theirs.size(), ours.size());
                ++st.mismatches;
                return true;
            }
            ++st.air_base_searches;
            st.air_bases_found += ours.size();
            what = "air bases";
        } else {  // profile text through the command dispatcher (0x4B7A30)
            std::string text;
            if (!profile_lines.empty()) {
                const std::size_t from = g() % profile_lines.size();
                const std::size_t n = 3 + g() % 20;
                for (std::size_t k = from; k < from + n && k < profile_lines.size(); ++k) text += profile_lines[k] + "\n";
            }
            static const char* kExtra[] = {"plan any", "plan easy medium", "plan hard", "plan", "// comment weight ARMCOM 0", "# weight ARMCOM 0",
                                           "weight", "limit", "  \tWEIGHT\tconstr  2.5  ", "Weight ALL 0.5", "limit NOTAIR 4", "weight nosuchthing 3",
                                           "weight armcom 1.25 # trailing", "\r", ""};
            for (int n = static_cast<int>(g() % 4); n > 0; --n) {
                if (g() % 2) {
                    const u16 t = usable[g() % usable.size()];
                    text += (g() % 2 ? "weight " : "limit ") + defs.units[t].unit_name + " " + std::to_string(g() % 5) + (g() % 2 ? "\r\n" : "\n");
                } else {
                    text += std::string(kExtra[g() % (sizeof kExtra / sizeof kExtra[0])]) + "\n";
                }
            }
            if (g() % 3 == 0 && !text.empty()) text.pop_back();  // no final line break
            ta::AiProfileReport rep;
            ta::run_ai_commands(text, vfs, defs, ai, rep);
            if (engine_faulted()) return true;
            r.write(L.text, text.data(), text.size());
            std::vector<std::uint8_t> parent(0xD4, 0);
            r.write(L.line + 0x800, parent.data(), parent.size());
            if (std::getenv("AI_HARNESS_SHOW_TEXT")) std::printf("--- text ---\n%s\n---\n", text.c_str());
            if (!r.call(0x004B7A30, 0, {L.text, static_cast<u32>(text.size()), L.line + 0x800, 0xFFFFFFFFu})) return fail("profile text");
            const bool rg = r.r32(kGate) != 0;
            if (rg != state.profile_gate) {
                std::printf("scenario %u step %d: profile gate differs after text\n", seed, step);
                ++st.mismatches;
                return true;
            }
            st.profile += rep.lines;
            what = "profile text";
            if (std::getenv("AI_HARNESS_SHOW_TEXT")) std::printf("--- text ---\n%s\n---\n", text.c_str());
        }
        ++st.ops;
        ++st.by_op[what];
        if (!compare(what + " step " + std::to_string(step))) return true;
    }
    return true;
}

// The remote definition table: the fields the computer player and the table refresh read.
void write_defs(harness::Remote& r, const Layout& L, const ta::UnitDefTable& defs, const ta::WeaponDefTable& wdefs) {
    for (std::size_t k = 0; k < wdefs.slots.size(); ++k) {
        std::vector<std::uint8_t> w(kWeaponSize, 0);
        const ta::WeaponDefRecord& d = wdefs.slots[k];
        std::memcpy(&w[0xDC], &d.range, 4);
        const u16 dmg = static_cast<u16>(d.default_damage);
        std::memcpy(&w[0xD4], &dmg, 2);
        w[0x10A] = d.index;
        r.write(L.weapons + static_cast<u32>(k) * kWeaponSize, w.data(), w.size());
    }
    u32 opt_at = L.options;
    for (std::size_t k = 0; k < defs.units.size(); ++k) {
        const ta::UnitDefRecord& d = defs.units[k];
        std::vector<std::uint8_t> b(kDefSize, 0);
        auto put = [&](u32 off, const void* p, std::size_t n) { std::memcpy(&b[off], p, n); };
        put(0x20, d.unit_name.c_str(), (std::min)(d.unit_name.size() + 1, std::size_t{32}));
        put(0xA0, d.side.c_str(), (std::min)(d.side.size() + 1, std::size_t{30}));
        put(0x14A, &d.footprint_x, 2);
        put(0x14C, &d.footprint_z, 2);
        const i32 n = d.build_options_count > 30 ? 30 : d.build_options_count;
        put(0x152, &n, 4);
        if (d.has_build_options) {
            put(0x156, &opt_at, 4);
            r.write(opt_at, d.build_options.data(), 60);
            opt_at += 64;
        }
        put(0x186, &d.build_cost_energy, 4);
        put(0x18A, &d.build_cost_metal, 4);
        put(0x1C0, &d.min_water_depth, 2);
        put(0x1C2, &d.energy_make, 4);
        put(0x1C6, &d.energy_use, 4);
        put(0x1CE, &d.extracts_metal, 4);
        put(0x1D2, &d.wind_generator, 4);
        put(0x1D6, &d.tidal_generator, 4);
        for (int s = 0; s < 3; ++s) {
            const int slot = s == 0 ? d.weapon1 : s == 1 ? d.weapon2 : d.weapon3;
            const u32 wa = L.weapons + static_cast<u32>(slot < 0 ? 0 : slot) * kWeaponSize;
            put(0x1EE + 4u * static_cast<u32>(s), &wa, 4);
        }
        put(0x204, &d.radar_distance, 2);
        put(0x206, &d.sonar_distance, 2);
        put(0x21E, &d.index, 2);
        put(0x22D, &d.makes_metal, 1);
        put(0x22F, &d.bm_code, 1);
        put(0x241, &d.type_flags, 4);
        put(0x245, &d.can_do_flags, 4);
        r.write(L.defs + static_cast<u32>(k) * kDefSize, b.data(), b.size());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ai_harness <TotalA.exe> <game_dir> [scenarios] [steps] [first scenario]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int scenarios = argc > 3 ? std::atoi(argv[3]) : 20;
    const int steps = argc > 4 ? std::atoi(argv[4]) : 60;
    const int first = argc > 5 ? std::atoi(argv[5]) : 0;
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[2]);
    ta::UnitDefContext ctx;
    ta::load_unit_context(vfs, ctx);
    const ta::UnitDefTable defs = ta::load_unit_defs(vfs, ctx);
    ta::WeaponDefOptions wopt;
    const ta::WeaponDefTable wdefs = ta::load_weapon_defs(vfs, wopt);
    const std::vector<sim::WeaponType> wtypes = ta::to_weapon_types(wdefs);
    std::vector<sim::AiTypeInfo> ai_types(defs.units.size());
    std::vector<u16> usable;
    for (std::size_t k = 0; k < defs.units.size(); ++k) {
        ta::to_ai_type(defs.units[k], wtypes, ai_types[k]);
        // (a missing weapon would crash the original's rescoring; the harness avoids it)
        for (auto& w : ai_types[k].weapons)
            if (!w) w = &wtypes[0];
        if (k > 0) usable.push_back(static_cast<u16>(k));
    }
    std::printf("%zu unit definitions\n", defs.units.size());
    harness::Remote r;
    Layout L;
    std::string report;
    if (!r.start(argv[1]) || !setup(r, L, report)) {
        std::fprintf(stderr, "cannot start the original (%s)\n", report.c_str());
        return 1;
    }
    write_defs(r, L, defs, wdefs);
    if (!register_commands(r, L, defs, report)) {
        std::fprintf(stderr, "%s\n", report.c_str());
        return 1;
    }
    // Lines of the stock profile, for the text operations.
    std::vector<std::string> profile_lines;
    {
        std::vector<std::uint8_t> f;
        if (vfs.read("ai\\default.txt", f)) {
            std::string cur;
            for (std::uint8_t c : f) {
                if (c == '\n') {
                    profile_lines.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(static_cast<char>(c));
                }
            }
        }
    }
    if (std::getenv("AI_HARNESS_PROBE")) {
        const char* probes[] = {"plan any\n", "plan any", "limit ARMCOM 3", "Limit ARMCOM 3\r\n", "\n\n", "// x\n", "weight ARMCOM 2",
                                "Limit ARMGUARD 18\r\n\r\nLimit ARMHAM 10\r\n"};
        for (const char* p : probes) {
            r.w32(L.gs + gDefs, L.defs);
            r.w32(L.gs + gDefCount, static_cast<u32>(defs.units.size()));
            const std::string t = p;
            r.write(L.text, t.data(), t.size() + 1);
            std::vector<std::uint8_t> parent(0xD4, 0);
            r.write(L.line + 0x800, parent.data(), parent.size());
            const bool ok = r.call(0x004B7A30, 0, {L.text, static_cast<u32>(t.size()), L.line + 0x800, 0xFFFFFFFFu});
            std::printf("probe [%s]: %s\n", p, ok ? "ok" : "FAILED");
            if (!ok) {
                r.stop();
                if (!r.start(argv[1]) || !setup(r, L, report)) return 1;
                write_defs(r, L, defs, wdefs);
                if (!register_commands(r, L, defs, report)) return 1;
            }
        }
    }
    Stats st;
    // Categories made during a scenario (unknown names) live in its heap: a new process then.
    auto categories = [&]() { return r.r32(0x0051E6B8) - r.r32(0x0051E6B4); };
    u32 registered = categories();
    for (int s = first; s < first + scenarios; ++s) {
        st.restart = false;
        const bool ok = run_scenario(r, L, vfs, defs, profile_lines, ai_types, usable, steps, static_cast<unsigned>(s * 7 + 13), st);
        if (!ok || categories() != registered) {
            if (!ok && !st.restart) {
                ++st.failures;
                std::printf("scenario %d: original call failed - restarting\n", s);
            }
            r.stop();
            if (!r.start(argv[1]) || !setup(r, L, report)) return 1;
            write_defs(r, L, defs, wdefs);
            if (!register_commands(r, L, defs, report)) return 1;
            registered = categories();
        }
    }
    r.stop();
    std::printf("scenarios %zu, operations %zu\n", st.scenarios, st.ops);
    for (const auto& [k, v] : st.by_op) std::printf("  %s: %zu\n", k.c_str(), v);
    std::printf("orders enqueued %zu, regroups %zu, refreshes %zu, rescores %zu, picks %zu (%zu chose a type), sites %zu (%zu found), "
                "nearest-enemy calls %zu, profile commands %zu\n",
                st.enqueues, st.regroups, st.refreshes, st.rescores, st.picks, st.picks_made, st.sites, st.sites_found, st.nearest, st.profile);
    std::printf("air base searches %zu (%zu bases listed)\n", st.air_base_searches, st.air_bases_found);
    static const char* kNames[] = {"resolve", "order", "build menu", "activate", "place check", "yard check", "range check", "explored", "visible"};
    std::printf("stub calls:");
    for (const auto& [k, v] : st.calls) std::printf(" %s %zu,", k >= kResolveId && k <= kVisibleId ? kNames[k - kResolveId] : "?", v);
    std::printf("\nresolutions by intent:");
    for (const auto& [k, v] : st.intents) std::printf(" %d:%zu", k, v);
    std::printf("\nmismatching scenarios %zu, engine faults %zu, predicted crashes confirmed %zu, original failures %zu\n", st.mismatches, st.faults,
                st.confirmed, st.failures);
    return st.mismatches || st.failures ? 1 : 0;
}
