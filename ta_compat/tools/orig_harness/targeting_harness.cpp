// Differential tester for target acquisition (sim/targeting.h). Inside a
// suspended TotalA.exe it builds a small battle like orders_harness (three
// players, units of random real types with real scripts and object states, the
// mission table from the original registration code), plus each player's unit
// table and weapon-target controller, a sight/explored grid, map cells with
// features and random category masks. The original routines run for real:
// unit table refresh (0x40B2C0), enemy listing (0x40AD80), best-target search
// (0x40B7B0), fire-at-will (0x43B700), attack order issue (0x43B1F0), mission
// resolution (0x43F0E0), retargeting (0x406F80) and the weapon-target pass
// (0x4089A0), with the original range check, order construction, clearing and
// weapon-slot routines underneath. Stubs: the line-of-sight test (answers from
// a queue), the AI rescoring, the "under attack" notice, the interceptor target
// search (answers from a queue), behaviours (an order destroyed while waiting
// for it), network notices and frees. After every operation it compares the
// stub log, results, the battle RNG, unit fields and weapon slots, both order
// lists of every unit field by field, target links, scripts, the unit tables
// and the controllers.
//
// Usage: targeting_harness <TotalA.exe> <game_dir> [scenarios] [steps] [first scenario]

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
#include "sim/missions.h"
#include "sim/orders.h"
#include "sim/pose.h"
#include "sim/rng.h"
#include "sim/script_vm.h"
#include "sim/targeting.h"
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
using u16 = std::uint16_t;
using u8 = std::uint8_t;

constexpr u32 kGameStatePtr = 0x00511DE8, kBattleRngState = 0x0051FC88, kTablePtrs = 0x005119C0;
constexpr u32 kRelocate = 0x004CB4C0, kNegateXZ = 0x004CB590, kBuildState = 0x0045A950;
constexpr u32 kCallNamed = 0x004B0940;
constexpr u32 kRegisterMissions = 0x0043C050, kMissionBegin = 0x00512344, kMissionEnd = 0x00512348;
constexpr u32 kAlloc = 0x004B4F10, kConstruct = 0x0043A0C0, kPrepend = 0x0043ACB0;
constexpr u32 kRefresh = 0x0040B2C0, kEnemies = 0x0040AD80, kBest = 0x0040B7B0, kFireAtWill = 0x0043B700,
              kIssueAttack = 0x0043B1F0, kResolve = 0x0043F0E0, kRetarget = 0x00406F80, kWeaponPass = 0x004089A0;
constexpr u32 kUnitVtable = 0x004FD698;
constexpr u32 kUnitSize = 0x118, kPlayerSize = 0x14B, kOrderBlock = 0x58;
constexpr int kMaxUnits = 18;

enum class StubRet { Zero, Queue, Silent };
struct Stub {
    u32 address;
    int args;
    int id;
    StubRet ret = StubRet::Zero;
    int queue = 0;
};
enum : int { qVisible, qIntercept, kQueues };
constexpr u32 kQueueSize = 0x4000;
constexpr int kVisibleId = 90, kRescoreId = 91, kUnderAttackId = 92, kInterceptId = 93, kHandlerId = 65, kFreeId = 53,
              kBroadcastId = 62, kSpeechId = 14, kGoalDetachId = 63, kAttachedDtorId = 64;
const Stub kStubs[] = {
    {0x00465AC0, 2, kVisibleId, StubRet::Queue, qVisible},      // unit visible(player record, unit)
    {0x00409730, 0, kRescoreId},                                // AI rescoring [fastcall: ecx = table]
    {0x0047F850, 3, kUnderAttackId},                            // under-attack notice(unit, 2, 0)
    {0x0049D120, 2, kInterceptId, StubRet::Queue, qIntercept},  // interceptor target(unit, slot) -> shot address
    {0x00456190, 2, kBroadcastId},                              // broadcast script call(unit, function)
    {0x0047F780, 3, kSpeechId},                                 // speech(unit, event, text)
    {0x004B4F20, 1, kFreeId},                                   // free(block) [cdecl]
    {0x00406F00, 0, 70, StubRet::Silent},                       // AI plan command registration (not simulated)
    // Script requests (no part of targeting; the engine side ignores them too).
    {0x00480EB0, 2, 71, StubRet::Silent},  // emit sfx(piece, type) [thiscall]
    {0x00481140, 2, 72, StubRet::Silent},  // explode(piece, type)
    {0x00481340, 3, 73, StubRet::Silent},  // attach(unit, piece, mode)
    {0x004813B0, 1, 74, StubRet::Silent},  // drop(unit)
};
const Stub kGoalDetach{0, 1, kGoalDetachId};
const Stub kAttachedDtor{0, 1, kAttachedDtorId};

std::size_t record_size(u32 id) {
    if (id == static_cast<u32>(kHandlerId)) return 5;
    if (id == static_cast<u32>(kGoalDetachId) || id == static_cast<u32>(kAttachedDtorId)) return 3;
    for (const Stub& s : kStubs)
        if (static_cast<u32>(s.id) == id) return 2 + static_cast<std::size_t>(s.args);
    return 0;
}

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8v(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void bytes(std::initializer_list<std::uint8_t> l) { for (auto v : l) b.push_back(v); }
    void align() { while (b.size() % 16) b.push_back(0xCC); }
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

void emit_stub(Emitter& e, const Stub& s, u32 log_ptr_addr, u32 queue_base) {
    const bool cdecl_ret = s.address == 0x004B4F20;
    if (s.ret != StubRet::Silent) {
        e.u8v(0xA1); e.u32le(log_ptr_addr);
        e.u8v(0xC7); e.u8v(0x00); e.u32le(static_cast<u32>(s.id));
        e.u8v(0x89); e.u8v(0x48); e.u8v(0x04);
        for (int i = 0; i < s.args; ++i) {
            e.u8v(0x8B); e.u8v(0x54); e.u8v(0x24); e.u8v(static_cast<std::uint8_t>(4 + 4 * i));
            e.u8v(0x89); e.u8v(0x50); e.u8v(static_cast<std::uint8_t>(8 + 4 * i));
        }
        e.u8v(0x83); e.u8v(0x05); e.u32le(log_ptr_addr); e.u8v(static_cast<std::uint8_t>(4 * (s.args + 2)));
    }
    if (s.ret == StubRet::Queue) emit_queue_read(e, queue_base + static_cast<u32>(s.queue) * kQueueSize);
    else { e.u8v(0x33); e.u8v(0xC0); }
    if (cdecl_ret || s.args == 0) e.u8v(0xC3);
    else { e.u8v(0xC2); e.u8v(static_cast<std::uint8_t>(4 * s.args)); e.u8v(0x00); }
}

struct Layout {
    u32 heap = 0, scenario_heap = 0, bump_ptr = 0, stubs = 0, log_ptr = 0, log_base = 0, gs = 0, units = 0, defs = 0, models = 0,
        cells = 0, schema = 0, names = 0, files = 0, pinfo = 0, empty_def = 0, motion = 0, vtables = 0, handler = 0, queues = 0,
        weapons = 0, tables = 0, controllers = 0, masks = 0, features = 0, sight = 0, shots = 0, out = 0;
};

bool setup(harness::Remote& r, Layout& L, const sim::HeightMap& hm, std::string& report) {
    L.heap = r.alloc(128u << 20);
    L.bump_ptr = r.alloc(0x1000);
    L.stubs = r.alloc(0x2000, PAGE_EXECUTE_READWRITE);
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
    L.pinfo = r.alloc(0x1000);
    L.empty_def = r.alloc(0x1000);
    L.motion = r.alloc(0x2000);
    L.vtables = r.alloc(0x1000);
    L.queues = r.alloc(kQueues * kQueueSize);
    L.weapons = r.alloc(0x2000);
    L.tables = r.alloc(0x4000);
    L.controllers = r.alloc(0x1000);
    L.masks = r.alloc(0x1000);
    L.features = r.alloc(0x10000);
    L.sight = r.alloc(0x40000);
    L.shots = r.alloc(0x1000);
    L.out = r.alloc(0x1000);
    if (!L.heap || !L.stubs || !L.log_base || !L.gs || !L.units || !L.cells || !L.files || !L.queues || !L.tables || !L.sight) {
        report = "allocation failed";
        return false;
    }
    Emitter e;
    auto here = [&]() { return L.stubs + static_cast<u32>(e.b.size()); };
    // Bump allocators in place of the heap routines.
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
    for (const Stub& s : kStubs) {
        const u32 at = here();
        emit_stub(e, s, L.log_ptr, L.queues);
        e.align();
        if (!r.patch_jump(s.address, at)) {
            report = "patch failed";
            return false;
        }
    }
    const u32 goal_detach = here();
    emit_stub(e, kGoalDetach, L.log_ptr, L.queues);
    e.align();
    const u32 attached_dtor = here();
    emit_stub(e, kAttachedDtor, L.log_ptr, L.queues);
    e.align();
    r.w32(L.vtables + 0x04, goal_detach);
    r.w32(L.vtables + 0x40, attached_dtor);
    // Every mission behaviour (reached when an order waiting for its destruction is destroyed):
    // logs (unit, order, event, kind) and returns 2.
    L.handler = here();
    e.u8v(0xA1); e.u32le(L.log_ptr);
    e.u8v(0xC7); e.u8v(0x00); e.u32le(static_cast<u32>(kHandlerId));
    e.bytes({0x8B, 0x4C, 0x24, 0x04, 0x89, 0x48, 0x04});
    e.bytes({0x8B, 0x4C, 0x24, 0x08, 0x89, 0x48, 0x08});
    e.bytes({0x8B, 0x4C, 0x24, 0x0C, 0x89, 0x48, 0x0C});
    e.bytes({0x8B, 0x4C, 0x24, 0x08, 0x0F, 0xB6, 0x49, 0x04, 0x89, 0x48, 0x10});
    e.u8v(0x83); e.u8v(0x05); e.u32le(L.log_ptr); e.u8v(0x14);
    e.bytes({0xB8, 0x02, 0x00, 0x00, 0x00});  // mov eax,2
    e.bytes({0xC2, 0x0C, 0x00});
    e.align();
    if (e.b.size() > 0x2000 || !r.write(L.stubs, e.b.data(), e.b.size())) {
        report = "stub write failed";
        return false;
    }
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
    r.w32(L.gs + 0x1426F, L.features);
    r.w32(L.gs + 0x14273, L.sight);
    if (!r.call(kRegisterMissions, 0, {})) {
        report = "mission registration failed";
        return false;
    }
    const u32 begin = r.r32(kMissionBegin), end = r.r32(kMissionEnd);
    const u32 count = (end - begin) / 0x19;
    for (u32 k = 0; k < count; ++k) r.w32(begin + k * 0x19 + 4, L.handler);
    report = "mission table: " + std::to_string(count) + " entries";
    r.w32(L.log_ptr, L.log_base);
    L.scenario_heap = (r.r32(L.bump_ptr) + 0xFFF) & ~0xFFFu;
    return count == sim::mission::kCount;
}

// Remote offsets.
enum : u32 {
    uMotion = 0x00, uOrders = 0x5C, uSecondary = 0x60, uPosX = 0x6A, uPosY = 0x6E, uPosZ = 0x72, uCarrier = 0x86, uCargoHead = 0x8A,
    uCargoNext = 0x8E, uDef = 0x92, uOwner = 0x96, uScript = 0x9A, uState = 0x9E, uReferrers = 0xA2, uDefIndex = 0xA6, uId = 0xA8,
    uNetDirty = 0xBA, uLastAttackerSlot = 0xF4, uLastDamageKind = 0xF5, uHoldoff = 0xFB, uOwnerSlot = 0xFF, uBuildLeft = 0x104,
    uHp = 0x108, uActivate = 0x10E, uFlags = 0x110,
    pActive = 0x00, pFirst = 0x67, pLast = 0x6B, pController = 0x73, pControllerObj = 0x74, pSightW = 0x80, pSightH = 0x84,
    pAllied = 0x108, pState = 0x146,
    dName = 0x20, dFootprintX = 0x14A, dBuildOptions = 0x156, dModelMaxY = 0x16E, dCob = 0x18E, dMaxWaterDepth = 0x1BE,
    dMinWaterDepth = 0x1C0, dWeapon1 = 0x1EE, dMaxDamage = 0x1FA, dSight = 0x202, dLeash = 0x214, dTransportSize = 0x22A,
    dTransportCap = 0x22B, dBadTarget = 0x231, dNoChase = 0x23D, dTypeFlags = 0x241, dCanDo = 0x245, kDefSize = 0x249,
    wRange = 0xDC, wVelocity = 0x68, wMinAngle = 0xC8, wFlags = 0x111,
    gGravity = 0x14263, gUnitsEnd = 0x1435B, gSea = 0x1427F, gTick = 0x38A47, gLocalPlayer = 0x2A42, gLocalView = 0x2A43,
    gUnitLimit = 0x37EE6, gOptions = 0x37F30, gCommandMode = 0x37EFA, gFeatureCount = 0x14253,
    oKind = 0x04, oStep = 0x05, oWait = 0x06, oWake = 0x0A, oUnit = 0x0E, oLink = 0x12, oPos = 0x22, oScratch = 0x2E, oParams = 0x36,
    oFlags = 0x42, oCreated = 0x46, oNext = 0x4A, oEvents = 0x4E, oAttached = 0x52,
    tPlayer = 0x00, tSeenBegin = 0x09, tSeenEnd = 0x0D, tRadarBegin = 0x19, tRadarEnd = 0x1D, tUpgrade = 0x79, tCounts = 0x81,
    tCountsEnd = 0x85, tWeights = 0xA1, tRefresh = 0xED, kTableSize = 0x200,
    cPlayer = 0x00, cHold = 0x0D, cCursor = 0x39, kControllerSize = 0x40,
    mGoal = 0x00, mGoalObject = 0x10,
};

std::size_t g_intercepts = 0, g_intercept_hits = 0;  // interceptor target searches (and how many found a shot)

struct LoadedType {
    const ta::UnitDefRecord* def = nullptr;
    std::vector<std::uint8_t> model_file, cob_file;
    sim::PoseModel pose_model;
    ta::CobFile cob;
    sim::UnitTypeInfo info;
    sim::UnitTickTypeInfo tick_info;
    u32 remote_model = 0, remote_cob = 0, remote_def = 0;
};

struct World : sim::UnitWorld, sim::CombatSystems, sim::OrderSystems, sim::MissionSystems, sim::TargetingSystems {
    std::vector<sim::Unit>* units = nullptr;
    std::vector<sim::UnitTickState>* ticks = nullptr;
    std::vector<sim::PlayerSlot>* players = nullptr;
    sim::OrderContext* orders = nullptr;
    const sim::HeightMap* hm = nullptr;
    u32 units_base = 0, shots_base = 0;
    sim::BattleRng rng;
    std::vector<u32> log;
    std::array<std::vector<u32>, kQueues> queue;
    std::array<std::size_t, kQueues> queue_at{};
    std::vector<std::array<i32, 3>> shot_pos;  // fake shots (intercept answers)
    std::vector<u8> feature_flag_bytes;
    // Sight grid: per player width/height, shared u16 cells; the local view bit.
    std::array<u32, 10> sight_w{}, sight_h{};
    std::vector<u16> sight_cells;
    u8 local_view = 0;
    u32 next_address = 0;
    std::map<u32, sim::MissionOrder*> live;
    std::set<u32> all_orders;
    std::set<u32> motion_units;  // ids with a motion object

    ~World() override {
        for (auto& [a, o] : live) delete o;
    }
    static u32 tok(const sim::Unit& u) { return 0x40000000u | u.id; }
    void rec(int id, u32 ecx, std::initializer_list<u32> args) {
        log.push_back(static_cast<u32>(id));
        log.push_back(ecx);
        for (u32 a : args) log.push_back(a);
    }
    u32 next_answer(int q) {
        const auto& v = queue[static_cast<std::size_t>(q)];
        std::size_t& at = queue_at[static_cast<std::size_t>(q)];
        return at < v.size() ? v[at++] : 0u;
    }
    // UnitWorld
    sim::Unit* unit_by_id(std::uint16_t id) override { return id < units->size() ? &(*units)[id] : nullptr; }
    const sim::HeightMap& heightmap() override { return *hm; }
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
    // OrderSystems
    std::int32_t run_mission(sim::Unit& u, sim::MissionOrder& o, std::uint32_t& event) override {
        log.insert(log.end(), {static_cast<u32>(kHandlerId), tok(u), o.address, event, o.kind});
        return 2;
    }
    void clear_weapon_target(sim::Unit& u, int slot) override { sim::clear_weapon_target(u, slot); }
    void release_weapon_targets(sim::Unit& u) override { sim::release_weapon_targets(u, 3); }
    void release_attached(sim::Unit&, sim::MissionOrder& o) override { o.attached = 0; }
    void broadcast_script_call(sim::Unit& u, std::int16_t fn) override { rec(kBroadcastId, 0, {tok(u), static_cast<u32>(static_cast<i32>(fn))}); }
    sim::MissionOrder* allocate_order() override {
        auto* o = new sim::MissionOrder;
        o->address = next_address;
        next_address += kOrderBlock;
        live[o->address] = o;
        all_orders.insert(o->address);
        return o;
    }
    void free_order(sim::MissionOrder* o) override {
        rec(kFreeId, 0, {o->address});
        live.erase(o->address);
        delete o;
    }
    // MissionSystems (only the parts the range check and orders reach)
    std::int32_t run_other(sim::Unit&, sim::MissionOrder&, std::uint32_t&) override { return 2; }
    void speech(sim::Unit& u, std::uint32_t id, sim::SpeechText) override { rec(kSpeechId, 0, {tok(u), id, 0}); }
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
    // Movement behaviours are not run by this harness (no motion goals).
    void attach_circle_goal(sim::Unit&, sim::MissionOrder&, const std::array<i32, 3>&, i32) override {}
    // TargetingSystems
    bool unit_visible(int player, sim::Unit& u) override {
        rec(kVisibleId, 0, {0x20000000u | static_cast<u32>(player), tok(u)});
        return next_answer(qVisible) != 0;
    }
    bool position_explored(int player, const std::array<i32, 3>& pos) override {
        const u32 cx = static_cast<u32>(static_cast<i32>(static_cast<std::int16_t>(static_cast<u32>(pos[0]) >> 16)) >> 5);
        const i32 yz = static_cast<std::int16_t>(static_cast<u32>(pos[2]) >> 16) - (static_cast<std::int16_t>(static_cast<u32>(pos[1]) >> 16) >> 1);
        const u32 cz = static_cast<u32>(yz >> 5);
        const std::size_t p = static_cast<std::size_t>(player);
        if (cx >= sight_w[p] || cz >= sight_h[p]) return false;
        const std::size_t at = static_cast<std::size_t>(sight_w[p]) * cz + cx;
        return at < sight_cells.size() && ((sight_cells[at] >> (local_view & 31)) & 1u);
    }
    std::uint8_t feature_flags(std::uint16_t f) override { return f < feature_flag_bytes.size() ? feature_flag_bytes[f] : 0; }
    std::int32_t intercept_target(sim::Unit& u, int slot, std::array<i32, 3>& pos) override {
        rec(kInterceptId, 0, {tok(u), static_cast<u32>(slot)});
        ++g_intercepts;
        const u32 a = next_answer(qIntercept);
        if (!a) return -1;
        ++g_intercept_hits;
        const std::size_t k = (a - shots_base) / 0x40;
        pos = shot_pos[k];
        return static_cast<std::int32_t>(k);
    }
    void ai_own_unit(int, sim::Unit&) override {}
    void ai_rescore(int table) override { rec(kRescoreId, 0x30000000u | static_cast<u32>(table), {}); }
    void under_attack(sim::Unit& u) override { rec(kUnderAttackId, 0, {tok(u), 2, 0}); }
};

struct Stats {
    std::size_t scenarios = 0, ops = 0, mismatches = 0, failures = 0, faults = 0;
    std::map<std::string, std::size_t> by_op;
    std::size_t targets_found = 0, attack_orders = 0, resolved_nonzero = 0, retargets_aimed = 0, refreshes = 0, listed = 0, pass_changes = 0;
    std::map<int, std::size_t> resolved_kinds;
    std::map<int, std::size_t> intents;  // resolutions by intent
};


bool run_scenario(harness::Remote& r, const Layout& L, std::vector<LoadedType>& types, const sim::HeightMap& hm, int steps,
                  unsigned seed, Stats& st) {
    std::mt19937 g(seed);
    r.w32(L.bump_ptr, L.scenario_heap);
    std::vector<std::uint8_t> zero(0x40000, 0);
    r.write(L.gs + 0x1B63, zero.data(), 10 * kPlayerSize);
    r.write(L.units, zero.data(), (kMaxUnits + 2) * kUnitSize);
    r.write(L.motion, zero.data(), 0x2000);
    r.write(L.tables, zero.data(), 0x4000);
    r.write(L.controllers, zero.data(), 0x1000);
    for (int k = 0; k < 10; ++k) r.w32(kTablePtrs + static_cast<u32>(k) * 4, 0);

    // Weapon types (random flags the targeting code reads).
    const int nweapons = 6;
    std::vector<sim::WeaponType> wtypes(nweapons);
    for (int w = 0; w < nweapons; ++w) {
        sim::WeaponType& wt = wtypes[static_cast<std::size_t>(w)];
        using namespace sim::weapon_flags;
        wt.flags = (g() % 4 == 0 ? kBallistic : 0u) | (g() % 5 == 0 ? kWaterWeapon : 0u) | (g() % 5 == 0 ? 1u << 17 : 0u) |
                   (g() % 5 == 0 ? kParalyzer : 0u) | (g() % 6 == 0 ? kDropped : 0u) | (g() % 5 == 0 ? kCommandFire : 0u) |
                   (g() % 6 == 0 ? kInterceptor : 0u) | (g() % 2 ? kTurret : 0u);
        wt.velocity = static_cast<i32>(0x10000 + g() % 0x400000);
        wt.min_barrel_angle = static_cast<float>(g() % 1000) / 1000.0f - 0.5f;
        wt.range = static_cast<i32>(g() % 8 == 0 ? g() % 0x10000 : 100 + g() % 1200);
        std::vector<std::uint8_t> wd(0x200, 0);
        std::memcpy(&wd[wFlags], &wt.flags, 4);
        std::memcpy(&wd[wVelocity], &wt.velocity, 4);
        std::memcpy(&wd[wMinAngle], &wt.min_barrel_angle, 4);
        std::memcpy(&wd[wRange], &wt.range, 4);
        r.write(L.weapons + static_cast<u32>(w) * 0x200, wd.data(), wd.size());
    }
    auto weapon_addr = [&](int w) { return L.weapons + static_cast<u32>(w) * 0x200; };

    // Unit types: real models and scripts, random targeting data.
    const int ntypes = 3;
    std::vector<int> pick;
    std::vector<sim::MissionTypeInfo> mission_types(ntypes + 1);
    std::vector<sim::TargetingTypeInfo> ttypes(ntypes + 1);
    std::vector<int> first_weapon(ntypes + 1, 0);
    u32 file_at = L.files;
    u32 mask_at = L.masks;
    for (int k = 0; k < ntypes; ++k) {
        std::size_t ti = g() % types.size();
        while (std::find(pick.begin(), pick.end(), static_cast<int>(ti)) != pick.end()) ti = g() % types.size();
        LoadedType& t = types[ti];
        pick.push_back(static_cast<int>(ti));
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
        const std::size_t kk = static_cast<std::size_t>(k + 1);
        // Type flags: can fly, hover, shoot me, has weapons, amphibious, hover attack, kamikaze,
        // air base, targeting facility (bits the targeting code reads), the rest from the definition.
        u32 tf = t.def->type_flags;
        const u32 random_bits[] = {0x200, 0x400, 0x800, 0x1000, 0x8000, 0x10000, 0x200000, 0x8000000, 0x10000000};
        for (u32 b : random_bits)
            if (g() % 3 == 0) tf ^= b;
        if (g() % 2) tf |= 0x8000;  // most units can be shot at
        t.tick_info.type_flags = tf;
        sim::MissionTypeInfo& mt = mission_types[kk];
        mt.abilities = static_cast<u32>(g()) & 0x000FFFF0u;
        if (g() % 2) mt.abilities |= 0x10;  // can attack
        sim::TargetingTypeInfo& tt = ttypes[kk];
        tt.category_bit = static_cast<u16>(kk);
        auto random_mask = [&]() {
            sim::CategoryMask m{};
            m[0] = g() % 3 == 0 ? static_cast<u32>(g()) & 0xF : 0u;
            return m;
        };
        tt.bad_target = {random_mask(), random_mask(), random_mask()};
        tt.no_chase = random_mask();
        tt.sight_distance = static_cast<std::int16_t>(g() % 8 == 0 ? static_cast<i32>(g() % 0x10000) - 0x8000 : static_cast<i32>(g() % 1400));
        tt.maneuver_leash = static_cast<std::int16_t>(g());
        tt.max_water_depth = static_cast<std::int16_t>(static_cast<i32>(g() % 400) - 100);
        tt.min_water_depth = static_cast<std::int16_t>(static_cast<i32>(g() % 400) - 300);
        tt.footprint_x = static_cast<std::int16_t>(g() % 6);
        tt.transport_size = static_cast<u8>(g() % 6);
        tt.transport_capacity = static_cast<u8>(g() % 4);
        tt.has_build_options = g() % 3 == 0;
        first_weapon[kk] = static_cast<int>(g() % nweapons);
        tt.first_weapon = &wtypes[static_cast<std::size_t>(first_weapon[kk])];
        t.info.max_damage = 1 + g() % 3000;
        t.info.model_max_y = static_cast<i32>(g() % 0x400000);
        t.remote_def = L.defs + static_cast<u32>(k) * 0x400;
        std::vector<std::uint8_t> d(kDefSize, 0);
        auto put = [&](u32 off, const void* p, std::size_t n) { std::memcpy(&d[off], p, n); };
        put(dName, t.tick_info.name.c_str(), (std::min)(t.tick_info.name.size() + 1, std::size_t{32}));
        put(dCob, &t.remote_cob, 4);
        put(dFootprintX, &tt.footprint_x, 2);
        const u32 bo = tt.has_build_options ? 0x1234u : 0u;
        put(dBuildOptions, &bo, 4);
        put(dModelMaxY, &t.info.model_max_y, 4);
        put(dMaxWaterDepth, &tt.max_water_depth, 2);
        put(dMinWaterDepth, &tt.min_water_depth, 2);
        const u32 w1 = weapon_addr(first_weapon[kk]);
        put(dWeapon1, &w1, 4);
        put(dMaxDamage, &t.info.max_damage, 4);
        put(dSight, &tt.sight_distance, 2);
        put(dLeash, &tt.maneuver_leash, 2);
        put(dTransportSize, &tt.transport_size, 1);
        put(dTransportCap, &tt.transport_capacity, 1);
        for (int m = 0; m < 4; ++m) {
            const sim::CategoryMask& cm = m < 3 ? tt.bad_target[static_cast<std::size_t>(m)] : tt.no_chase;
            r.write(mask_at, cm.data(), 64);
            put(m < 3 ? dBadTarget + static_cast<u32>(m) * 4 : dNoChase, &mask_at, 4);
            mask_at += 64;
        }
        put(dTypeFlags, &tf, 4);
        put(dCanDo, &mt.abilities, 4);
        r.write(t.remote_def, d.data(), d.size());
        r.w32(L.models + static_cast<u32>(k + 1) * 4, t.remote_model);
    }
    {
        std::vector<std::uint8_t> ed(kDefSize, 0);
        // The blank type's masks (dead targets have type index 0).
        r.write(mask_at, zero.data(), 64);
        for (int m = 0; m < 4; ++m) std::memcpy(&ed[m < 3 ? dBadTarget + static_cast<u32>(m) * 4 : dNoChase], &mask_at, 4);
        r.write(L.empty_def, ed.data(), ed.size());
    }

    // Players 0..2 playing; tables 0..3 (index = player record).
    std::vector<sim::PlayerSlot> players(11);
    std::vector<std::array<std::uint8_t, 16>> allied(11);
    const int per = kMaxUnits / 3;
    for (int p = 0; p < 10; ++p) {
        sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        const bool playing = p < 3;
        s.active = playing || g() % 3 == 0;
        s.controller = static_cast<std::uint8_t>(playing ? 1 + g() % 3 : g() % 4);
        s.state = static_cast<std::uint8_t>(g() % 4);
        if (playing) {
            s.first_unit = 1 + p * per;
            s.last_unit = s.first_unit + per - 1;
        }
        const u32 at = L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize;
        r.w32(at + pActive, s.active ? 1u : 0u);
        r.write(at + pController, &s.controller, 1);
        r.write(at + pState, &s.state, 1);
        auto& al = allied[static_cast<std::size_t>(p)];
        for (int k = 0; k < 4; ++k) al[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(g() % 3 == 0 ? 1 + g() % 255 : 0);
        r.write(at + pAllied, al.data(), 4);
        r.w32(at + pFirst, L.units + static_cast<u32>(playing ? s.first_unit : 1) * kUnitSize);
        r.w32(at + pLast, L.units + static_cast<u32>(playing ? s.last_unit : 0) * kUnitSize);
    }

    // Units.
    std::vector<sim::Unit> units(kMaxUnits + 2);
    std::vector<sim::UnitTickState> tstate(kMaxUnits + 2);
    sim::CombatEnvironment cenv;
    sim::UnitTypeInfo empty_type;
    empty_type.max_damage = 0;
    sim::UnitTickTypeInfo empty_tick;
    cenv.empty_type = &empty_type;
    cenv.empty_tick_type = &empty_tick;
    World world;
    sim::Combat combat{units, tstate, players, cenv, world};
    sim::OrderContext octx{tstate, players, world.rng, world};
    sim::MissionEnvironment menv;
    menv.gravity = static_cast<i32>(g() % 3 == 0 ? 1 + g() % 4000 : 50 + g() % 200);
    menv.addresses.game_state = L.gs;
    menv.addresses.unit_table = L.units;
    r.w32(L.gs + gGravity, static_cast<u32>(menv.gravity));
    r.w32(L.gs + gUnitsEnd, L.units + static_cast<u32>(units.size() - 1) * kUnitSize);
    sim::Missions missions{octx, units, combat, world, world, mission_types, allied, menv};
    sim::MapGrid map;
    map.width = hm.width();
    map.height = hm.height();
    map.cells.assign(static_cast<std::size_t>(map.width) * static_cast<std::size_t>(map.height), sim::MapCell{});
    sim::TargetingEnvironment tenv;
    sim::TargetingState tstate_t;
    sim::Targeting targeting{units, tstate, players, allied, ttypes, mission_types, map, 0, missions, tenv, world, tstate_t};
    world.units_base = L.units;
    world.shots_base = L.shots;
    world.units = &units;
    world.ticks = &tstate;
    world.players = &players;
    world.orders = &octx;
    world.hm = &hm;
    const u32 rng0 = static_cast<u32>(g()) | 1u;
    r.w32(kBattleRngState, rng0);
    world.rng.set_state(rng0);

    // Features: a few feature types with random flag bytes; cells holding them (and parts of them).
    const u32 nfeatures = 1 + g() % 6;
    targeting.feature_count = nfeatures;
    r.w32(L.gs + gFeatureCount, nfeatures);
    world.feature_flag_bytes.resize(nfeatures);
    for (u32 f = 0; f < nfeatures; ++f) {
        world.feature_flag_bytes[f] = static_cast<u8>(g() % 2 ? 0x80 | (g() & 0x7F) : g() & 0x7F);
        r.write(L.features + f * 0x100 + 0xFE, &world.feature_flag_bytes[f], 1);
    }
    std::vector<std::uint8_t> cells(map.cells.size() * 13, 0);
    for (std::size_t c = 0; c < map.cells.size(); ++c) {
        map.cells[c].feature = 0xFFFF;
        const std::uint16_t v = 0xFFFF;
        std::memcpy(&cells[c * 13 + 8], &v, 2);
    }
    // The units live in a 2048 x 2048 area near the map origin (cells 8..136).
    std::vector<std::size_t> feature_cells;
    for (int k = 0; k < 120; ++k) {
        const i32 cx = static_cast<i32>(8 + g() % 128), cz = static_cast<i32>(8 + g() % 128);
        if (cx >= map.width - 2 || cz >= map.height - 2) continue;
        const std::size_t at = static_cast<std::size_t>(cz) * static_cast<std::size_t>(map.width) + static_cast<std::size_t>(cx);
        sim::MapCell& mc = map.cells[at];
        const u32 kind = g() % 8;
        mc.feature = static_cast<u16>(kind == 0 ? nfeatures + g() % 4 : kind == 1 ? 0xFFFB + g() % 5 : g() % nfeatures);
        mc.feature_aux = 0;
        std::memcpy(&cells[at * 13 + 8], &mc.feature, 2);
        feature_cells.push_back(at);
        if (mc.feature < nfeatures && g() % 2) {  // a bigger feature: the cell right of it refers back
            sim::MapCell& part = map.cells[at + 1];
            part.feature = 0xFFFE;
            part.feature_aux = 0x0100;  // 0 rows, 1 column back
            std::memcpy(&cells[(at + 1) * 13 + 8], &part.feature, 2);
            std::memcpy(&cells[(at + 1) * 13 + 0xA], &part.feature_aux, 2);
            feature_cells.push_back(at + 1);
        }
    }
    r.write(L.cells, cells.data(), cells.size());
    // Sight grids (32-unit cells).
    world.sight_cells.assign(128 * 128, 0);
    for (auto& c : world.sight_cells) c = static_cast<u16>(g() % 2 ? g() : 0);
    r.write(L.sight, world.sight_cells.data(), world.sight_cells.size() * 2);
    for (int p = 0; p < 10; ++p) {
        world.sight_w[static_cast<std::size_t>(p)] = 16 + g() % 112;
        world.sight_h[static_cast<std::size_t>(p)] = (128 * 128) / world.sight_w[static_cast<std::size_t>(p)] > 128 ? 128 : (128 * 128) / world.sight_w[static_cast<std::size_t>(p)];
        const u32 at = L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize;
        r.w32(at + pSightW, world.sight_w[static_cast<std::size_t>(p)]);
        r.w32(at + pSightH, world.sight_h[static_cast<std::size_t>(p)]);
    }
    // Fake shots for the interceptor answers.
    for (int k = 0; k < 8; ++k) {
        std::array<i32, 3> pos = {static_cast<i32>(g()), static_cast<i32>(g()), static_cast<i32>(g())};
        if (k == 0) pos[2] = static_cast<i32>(0x80000000u | (g() & 0xFFFF));  // z = -0x8000 whole units
        world.shot_pos.push_back(pos);
        r.write(L.shots + static_cast<u32>(k) * 0x40 + 4, pos.data(), 12);
    }

    std::vector<std::unique_ptr<sim::UnitScriptHost>> hosts(units.size());
    std::vector<u32> remote_script(units.size(), 0);
    std::vector<int> present;
    auto unit_addr = [&](int i) { return L.units + static_cast<u32>(i) * kUnitSize; };
    for (int i = 1; i <= kMaxUnits; ++i) {
        units[static_cast<std::size_t>(i)].id = static_cast<std::uint16_t>(i);
        tstate[static_cast<std::size_t>(i)].owner = (i - 1) / per;
        if ((i - 1) / per <= 2 && g() % 6 != 0) present.push_back(i);
    }
    if (present.empty()) return true;
    for (int i = 1; i <= kMaxUnits; ++i) {  // owner of every entry of the ranges (dead ones too)
        const u32 at = unit_addr(i);
        const int o = tstate[static_cast<std::size_t>(i)].owner;
        r.w32(at + uOwner, L.gs + 0x1B63 + static_cast<u32>(o) * kPlayerSize);
        const u8 os = static_cast<u8>(o);
        r.write(at + uOwnerSlot, &os, 1);
        r.w32(at + uDef, L.empty_def);
        r.write(at + uId, &units[static_cast<std::size_t>(i)].id, 2);
        units[static_cast<std::size_t>(i)].type = &empty_type;
        tstate[static_cast<std::size_t>(i)].tick_type = &empty_tick;
    }
    for (int i : present) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
        const int k = static_cast<int>(g() % ntypes);
        LoadedType& lt = types[static_cast<std::size_t>(pick[static_cast<std::size_t>(k)])];
        t.type_index = static_cast<std::uint16_t>(k + 1);
        t.tick_type = &lt.tick_info;
        u.type = &lt.info;
        auto coord = [&]() -> i32 { return static_cast<i32>((static_cast<u32>(128 + g() % 2048) << 16) | (g() & 0xFFFF)); };
        u.pos = {coord(), static_cast<i32>(g() % 3 == 0 ? (g() % 255) << 16 : (g() % 60) << 16), coord()};
        u.hit_points = static_cast<std::int16_t>(g() % 4 == 0 ? static_cast<i32>(lt.info.max_damage) : static_cast<i32>(g() % (lt.info.max_damage + 50)));
        u.flags = sim::unit_flags::kAlive | (g() % 2 ? 0x20u : 0u) | (g() % 12 == 0 ? 0x4000u : 0u) | (g() % 6 == 0 ? 2u : 1u) |
                  (g() % 3 ? 0x80000000u : 0u) | (g() % 4 == 0 ? 0x20000000u : 0u) | (static_cast<u32>(g() % 16) << 18) |
                  (g() % 4 == 0 ? 0x100u : 0u) | (g() % 8 == 0 ? 0x8000u : 0u);
        if (g() % 2) u.flags = (u.flags & ~0x300000u) | 0x200000u;  // fire at will more often
        u.activate_flags = static_cast<std::uint8_t>((g() % 2 ? 1u : 0u) | (g() % 5 == 0 ? 0x10u : 0u));
        u.net_dirty = static_cast<std::uint16_t>(g() % 3 == 0 ? g() : 0);
        u.build_fraction_left = g() % 6 == 0 ? 0.5f : 0.0f;
        t.has_motion = g() % 4 != 0;
        t.last_attacker_slot = static_cast<u8>(g() % 11);
        t.last_damage_kind = static_cast<u8>(g() % 4);
        t.transfer_holdoff = g() % 4 == 0 ? g() % 100 : 0;
        const u32 at = unit_addr(i);
        if (t.has_motion) {
            const u32 mb = L.motion + static_cast<u32>(i) * 0x40;
            r.w32(mb + mGoal, mb + mGoalObject);
            r.w32(mb + mGoalObject, L.vtables);
            r.w32(mb + mGoalObject + 4, 0);
            r.w32(at + uMotion, mb);
        }
        r.write(at + uPosX, &u.pos[0], 4);
        r.write(at + uPosY, &u.pos[1], 4);
        r.write(at + uPosZ, &u.pos[2], 4);
        r.w32(at + uDef, lt.remote_def);
        r.write(at + uDefIndex, &t.type_index, 2);
        r.write(at + uNetDirty, &u.net_dirty, 2);
        r.write(at + uLastAttackerSlot, &t.last_attacker_slot, 1);
        r.write(at + uLastDamageKind, &t.last_damage_kind, 1);
        r.w32(at + uHoldoff, t.transfer_holdoff);
        r.write(at + uBuildLeft, &u.build_fraction_left, 4);
        r.write(at + uHp, &u.hit_points, 2);
        r.w32(at + uFlags, u.flags);
        r.write(at + uActivate, &u.activate_flags, 1);
        for (int s = 0; s < 3; ++s) {
            sim::WeaponSlot& ws = u.weapons[static_cast<std::size_t>(s)];
            const u32 kind = g() % 4;
            ws.target_x = kind == 0 ? 0 : static_cast<std::int16_t>(kind == 1 ? g() % 3000 : 1 + g() % kMaxUnits);
            ws.target_z = kind == 1 ? static_cast<std::int16_t>(g() % 3000) : sim::kUnitTarget;
            ws.flags = static_cast<std::uint8_t>((g() & 0x0D) | (g() % 4 ? 2u : 0u) | (g() % 3 ? 0x10u : 0u) | static_cast<u32>(s) << 2);
            const u32 wi = s == 0 ? static_cast<u32>(first_weapon[t.type_index]) : g() % nweapons;
            ws.type = &wtypes[wi];
            const u32 sa = at + 4 + static_cast<u32>(s) * 0x1C;
            r.write(sa + 0x00, &ws.target_x, 2);
            r.write(sa + 0x02, &ws.target_z, 2);
            r.w32(sa + 0x0C, weapon_addr(static_cast<int>(wi)));
            r.write(sa + 0x1B, &ws.flags, 1);
        }
        u32 state = 0;
        if (!r.call(kBuildState, 0, {lt.remote_model, lt.remote_cob, at}, &state) || !state) return false;
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
        u.pose.build(lt.pose_model, lt.cob.program.piece_names);
        hosts[static_cast<std::size_t>(i)] = std::make_unique<sim::UnitScriptHost>(u, world);
        u.script = std::make_unique<sim::ScriptInstance>(lt.cob.program, *hosts[static_cast<std::size_t>(i)], 30);
        u.script->set_step_limit(50000000);
    }
    // Cargo: a few units carry others (some list entries name another carrier).
    for (int i : present) {
        if (g() % 4) continue;
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::Unit* prev = nullptr;
        for (int n = static_cast<int>(1 + g() % 2); n > 0; --n) {
            const int c = present[g() % present.size()];
            sim::Unit& cu = units[static_cast<std::size_t>(c)];
            if (c == i || cu.carrier || &cu == u.cargo_head || cu.cargo_next || cu.cargo_head || (prev && prev == &cu)) continue;
            cu.carrier = g() % 3 ? &u : &units[static_cast<std::size_t>(present[0])];
            if (prev) prev->cargo_next = &cu;
            else u.cargo_head = &cu;
            prev = &cu;
        }
    }
    for (int i : present) {
        const sim::Unit& u = units[static_cast<std::size_t>(i)];
        const u32 at = unit_addr(i);
        r.w32(at + uCarrier, u.carrier ? unit_addr(u.carrier->id) : 0u);
        r.w32(at + uCargoHead, u.cargo_head ? unit_addr(u.cargo_head->id) : 0u);
        r.w32(at + uCargoNext, u.cargo_next ? unit_addr(u.cargo_next->id) : 0u);
    }
    // Unit tables (index 0..3, made for that player record) and the playing players' controllers.
    std::vector<u32> table_addr(10, 0);
    for (int k = 0; k < 4; ++k) {
        if (k == 3 && g() % 2) continue;  // the playing players' tables always exist (a missing one crashes the original)
        sim::UnitTable& tb = tstate_t.tables[static_cast<std::size_t>(k)];
        tb.exists = true;
        tb.player = k;
        tb.refresh_tick = 0;
        const u32 ta = L.tables + static_cast<u32>(k) * kTableSize;
        table_addr[static_cast<std::size_t>(k)] = ta;
        r.w32(ta + tPlayer, L.gs + 0x1B63 + static_cast<u32>(k) * kPlayerSize);
        const u32 counts = ta + 0x100, weights = ta + 0x140;
        r.w32(ta + tCounts, counts);
        r.w32(ta + tCountsEnd, counts + 2 * (ntypes + 1));
        r.w32(ta + tWeights, weights);
        for (int w = 0; w <= ntypes; ++w) {
            const u8 wv = static_cast<u8>(g() % 200);
            r.write(weights + static_cast<u32>(w), &wv, 1);
        }
        r.w32(kTablePtrs + static_cast<u32>(k) * 4, ta);
    }
    std::vector<u32> controller_addr(10, 0);
    for (int p = 0; p < 3; ++p) {
        sim::PlayerController& c = tstate_t.controllers[static_cast<std::size_t>(p)];
        c.exists = true;
        const u32 ca = L.controllers + static_cast<u32>(p) * kControllerSize;
        controller_addr[static_cast<std::size_t>(p)] = ca;
        r.w32(ca + cPlayer, L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize);
        r.w32(L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize + pControllerObj, ca);
    }
    ++st.scenarios;

    auto norm_ptr = [&](u32& w) {
        if (w >= L.units && w < L.units + (kMaxUnits + 2) * kUnitSize) {
            const u32 off = (w - L.units) % kUnitSize, idx = (w - L.units) / kUnitSize;
            if (off == 0) w = 0x40000000u | idx;
        } else if (w >= L.gs + 0x1B63 && w < L.gs + 0x1B63 + 10 * kPlayerSize && (w - L.gs - 0x1B63) % kPlayerSize == 0) {
            w = 0x20000000u | ((w - L.gs - 0x1B63) / kPlayerSize);
        } else if (w >= L.tables && w < L.tables + 10 * kTableSize && (w - L.tables) % kTableSize == 0) {
            w = 0x30000000u | ((w - L.tables) / kTableSize);
        }
    };
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
            if (remote && id != static_cast<u32>(kHandlerId) && id != static_cast<u32>(kRescoreId) && id != static_cast<u32>(kGoalDetachId) &&
                id != static_cast<u32>(kAttachedDtorId))
                rec[1] = 0;
            if (id == static_cast<u32>(kFreeId) && !world.all_orders.count(rec[2])) continue;  // vectors and the like
            if (id == static_cast<u32>(kUnderAttackId) || id == static_cast<u32>(kSpeechId)) rec[4] = 0;
            out.insert(out.end(), rec.begin(), rec.end());
        }
        return out;
    };
    auto link_value = [&](const sim::UnitLink* l) -> u32 { return l ? l->owner->address + oLink : 0u; };
    auto ids_of = [&](u32 begin, u32 end) {
        std::vector<u32> v;
        if (begin && end > begin && (end - begin) / 4 < 1000) {
            v.resize((end - begin) / 4);
            r.read(begin, v.data(), v.size() * 4);
            for (auto& w : v) norm_ptr(w);
        }
        return v;
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
        for (auto& w : world.log) norm_ptr(w);
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
        if (r.r32(kBattleRngState) != world.rng.state()) bad("battle RNG state");
        // Unit tables and controllers.
        for (int k = 0; k < 10; ++k) {
            const sim::UnitTable& tb = tstate_t.tables[static_cast<std::size_t>(k)];
            const u32 ta = table_addr[static_cast<std::size_t>(k)];
            if (!tb.exists) continue;
            std::vector<u32> seen, radar;
            for (const sim::Unit* u : tb.seen) seen.push_back(0x40000000u | u->id);
            for (const sim::Unit* u : tb.radar) radar.push_back(0x40000000u | u->id);
            if (ids_of(r.r32(ta + tSeenBegin), r.r32(ta + tSeenEnd)) != seen) bad("table " + std::to_string(k) + " sight list");
            if (ids_of(r.r32(ta + tRadarBegin), r.r32(ta + tRadarEnd)) != radar) bad("table " + std::to_string(k) + " radar list");
            if ((r.r32(ta + tUpgrade) != 0) != tb.targeting_upgrade) bad("table " + std::to_string(k) + " targeting upgrade");
            if (r.r32(ta + tRefresh) != tb.refresh_tick) bad("table " + std::to_string(k) + " refresh tick");
        }
        for (int p = 0; p < 3; ++p) {
            const sim::PlayerController& c = tstate_t.controllers[static_cast<std::size_t>(p)];
            const u32 ca = controller_addr[static_cast<std::size_t>(p)];
            const u32 cur = r.r32(ca + cCursor);
            const u32 mine_cur = c.cursor ? unit_addr(c.cursor) : 0u;
            if (cur != mine_cur) bad("controller " + std::to_string(p) + " cursor");
            if (r.r32(ca + cHold) != c.ai_hold_tick) bad("controller " + std::to_string(p) + " AI hold tick");
        }
        for (int i : present) {
            sim::Unit& u = units[static_cast<std::size_t>(i)];
            const u32 at = unit_addr(i);
            std::vector<std::uint8_t> ru(kUnitSize);
            r.read(at, ru.data(), ru.size());
            auto f8 = [&](u32 off) { return ru[off]; };
            auto f16 = [&](u32 off) { std::uint16_t v; std::memcpy(&v, &ru[off], 2); return v; };
            auto f32 = [&](u32 off) { u32 v; std::memcpy(&v, &ru[off], 4); return v; };
            const std::string un = "unit " + std::to_string(i) + " ";
            if (f32(uFlags) != u.flags) bad(un + "flags");
            if (f16(uNetDirty) != u.net_dirty) bad(un + "pending events");
            if (f8(uActivate) != u.activate_flags) bad(un + "activate flags");
            for (int s = 0; s < 3; ++s) {
                const sim::WeaponSlot& ws = u.weapons[static_cast<std::size_t>(s)];
                const u32 so = 4 + static_cast<u32>(s) * 0x1C;
                if (static_cast<std::int16_t>(f16(so)) != ws.target_x || static_cast<std::int16_t>(f16(so + 2)) != ws.target_z ||
                    f8(so + 0x1B) != ws.flags)
                    bad(un + "weapon slot " + std::to_string(s));
            }
            for (int list = 0; list < 2; ++list) {
                u32 ra = f32(list ? uSecondary : uOrders);
                const sim::MissionOrder* o = list ? u.secondary_orders : u.orders;
                for (int n = 0; (ra || o) && n < 1000; ++n) {
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
                    if (o32(oWait) != o->wait || o32(oWake) != o->wake_tick || o32(oEvents) != o->events) {
                        char b[160];
                        std::snprintf(b, sizeof b, "wait/wake/events orig %08x %08x %08x ours %08x %08x %08x (kind %u)", o32(oWait), o32(oWake),
                                      o32(oEvents), o->wait, o->wake_tick, o->events, o->kind);
                        bad(on + b);
                    }
                    if (ounit != (o->unit ? World::tok(*o->unit) : 0u)) bad(on + "unit");
                    if (tgt != (o->target.unit ? World::tok(*o->target.unit) : 0u) || o32(oLink + 8) != link_value(o->target.next))
                        bad(on + "target link");
                    for (int k = 0; k < 3; ++k)
                        if (static_cast<i32>(o32(oPos + 4u * static_cast<u32>(k))) != o->pos[static_cast<std::size_t>(k)]) bad(on + "position");
                    if (o32(oScratch) != o->scratch0 || o32(oScratch + 4) != o->scratch1) bad(on + "scratch");
                    if (static_cast<i32>(o32(oParams)) != o->param0 || static_cast<i32>(o32(oParams + 4)) != o->param1 ||
                        static_cast<i32>(o32(oParams + 8)) != o->param2)
                        bad(on + "params");
                    if (o32(oFlags) != o->flags) bad(on + "flags");
                    if (o32(oCreated) != o->created_tick) bad(on + "created");
                    ra = o32(oNext);
                    o = o->next;
                }
            }
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
        }
        if (!ok) ++st.mismatches;
        return ok;
    };
    auto engine_faulted = [&]() {
        std::string f = !targeting.fault.empty() ? targeting.fault : missions.fault;
        if (f.empty()) return false;
        ++st.faults;
        std::printf("scenario %u: stopped, the original would fail here: %s\n", seed, f.c_str());
        return true;
    };
    auto fail = [&](const std::string& what) {
        std::printf("scenario %u: the original failed during %s\n", seed, what.c_str());
        return false;
    };
    auto any_unit = [&]() { return present[g() % present.size()]; };
    auto is_alive = [&](int i) { return tstate[static_cast<std::size_t>(i)].type_index != 0; };
    // Answers for the stubs (same lists on both sides).
    auto deal = [&]() {
        for (int q = 0; q < kQueues; ++q) {
            std::vector<u32>& v = world.queue[static_cast<std::size_t>(q)];
            v.clear();
            world.queue_at[static_cast<std::size_t>(q)] = 0;
            const int answers = q == qVisible ? 64 : static_cast<int>(g() % 8);
            const u32 bias = g() % 4;
            for (int k = 0; k < answers; ++k)
                v.push_back(q == qVisible ? (g() % 4 < bias ? 0u : 1u) : (g() % 3 == 0 ? 0u : L.shots + (g() % 8) * 0x40));
            const u32 qa = L.queues + static_cast<u32>(q) * kQueueSize;
            std::vector<u32> img = {qa + 8, qa + 8 + static_cast<u32>(v.size()) * 4};
            img.insert(img.end(), v.begin(), v.end());
            r.write(qa, img.data(), img.size() * 4);
        }
        world.next_address = r.r32(L.bump_ptr);
    };
    auto set_env = [&](int step) {
        tenv.game_tick = g() % 4 == 0 ? static_cast<u32>(g() % 0x70000000u) : static_cast<u32>(1000 + step * 7);
        tenv.sea_level = static_cast<u8>(g() % 3 == 0 ? g() : g() % 40);
        tenv.local_player = static_cast<u8>(g() % 4);
        tenv.unit_limit = static_cast<u16>(g() % 4 == 0 ? g() : g() % 300);
        tenv.target_everything = g() % 4 == 0;
        tenv.command_mode = g() % 3 == 0 ? 1 : static_cast<i32>(g() % 3);
        world.local_view = static_cast<u8>(g() % 16);
        octx.game_tick = tenv.game_tick;
        menv.sea_level = tenv.sea_level;
        r.w32(L.gs + gTick, tenv.game_tick);
        r.write(L.gs + gSea, &tenv.sea_level, 1);
        r.write(L.gs + gLocalPlayer, &tenv.local_player, 1);
        r.write(L.gs + gLocalView, &world.local_view, 1);
        r.write(L.gs + gUnitLimit, &tenv.unit_limit, 2);
        const u8 opt = static_cast<u8>((g() & 0xFB) | (tenv.target_everything ? 4 : 0));
        r.write(L.gs + gOptions, &opt, 1);
        r.w32(L.gs + gCommandMode, static_cast<u32>(tenv.command_mode));
    };
    auto random_pos = [&]() -> std::array<i32, 3> {
        if (g() % 3 == 0 && !feature_cells.empty()) {  // on a feature cell
            const std::size_t c = feature_cells[g() % feature_cells.size()];
            const i32 cx = static_cast<i32>(c % static_cast<std::size_t>(map.width)), cz = static_cast<i32>(c / static_cast<std::size_t>(map.width));
            return {static_cast<i32>((static_cast<u32>(cx) << 20) | (g() & 0xFFFFF)), static_cast<i32>((g() % 100) << 16),
                    static_cast<i32>((static_cast<u32>(cz) << 20) | (g() & 0xFFFFF))};
        }
        if (g() % 8 == 0) return {static_cast<i32>(g()), static_cast<i32>(g()), static_cast<i32>(g())};
        return {static_cast<i32>((128 + g() % 2048) << 16), static_cast<i32>((g() % 100) << 16), static_cast<i32>((128 + g() % 2048) << 16)};
    };
    auto sync_unit = [&](int i) {  // after a tweak: write our unit's fields to the original
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        const sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
        const u32 at = unit_addr(i);
        r.w32(at + uFlags, u.flags);
        r.write(at + uActivate, &u.activate_flags, 1);
        r.write(at + uBuildLeft, &u.build_fraction_left, 4);
        r.write(at + uHp, &u.hit_points, 2);
        r.write(at + uPosX, &u.pos[0], 4);
        r.write(at + uPosZ, &u.pos[2], 4);
        r.write(at + uLastAttackerSlot, &t.last_attacker_slot, 1);
        r.write(at + uLastDamageKind, &t.last_damage_kind, 1);
        for (int s = 0; s < 3; ++s) {
            const sim::WeaponSlot& ws = u.weapons[static_cast<std::size_t>(s)];
            const u32 sa = at + 4 + static_cast<u32>(s) * 0x1C;
            r.write(sa + 0x00, &ws.target_x, 2);
            r.write(sa + 0x02, &ws.target_z, 2);
            r.write(sa + 0x1B, &ws.flags, 1);
        }
    };
    auto kill = [&](int i) {  // an empty table entry from now on (type index 0, alive bit off)
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
        t.type_index = 0;
        u.flags &= ~sim::unit_flags::kAlive;
        u.type = &empty_type;
        t.tick_type = &empty_tick;
        const u32 at = unit_addr(i);
        r.write(at + uDefIndex, &t.type_index, 2);
        r.w32(at + uFlags, u.flags);
        r.w32(at + uDef, L.empty_def);
    };

    set_env(0);
    for (int i : present) {
        r.write(L.names, "Create", 7);
        if (!r.call(kCallNamed, remote_script[static_cast<std::size_t>(i)], {L.names, 0, 1})) return fail("Create");
        units[static_cast<std::size_t>(i)].script->call("Create", nullptr, true);
    }
    // A few orders (some on a target, some waiting for their destruction).
    for (int i : present) {
        const int n = static_cast<int>(g() % 3);
        for (int k = 0; k < n; ++k) {
            deal();
            const auto kind = static_cast<std::uint8_t>(g() % sim::mission::kCount);
            const int ti = g() % 2 ? any_unit() : 0;
            sim::MissionOrder* o = world.allocate_order();
            sim::construct_order(octx, *o, kind, ti ? &units[static_cast<std::size_t>(ti)] : nullptr, nullptr, 0, 0, 0);
            u32 mem = 0;
            if (!r.call(kAlloc, 0, {0x56}, &mem) || mem != o->address) return fail("allocation");
            if (!r.call(kConstruct, mem, {kind, ti ? unit_addr(ti) : 0u, 0u, 0u, 0u, 0u})) return fail("constructor");
            if (g() % 3 == 0) {
                o->wait = 2;
                r.w32(mem + oWait, o->wait);
            }
            if (g() % 3 == 0) {
                o->flags ^= g() % 2 ? 0x20000u : 0x80u;
                if (g() % 3 == 0) o->flags |= sim::order_flags::kKeep;
                r.w32(mem + oFlags, o->flags);
            }
            sim::prepend_order(units[static_cast<std::size_t>(i)], *o);
            if (!r.call(kPrepend, 0, {unit_addr(i), mem})) return fail("prepend");
        }
    }
    if (engine_faulted()) return true;
    if (!compare("after setup")) return true;

    for (int step = 0; step < steps; ++step) {
        set_env(step);
        deal();
        const int i = any_unit();
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        // Operation mix (by the op numbers below): refresh x3, enemies, best target x2, fire at will,
        // attack order, resolution x2, retargeting x3, weapon-target pass x3, tweaks x2, a death.
        static constexpr int kMix[] = {0, 0, 0, 1, 2, 2, 3, 4, 5, 6, 7, 7, 7, 8, 8, 8, 9, 9, 10};
        const int op = kMix[g() % (sizeof kMix / sizeof kMix[0])];
        std::string what;
        u32 result = 0;
        if (op == 0) {  // unit table refresh (often due)
            const int k = static_cast<int>(g() % 4);
            sim::UnitTable& tb = tstate_t.tables[static_cast<std::size_t>(k)];
            if (tb.exists && g() % 4) {
                tb.refresh_tick = tenv.game_tick - 30 - g() % 3 + (g() % 6 == 0 ? 2 : 0);
                r.w32(table_addr[static_cast<std::size_t>(k)] + tRefresh, tb.refresh_tick);
            }
            sim::refresh_unit_table(targeting, k);
            if (engine_faulted()) return true;
            if (!r.call(kRefresh, 0, {static_cast<u32>(k)})) return fail("refresh");
            ++st.refreshes;
            what = "refresh";
        } else if (op == 1) {  // enemy listing
            const int k = static_cast<int>(g() % 4);
            if (!tstate_t.tables[static_cast<std::size_t>(k)].exists) continue;
            const auto pos = random_pos();
            const i32 radius = static_cast<i32>(g() % 6 == 0 ? g() : g() % 1500);
            std::vector<sim::Unit*> out;
            sim::enemies_in_radius(targeting, k, pos, radius, out);
            if (engine_faulted()) return true;
            r.write(L.out, pos.data(), 12);
            r.write(L.out + 0x20, zero.data(), 16);
            if (!r.call(kEnemies, 0, {static_cast<u32>(k), L.out, static_cast<u32>(radius), 0u, L.out + 0x20})) return fail("enemies");
            std::vector<u32> mine;
            for (const sim::Unit* x : out) mine.push_back(0x40000000u | x->id);
            if (ids_of(r.r32(L.out + 0x24), r.r32(L.out + 0x28)) != mine) {
                std::printf("scenario %u step %d: enemy list differs\n", seed, step);
                ++st.mismatches;
                return true;
            }
            st.listed += out.size();
            what = "enemies in radius";
        } else if (op == 2 || op == 3) {  // best target / fire at will
            sim::Unit* t = nullptr;
            if (op == 2) {
                const int slot = static_cast<int>(g() % 3);
                const bool weapon_range = g() % 2;
                t = sim::find_best_target(targeting, u, slot, weapon_range);
                if (engine_faulted()) return true;
                if (!r.call(kBest, 0, {unit_addr(i), static_cast<u32>(slot), weapon_range ? 1u : 0u}, &result)) return fail("best target");
                what = "best target";
            } else {
                t = sim::fire_at_will_target(targeting, u);
                if (engine_faulted()) return true;
                if (!r.call(kFireAtWill, 0, {unit_addr(i)}, &result)) return fail("fire at will");
                what = "fire at will";
            }
            if (result != (t ? unit_addr(t->id) : 0u)) {
                std::printf("scenario %u step %d: %s result differs (orig %08x ours %08x)\n", seed, step, what.c_str(), result,
                            t ? unit_addr(t->id) : 0u);
                ++st.mismatches;
                return true;
            }
            st.targets_found += t != nullptr;
        } else if (op == 4) {  // attack order
            const int ti = any_unit();
            if (!is_alive(ti) || !is_alive(i)) continue;
            const bool forced = g() % 4 == 0;
            const bool ok = sim::issue_attack_order(targeting, u, units[static_cast<std::size_t>(ti)], forced);
            if (engine_faulted()) return true;
            if (!r.call(kIssueAttack, 0, {unit_addr(i), unit_addr(ti), forced ? 1u : 0u}, &result)) return fail("issue attack");
            if ((result != 0) != ok) {
                std::printf("scenario %u step %d: attack order result differs\n", seed, step);
                ++st.mismatches;
                return true;
            }
            st.attack_orders += ok;
            what = "issue attack";
        } else if (op == 5 || op == 6) {  // mission resolution
            const u32 intents[] = {0, 1, 1, 1, 2, 2, 3, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 12, 13, 14, 15};
            const u8 intent = static_cast<u8>(intents[g() % (sizeof intents / sizeof intents[0])]);
            const int ti = g() % 4 == 0 ? 0 : any_unit();
            const bool with_pos = intent == 12 ? g() % 20 != 0 : g() % 2;
            const auto pos = random_pos();
            const u8 kind = sim::resolve_mission(targeting, intent, u, ti ? &units[static_cast<std::size_t>(ti)] : nullptr, with_pos ? &pos : nullptr);
            if (engine_faulted()) return true;
            r.write(L.out, pos.data(), 12);
            r.w32(L.out + 0x40, 0xCCCCCCCCu);
            if (!r.call(kResolve, 0, {L.out + 0x40, intent, unit_addr(i), ti ? unit_addr(ti) : 0u, with_pos ? L.out : 0u})) return fail("resolve");
            const u8 theirs = static_cast<u8>(r.r32(L.out + 0x40));
            if (theirs != kind) {
                std::printf("scenario %u step %d: resolved kind differs for intent %u (orig %u ours %u)\n", seed, step, intent, theirs, kind);
                ++st.mismatches;
                return true;
            }
            st.resolved_nonzero += kind != 0;
            ++st.intents[intent];
            ++st.resolved_kinds[kind];
            what = "resolve";
        } else if (op == 7) {  // retargeting (the victim is alive and not dying, as the damage routine requires)
            if (!is_alive(i) || (u.flags & 0x4000u)) continue;
            const int ai = g() % 5 == 0 ? 0 : any_unit();
            sim::Unit* attacker = ai ? &units[static_cast<std::size_t>(ai)] : nullptr;
            std::array<std::pair<std::int16_t, std::int16_t>, 3> before{};
            for (int s = 0; s < 3; ++s) before[static_cast<std::size_t>(s)] = {u.weapons[static_cast<std::size_t>(s)].target_x, u.weapons[static_cast<std::size_t>(s)].target_z};
            sim::retarget_attack(targeting, attacker, u);
            if (engine_faulted()) return true;
            if (!r.call(kRetarget, 0, {ai ? unit_addr(ai) : 0u, unit_addr(i), static_cast<u32>(g() % 500)})) return fail("retarget");
            for (int s = 0; s < 3; ++s)
                if (before[static_cast<std::size_t>(s)] != std::make_pair(u.weapons[static_cast<std::size_t>(s)].target_x, u.weapons[static_cast<std::size_t>(s)].target_z))
                    ++st.retargets_aimed;
            what = "retarget";
        } else if (op == 8) {  // weapon-target pass
            const int p = static_cast<int>(g() % 3);
            const bool ai = g() % 3 == 0;
            std::vector<std::pair<std::int16_t, std::int16_t>> before;
            for (const sim::Unit& x : units)
                for (const sim::WeaponSlot& ws : x.weapons) before.emplace_back(ws.target_x, ws.target_z);
            sim::weapon_target_pass(targeting, p, ai);
            if (engine_faulted()) return true;
            std::size_t n = 0;
            for (const sim::Unit& x : units)
                for (const sim::WeaponSlot& ws : x.weapons) st.pass_changes += before[n++] != std::make_pair(ws.target_x, ws.target_z);
            if (!r.call(kWeaponPass, controller_addr[static_cast<std::size_t>(p)], {ai ? 1u : 0u})) return fail("weapon pass");
            what = "weapon-target pass";
        } else if (op == 9) {  // tweaks
            u.flags ^= (g() % 2 ? 0x300000u : 0xC0000u) & static_cast<u32>(g());
            if (g() % 4 == 0) u.flags ^= 0x100u;
            if (g() % 6 == 0) u.flags ^= 0x8000u;
            if (g() % 8 == 0) u.flags ^= 0x4000u;
            if (g() % 4 == 0) u.activate_flags ^= g() % 2 ? 0x10u : 1u;
            if (g() % 6 == 0) u.build_fraction_left = u.build_fraction_left == 0.0f ? 0.25f : 0.0f;
            if (g() % 3 == 0) u.pos = {static_cast<i32>((128 + g() % 2048) << 16), u.pos[1], static_cast<i32>((128 + g() % 2048) << 16)};
            sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
            t.last_attacker_slot = static_cast<u8>(g() % 11);
            t.last_damage_kind = static_cast<u8>(g() % 3);
            for (int s = 0; s < 3; ++s)
                if (g() % 3 == 0) {
                    sim::WeaponSlot& ws = u.weapons[static_cast<std::size_t>(s)];
                    ws.flags ^= static_cast<u8>(g() % 2 ? 0x10 : 0x02);
                    if (g() % 2) {
                        ws.target_x = static_cast<std::int16_t>(g() % 3 ? 1 + g() % kMaxUnits : 0);
                        ws.target_z = sim::kUnitTarget;
                    }
                }
            sync_unit(i);
            what = "tweak";
        } else {  // a unit dies (its entry stays in the table)
            if (g() % 4 || !is_alive(i) || present.size() < 4) continue;
            kill(i);
            what = "unit removed";
        }
        ++st.ops;
        ++st.by_op[what];
        if (!compare(what + " on unit " + std::to_string(i) + " step " + std::to_string(step))) return true;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: targeting_harness <TotalA.exe> <game_dir> [scenarios] [steps] [first scenario]\n");
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
    std::vector<std::uint8_t> tnt;
    sim::HeightMap hm;
    if (!vfs.read("maps\\Moon Quartet.tnt", tnt) || !ta::tnt_load_heights(tnt, hm)) {
        std::fprintf(stderr, "cannot load the Moon Quartet heights\n");
        return 1;
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
        if (!run_scenario(r, L, types, hm, steps, static_cast<unsigned>(s * 7 + 13), st)) {
            ++st.failures;
            std::printf("scenario %d: original call failed - restarting\n", s);
            r.stop();
            if (!r.start(argv[1]) || !setup(r, L, hm, report)) return 1;
        }
    }
    r.stop();
    std::printf("scenarios %zu, operations %zu\n", st.scenarios, st.ops);
    for (const auto& [k, v] : st.by_op) std::printf("  %s: %zu\n", k.c_str(), v);
    std::printf("targets found %zu, units listed %zu, attack orders %zu, refreshes %zu, retargets that changed a slot %zu, "
                "weapon-target pass slot changes %zu, nonzero resolutions %zu\n",
                st.targets_found, st.listed, st.attack_orders, st.refreshes, st.retargets_aimed, st.pass_changes, st.resolved_nonzero);
    std::printf("interceptor searches %zu (%zu found a shot)\nresolutions by intent:", g_intercepts, g_intercept_hits);
    for (const auto& [k, v] : st.intents) std::printf(" %d:%zu", k, v);
    std::printf("\nresolved kinds:");
    for (const auto& [k, v] : st.resolved_kinds) std::printf(" %d:%zu", k, v);
    std::printf("\nmismatching scenarios %zu, engine faults %zu, original failures %zu\n", st.mismatches, st.faults, st.failures);
    return st.mismatches || st.failures ? 1 : 0;
}
