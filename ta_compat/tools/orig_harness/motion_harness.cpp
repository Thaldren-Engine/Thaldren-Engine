// Differential tester for unit motion (sim/motion.h). Inside a suspended
// TotalA.exe it builds a small battle like orders_harness (three players,
// units of random real types with scripts, object states, models and the Moon
// Quartet terrain), gives every unit a motion object with the original
// constructor, and fake mission orders that receive order goals made by the
// original constructors (point, follow, dock, face, manoeuvre, circle, ring,
// edge). Each step applies random operations: attach/replace/detach goals,
// goal setters, route answers and route requests, goal queries, layer changes,
// motion and unit state changes, direct calls of the motion step and its parts
// (acceleration, lean, placement, move-rate and water scripts), then one unit
// pass with the real motion step and ground settle. The occupancy grid, sight,
// the route search, orders, weapons and damage are logging stubs. After every
// operation it compares the stub log, units, motion objects, motion goals,
// order goals, orders' events, unit link lists, scripts and the stale stack word.
//
// Usage: motion_harness <TotalA.exe> <game_dir> [scenarios] [ticks] [first seed]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/ground_settle.h"
#include "sim/motion.h"
#include "sim/orders.h"
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
using Vec3 = std::array<i32, 3>;

constexpr u32 kGameStatePtr = 0x00511DE8, kBattleRngState = 0x0051FC88;
constexpr u32 kRelocate = 0x004CB4C0, kNegateXZ = 0x004CB590, kBuildState = 0x0045A950;
constexpr u32 kCallNamed = 0x004B0940, kUnitPass = 0x0048AD30, kAlloc = 0x004B4F10, kClock = 0x004B6340;
constexpr u32 kMotionCtor = 0x0043DC00, kMotionTick = 0x0043DD20, kAccelerate = 0x0043CC20, kLean = 0x0043D0D0,
              kSetLayer = 0x0043D210, kMoveUnit = 0x0043D6D0, kMoveRate = 0x0043DA70, kWaterOcc = 0x0043DB50,
              kPlace = 0x0048A9F0, kSteerGround = 0x0043CD20, kSteerAir = 0x0043D290;
constexpr u32 kAttachGoal = 0x004388D0, kAttachCircle = 0x00438930, kAttachRing = 0x00438A00, kOrderDestroy = 0x0043A1F0;
constexpr u32 kPointCtor = 0x0044E2D0, kFollowCtor = 0x0044E190, kDockCtor = 0x0044E250, kFaceCtor = 0x0044E330,
              kManoeuvreCtor = 0x0044E740, kEdgeCtor = 0x0044D8A0;
constexpr u32 kSetAltitude = 0x0044E6C0, kSetHeading = 0x0044E720, kSetRadius = 0x0044E730, kSetTargetHeading = 0x0044EC20;
constexpr u32 kSetRoute = 0x0044F080;
constexpr u32 kUnitVtable = 0x004FD698;
constexpr u32 kUnitSize = 0x118, kPlayerSize = 0x14B, kDefSize = 0x249;
constexpr int kMaxUnits = 16;

enum class StubRet { Zero, Variable, Silent };
struct Stub {
    u32 address;
    int args;
    int id;
    int extra = 0;
    StubRet ret = StubRet::Zero;
};
const Stub kStubs[] = {
    {0x0049E1A0, 1, 1},   // weapons(unit)
    {0x0043B7C0, 1, 2},   // primary orders(unit)
    {0x0043BAD0, 1, 3},   // secondary orders(unit)
    {0x00489BB0, 5, 5},   // apply damage(attacker, target, amount, kind, heading)
    {0x0041BD10, 3, 6},   // repair/heal(worker, target, rate)
    {0x0048B710, 1, 9},   // broadcast player units(player)
    {0x00480EB0, 2, 10},  // script emit sfx(piece, type) [thiscall]
    {0x00481140, 2, 11},  // script explode(piece, type)
    {0x00481340, 3, 12},  // script attach(unit, piece, mode)
    {0x004813B0, 1, 13},  // script drop(unit)
    {0x0047F780, 3, 14},  // speech(unit, event, 0)
    {0x0041C110, 1, 15},  // HUD dirty(unit)
    {0x00451DF0, 3, 16, 3},  // network send(net id, packet, size) + the first 12 packet bytes
    {0x0047DAC0, 2, 17},  // yard open(unit, open)
    {0x0047DB70, 4, 20, 0, StubRet::Variable},  // can place footprint(def, id, cell, layer)
    {0x0047D0E0, 1, 21},  // clear footprint(unit)
    {0x0047CC30, 1, 22},  // claim footprint / spatial link(unit)
    {0x004827B0, 1, 23},  // sight update(unit)
    {0x0040E9C0, 1, 24},  // route search: forget goal(goal) [thiscall]
    {0x004864B0, 2, 25},  // kill(unit, kind) (never expected)
    {0x004B4F20, 1, 53},  // free(block) [cdecl]
};

std::size_t record_size(u32 id) {
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

void emit_stub(Emitter& e, const Stub& s, u32 log_ptr_addr, u32 ret_addr) {
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
    } else {
        e.u8(0x33); e.u8(0xC0);
    }
    if (cdecl_ret || s.args == 0) {
        e.u8(0xC3);
    } else {
        e.u8(0xC2); e.u8(static_cast<std::uint8_t>(4 * s.args)); e.u8(0x00);
    }
}

struct Layout {
    u32 heap = 0, scenario_heap = 0, bump_ptr = 0, stubs = 0, log_ptr = 0, log_base = 0, ret_val = 0, gs = 0, units = 0, defs = 0,
        models = 0, cells = 0, schema = 0, names = 0, files = 0, pinfo = 0, empty_def = 0, buckets = 0, weapons = 0, search = 0,
        clock = 0, caller_pass = 0, caller_motion = 0, args = 0;
};
// ret_val block: +0 can-place result, +0x10 stale in, +0x14 stale out, +0x18 EBX for the motion call,
// +0x1C motion object, +0x20 unit, +0x40 clock.
constexpr u32 kStaleIn = 0x10, kStaleOut = 0x14, kEbx = 0x18, kMotionArg = 0x1C, kUnitArg = 0x20;

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
    L.buckets = r.alloc(0x1000);
    L.weapons = r.alloc(0x1000);
    L.search = r.alloc(0x1000);
    L.args = r.alloc(0x1000);
    if (!L.heap || !L.stubs || !L.log_base || !L.gs || !L.units || !L.cells || !L.files || !L.pinfo || !L.empty_def || !L.buckets ||
        !L.weapons || !L.search || !L.args)
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
        emit_stub(e, s, L.log_ptr, L.ret_val);
        e.align();
        if (!r.patch_jump(s.address, at)) return false;
    }
    {  // real-time clock: returns the counter, then advances it by 7
        const u32 at = here();
        e.u8(0xA1); e.u32le(L.clock);
        e.u8(0x83); e.u8(0x05); e.u32le(L.clock); e.u8(0x07);
        e.u8(0xC3);
        e.align();
        if (!r.patch_jump(kClock, at)) return false;
    }
    const u32 in = L.ret_val + kStaleIn, out = L.ret_val + kStaleOut;
    auto call_rel = [&](u32 target) {
        const u32 next = here() + 5;
        e.u8(0xE8); e.u32le(target - next);
    };
    // The unit pass, with the stale stack word preset and saved.
    L.caller_pass = here();
    e.u8(0xA1); e.u32le(in);
    e.bytes({0x89, 0x44, 0x24, 0xC0});  // mov [esp-0x40],eax
    call_rel(kUnitPass);
    e.bytes({0x8B, 0x44, 0x24, 0xC0});
    e.u8(0xA3); e.u32le(out);
    e.u8(0xC3);
    e.align();
    // The motion step called like the pass does (ECX = motion object, EBX = the pass's EBX).
    L.caller_motion = here();
    e.u8(0x53);                                              // push ebx
    e.u8(0xA1); e.u32le(in);
    e.bytes({0x89, 0x44, 0x24, 0xE4});                       // mov [esp-0x1C],eax
    e.u8(0x8B); e.u8(0x1D); e.u32le(L.ret_val + kEbx);       // mov ebx,[ebx value]
    e.u8(0x8B); e.u8(0x0D); e.u32le(L.ret_val + kMotionArg); // mov ecx,[motion]
    e.u8(0xFF); e.u8(0x35); e.u32le(L.ret_val + kUnitArg);   // push [unit]
    call_rel(kMotionTick);
    e.bytes({0x8B, 0x44, 0x24, 0xE4});
    e.u8(0xA3); e.u32le(out);
    e.u8(0x5B);                                              // pop ebx
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
    r.w32(L.gs + 0x1439B, L.empty_def);
    r.w32(L.gs + 0x142B7, L.buckets);  // the off-map bucket is the first one
    r.w32(L.gs + 0x14207, L.search);
    std::vector<std::uint8_t> cells(static_cast<std::size_t>(hm.width()) * hm.height() * 13, 0);
    for (i32 z = 0; z < hm.height(); ++z)
        for (i32 x = 0; x < hm.width(); ++x) cells[(static_cast<std::size_t>(z) * hm.width() + x) * 13 + 4] = hm.at(x, z);
    if (!r.write(L.cells, cells.data(), cells.size())) return false;
    r.w32(L.log_ptr, L.log_base);
    L.scenario_heap = (r.r32(L.bump_ptr) + 0xFFF) & ~0xFFFu;
    return true;
}

// Remote unit / player / def / motion offsets.
enum : u32 {
    uMotion = 0x00, uOrders = 0x5C, uPitch = 0x64, uHeading = 0x66, uBank = 0x68, uPosX = 0x6A, uPosY = 0x6E, uPosZ = 0x72, uCell = 0x76,
    uFootprint = 0x7E, uBucket = 0x82, uCarrier = 0x86, uDef = 0x92, uOwner = 0x96, uScript = 0x9A, uState = 0x9E, uReferrers = 0xA2,
    uDefIndex = 0xA6, uId = 0xA8, uHoverPhase = 0xAA, uNetDirty = 0xBA, uResourcePlayer = 0xEC, uLastAttackerSlot = 0xF4,
    uHealth = 0xF6, uPrevHealth = 0xF7, uTransportPiece = 0xF9, uOwnerSlot = 0xFF, uBuildLeft = 0x104, uHp = 0x108,
    uWaterOcc = 0x10A, uActivate = 0x10E, uScriptFlags = 0x10F, uFlags = 0x110,
    pActive = 0x00, pNetId = 0x04, pInfo = 0x27, pFirst = 0x67, pLast = 0x6B, pController = 0x73, pAlive = 0x144, pState = 0x146,
    dName = 0x20, dModelMaxY = 0x16E, dCob = 0x18E, dMaxVelocity = 0x192, dBrake = 0x19A, dAccel = 0x19E, dBankScale = 0x1A2,
    dPitchScale = 0x1A6, dMoveRate1 = 0x1AE, dMoveRate2 = 0x1B2, dMoveClass = 0x1B6, dTurnRate = 0x1BA, dMaxDamage = 0x1FA,
    dHealTime = 0x200, dCruise = 0x21C, dWaterline = 0x22C, dDefaultMission = 0x230, dTypeFlags = 0x241,
    mGoal = 0x00, mVel = 0x08, mTilt = 0x14, mSpeed = 0x20, mTurn = 0x24, mClaim = 0x26, mMove = 0x2A, mFlags = 0x2E,
    oUnit = 0x0E, oFlags = 0x42, oEvents = 0x4E, oAttached = 0x52,
};

struct LoadedType {
    const ta::UnitDefRecord* def = nullptr;
    std::vector<std::uint8_t> model_file, cob_file;
    sim::PoseModel pose_model;
    ta::CobFile cob;
    sim::UnitTypeInfo info;
    sim::UnitTickTypeInfo tick_info;
    sim::GroundSettleType settle;
    sim::MotionType motion;
    u32 remote_model = 0, remote_cob = 0, remote_def = 0;
};

u32 fbits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

struct Order {
    sim::MissionOrder o;
};

struct World : sim::UnitWorld, sim::UnitTickSystems, sim::GroundSettleSystems, sim::MotionSystems {
    std::vector<sim::Unit>* units = nullptr;
    std::vector<sim::UnitTickState>* ticks = nullptr;
    std::vector<sim::PlayerSlot>* players = nullptr;
    sim::UnitTickEnvironment* env = nullptr;
    std::vector<sim::GroundSettleType>* settle_types = nullptr;
    sim::MotionContext* mc = nullptr;
    const sim::HeightMap* hm = nullptr;
    std::vector<u32> log;
    std::vector<bool> off_map_units;
    std::vector<std::uint8_t> bucket_heights;
    u32 next_address = 0, clock = 0, place_result = 1;
    sim::BattleRng rng;

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
        rec(5, 0, {a ? tok(*a) : 0u, tok(t), static_cast<u32>(amount), static_cast<u32>(kind), static_cast<u32>(heading)});
    }
    void repair_heal(sim::Unit& w, sim::Unit& t, float rate) override { rec(6, 0, {tok(w), tok(t), fbits(rate)}); }
    void primary_orders(sim::Unit& u, u32&) override { rec(2, 0, {tok(u)}); }
    void secondary_orders(sim::Unit& u, u32&) override { rec(3, 0, {tok(u)}); }
    std::size_t pass_steps = 0, routed = 0, flying = 0, carried = 0, place_asks = 0, refused = 0, reached = 0, rate_changes = 0;
    void count_step(sim::Unit& u) {
        const sim::UnitTickState& t = (*ticks)[u.id];
        ++pass_steps;
        if (u.carrier) ++carried;
        else if ((mc->type(u).type_flags & sim::motion_flags::kTypeFlies) && (t.motion.flags & 3) == 2) ++flying;
        else if (t.motion.goal->has_route()) ++routed;
    }
    void motion(sim::Unit& u, u32& stale) override {
        count_step(u);
        const u32 rate = u.flags & 0xC;
        sim::run_motion(*mc, u);
        if ((u.flags & 0xC) != rate) ++rate_changes;
        stale = sim::motion_stale_word(*env);
    }
    void settle_height(sim::Unit& u, u32& stale) override {
        const sim::UnitTickState& t = (*ticks)[u.id];
        sim::settle_on_ground(u, t.has_motion, (*settle_types)[t.type_index], env->sea_level, env->game_tick, *hm, *this, stale);
    }
    void killed(sim::Unit& u, std::uint8_t kind, u32&) override { rec(25, 0, {tok(u), kind}); }
    void broadcast_player_units(int player, u32&) override { rec(9, 0, {0x20000000u | static_cast<u32>(player)}); }
    void camera_follow_step(u32&) override {}
    // GroundSettleSystems
    std::int32_t motion_speed(sim::Unit& u) override { return (*ticks)[u.id].motion.speed; }
    std::uint32_t motion_start_tick(sim::Unit& u) override { return (*ticks)[u.id].motion.move_tick; }
    std::uint32_t clock_ticks() override {
        const u32 v = clock;
        clock += 7;
        return v;
    }
    // MotionSystems
    bool can_place(sim::Unit& u, std::int16_t cx, std::int16_t cz, std::uint32_t layer) override {
        rec(20, 0, {0x43000000u | (*ticks)[u.id].type_index, u.id, (static_cast<u32>(static_cast<std::uint16_t>(cx))) | (static_cast<u32>(cz) << 16), layer});
        ++place_asks;
        if (!place_result) ++refused;
        return place_result != 0;
    }
    void clear_footprint(sim::Unit& u) override { rec(21, 0, {tok(u)}); }
    void claim_footprint(sim::Unit& u) override { rec(22, 0, {tok(u)}); }
    void update_sight(sim::Unit& u) override { rec(23, 0, {tok(u)}); }
    bool off_map(const sim::Unit& u) override { return off_map_units[u.id]; }
    std::uint8_t bucket_height(const sim::Unit& u) override { return bucket_heights[u.id]; }
    void route_search_forget(sim::MotionGoal& g) override { rec(24, 0, {g.address}); }
    void set_activated(sim::Unit& u, bool on) override { sim::unit_set_activate(u, *this, sim::unit_flags::kActivated, on); }
    std::uint32_t allocate_object(std::uint32_t size) override {
        const u32 a = next_address;
        next_address += (size + 3) & ~3u;
        return a;
    }
    void free_object(std::uint32_t address) override { rec(53, 0, {address}); }
};

struct Stats {
    std::size_t pass_steps = 0, routed = 0, flying = 0, carried = 0, place_asks = 0, refused = 0, rate_changes = 0, events = 0;
    std::size_t scenarios = 0, ticks = 0, ops = 0, motion_steps = 0, ground_steps = 0, air_steps = 0, mismatches = 0, failures = 0,
                faults = 0;
    std::map<std::string, std::size_t> by_op, pass_stale;
};

bool run_scenario(harness::Remote& r, const Layout& L, std::vector<LoadedType>& types, const sim::HeightMap& hm, int ticks_to_run,
                  unsigned seed, Stats& st) {
    std::mt19937 g(seed);
    r.w32(L.bump_ptr, L.scenario_heap);
    std::vector<std::uint8_t> zero(0x40000, 0);
    r.write(L.gs + 0x1B63, zero.data(), 10 * kPlayerSize);
    r.write(L.units, zero.data(), (kMaxUnits + 2) * kUnitSize);
    r.write(L.scenario_heap, zero.data(), 0x40000);  // the first motion and goal blocks start zeroed
    r.w32(L.gs + 0x14353, 0);

    // Unit types: a mix of ground units, ships, hovercraft and aircraft.
    const int ntypes = 4;
    std::vector<int> pick;
    std::vector<sim::GroundSettleType> settle_types(ntypes + 1);
    std::vector<sim::MotionType> motion_types(ntypes + 1);
    u32 file_at = L.files;
    for (int k = 0; k < ntypes; ++k) {
        std::size_t ti = g() % types.size();
        if (k == 0 || g() % 3 == 0) {  // make sure aircraft appear often
            for (int tries = 0; tries < 50 && !(types[ti].def->type_flags & sim::motion_flags::kTypeFlies); ++tries) ti = g() % types.size();
        }
        while (std::find(pick.begin(), pick.end(), static_cast<int>(ti)) != pick.end()) ti = g() % types.size();
        LoadedType& t = types[ti];
        pick.push_back(static_cast<int>(ti));
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
        // Motion values: real most of the time, varied otherwise (never the values the original divides by zero with).
        sim::MotionType& m = t.motion;
        m.type_flags = t.def->type_flags;
        m.max_velocity = t.def->max_velocity;
        m.brake_rate = t.def->brake_rate;
        m.acceleration = t.def->acceleration;
        m.bank_scale = t.def->bank_scale;
        m.pitch_scale = t.def->pitch_scale;
        m.move_rate1 = t.def->move_rate1;
        m.move_rate2 = t.def->move_rate2;
        m.turn_rate = t.def->turn_rate;
        m.cruise_altitude = t.def->cruise_alt;
        m.waterline = t.def->waterline;
        if (g() % 3 == 0) {
            m.max_velocity = static_cast<i32>(g() % 0x60000);
            m.brake_rate = static_cast<i32>(g() % 0x8000);
            m.acceleration = static_cast<i32>(g() % 0x8000);
            m.turn_rate = static_cast<std::uint16_t>(g() % 3 == 0 ? g() : g() % 2000);
            m.move_rate1 = static_cast<i32>(g() % 0x40000);
            m.move_rate2 = static_cast<i32>(g() % 0x60000);
            m.bank_scale = static_cast<i32>(g() % 0x30000) - 0x8000;
            m.pitch_scale = static_cast<i32>(g() % 0x30000) - 0x8000;
            m.cruise_altitude = static_cast<std::int16_t>(g() % 300);
            m.waterline = static_cast<std::uint8_t>(g() % 40);
            const u32 bits[] = {0x1000, 0x80000, 0x400000, 0x100000};
            m.type_flags ^= bits[g() % 4];
        }
        if (m.max_velocity == 0) m.max_velocity = 0x20000;
        if (m.brake_rate == 0) m.brake_rate = 0x800;
        if (m.turn_rate == 0) m.turn_rate = 0x200;
        m.model_height = static_cast<std::int16_t>(static_cast<u32>(t.def->model_max_y) >> 16);
        t.tick_info.type_flags = m.type_flags;
        t.tick_info.heal_time = 0;
        t.tick_info.default_mission = 0;
        t.settle.type_flags = m.type_flags;
        t.settle.waterline = m.waterline;
        t.settle.max_velocity = m.max_velocity;
        if (t.settle.max_velocity >= -1 && t.settle.max_velocity <= 1) t.settle.max_velocity = 0x20000;
        settle_types[static_cast<std::size_t>(k + 1)] = t.settle;
        motion_types[static_cast<std::size_t>(k + 1)] = m;
        t.remote_def = L.defs + static_cast<u32>(k) * 0x400;
        std::vector<std::uint8_t> d(kDefSize, 0);
        auto put = [&](u32 off, const void* p, std::size_t n) { std::memcpy(&d[off], p, n); };
        put(dName, t.tick_info.name.c_str(), (std::min)(t.tick_info.name.size() + 1, std::size_t{32}));
        put(dCob, &t.remote_cob, 4);
        put(dModelMaxY, &t.info.model_max_y, 4);
        put(dMaxVelocity, &m.max_velocity, 4);
        put(dBrake, &m.brake_rate, 4);
        put(dAccel, &m.acceleration, 4);
        put(dBankScale, &m.bank_scale, 4);
        put(dPitchScale, &m.pitch_scale, 4);
        put(dMoveRate1, &m.move_rate1, 4);
        put(dMoveRate2, &m.move_rate2, 4);
        put(dTurnRate, &m.turn_rate, 2);
        put(dMaxDamage, &t.info.max_damage, 4);
        put(dCruise, &m.cruise_altitude, 2);
        put(dWaterline, &m.waterline, 1);
        put(dTypeFlags, &m.type_flags, 4);
        r.write(t.remote_def, d.data(), d.size());
        r.w32(L.models + static_cast<u32>(k + 1) * 4, t.remote_model);
    }
    std::vector<std::uint8_t> ed(kDefSize, 0);
    r.write(L.empty_def, ed.data(), ed.size());

    // Players: slots 0..2 playing (controllers 1, 2, 1) - the motion step runs only for playing owners.
    std::vector<sim::PlayerSlot> players(10);
    const int per = kMaxUnits / 3;
    for (int p = 0; p < 10; ++p) {
        sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        const bool playing = p < 3;
        s.active = playing;
        s.controller = static_cast<std::uint8_t>(playing ? (p == 1 ? 2 : 1) : 0);
        s.state = static_cast<std::uint8_t>(p);
        s.net_id = p;
        if (playing) {
            s.first_unit = 1 + p * per;
            s.last_unit = s.first_unit + per - 1;
        }
        const u32 at = L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize;
        r.w32(at + pActive, s.active ? 1u : 0u);
        r.w32(at + pNetId, static_cast<u32>(s.net_id));
        r.w32(at + pInfo, L.pinfo + static_cast<u32>(p) * 0x100);
        r.write(at + pController, &s.controller, 1);
        r.write(at + pState, &s.state, 1);
        r.w32(at + pFirst, L.units + static_cast<u32>(playing ? s.first_unit : 1) * kUnitSize);
        r.w32(at + pLast, L.units + static_cast<u32>(playing ? s.last_unit : 0) * kUnitSize);
    }

    // Units.
    std::vector<sim::Unit> units(kMaxUnits + 2);
    std::vector<sim::UnitTickState> tstate(kMaxUnits + 2);
    World world;
    sim::UnitTickEnvironment env;
    world.units = &units;
    world.ticks = &tstate;
    world.players = &players;
    world.env = &env;
    world.settle_types = &settle_types;
    world.hm = &hm;
    world.off_map_units.assign(units.size(), false);
    world.bucket_heights.assign(units.size(), 0);
    sim::MotionContext mc{units, tstate, players, motion_types, hm, world, {}, {}};
    world.mc = &mc;
    world.clock = static_cast<u32>(g());
    r.w32(L.clock, world.clock);
    const u32 rng0 = static_cast<u32>(g()) | 1u;
    r.w32(kBattleRngState, rng0);
    world.rng.set_state(rng0);
    world.next_address = r.r32(L.bump_ptr);
    std::vector<std::unique_ptr<sim::UnitScriptHost>> hosts(units.size());
    std::vector<u32> remote_script(units.size(), 0), motion_block(units.size(), 0);
    std::vector<int> present;
    auto unit_addr = [&](int i) { return L.units + static_cast<u32>(i) * kUnitSize; };
    for (int i = 1; i <= kMaxUnits; ++i) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
        u.id = static_cast<std::uint16_t>(i);
        t.owner = (i - 1) / per;
        if (t.owner > 2 || g() % 8 == 0) continue;
        present.push_back(i);
        ++players[static_cast<std::size_t>(t.owner)].alive_count;
    }
    if (present.empty()) return true;
    for (int p = 0; p < 3; ++p)
        r.write(L.gs + 0x1B63 + static_cast<u32>(p) * kPlayerSize + pAlive, &players[static_cast<std::size_t>(p)].alive_count, 2);
    // A fake weapon type per unit (slot 0): aircraft follow goals keep its range as their distance.
    std::vector<sim::WeaponType> weapon_types(units.size());
    // Spatial buckets: bucket 0 is the off-map one; units get their own bucket with a terrain height.
    const i32 map_w = hm.width() * 16, map_h = hm.height() * 16;
    env.sea_level = static_cast<std::uint8_t>(g() % 3 == 0 ? g() % 60 : 0);
    env.game_tick = 1000;
    mc.env.game_tick = env.game_tick;
    mc.env.sea_level = env.sea_level;
    mc.env.gravity = static_cast<i32>(g() % 3 == 0 ? g() % 300 : 112);
    mc.env.addresses.game_state = L.gs;
    mc.env.addresses.unit_table = L.units;
    env.addresses = mc.env.addresses;
    r.w32(L.gs + 0x38A47, env.game_tick);
    r.write(L.gs + 0x1427F, &env.sea_level, 1);
    r.w32(L.gs + 0x14263, static_cast<u32>(mc.env.gravity));
    const std::uint8_t off_map_height = static_cast<std::uint8_t>(g());
    for (int i : present) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
        const int k = static_cast<int>(g() % ntypes);
        LoadedType& lt = types[static_cast<std::size_t>(pick[static_cast<std::size_t>(k)])];
        t.type_index = static_cast<std::uint16_t>(k + 1);
        t.tick_type = &lt.tick_info;
        u.type = &lt.info;
        u.pos = {static_cast<i32>((g() % static_cast<u32>(map_w - 400) + 200) << 16 | (g() & 0xFFFF)),
                 static_cast<i32>(g() % 3 == 0 ? (g() % 255) << 16 : (g() % 120) << 16 | (g() & 0xFFFF)),
                 static_cast<i32>((g() % static_cast<u32>(map_h - 400) + 200) << 16 | (g() & 0xFFFF))};
        u.heading = static_cast<std::uint16_t>(g());
        u.pitch = static_cast<std::int16_t>(g() % 2000);
        u.bank = static_cast<std::int16_t>(g() % 4 == 0 ? g() : g() % 12000);
        u.hover_phase = static_cast<std::uint16_t>(g());
        u.hit_points = static_cast<std::int16_t>(lt.info.max_damage);
        u.flags = sim::unit_flags::kAlive | 0x30u | (g() % 4 == 0 ? 2u : 1u) | (g() % 2 ? 0x10000u : 0u) | (g() % 4 == 0 ? (g() % 4) << 2 : 0u);
        u.footprint_x = static_cast<std::int16_t>(lt.def->footprint_x ? lt.def->footprint_x : 1 + g() % 4);
        u.footprint_z = static_cast<std::int16_t>(lt.def->footprint_z ? lt.def->footprint_z : 1 + g() % 4);
        u.cell_x = sim::footprint_cell(u.pos[0], u.footprint_x);
        u.cell_z = sim::footprint_cell(u.pos[2], u.footprint_z);
        if (g() % 4 == 0) u.cell_x = static_cast<std::int16_t>(u.cell_x + 1);
        u.water_occupancy = static_cast<i32>(g() % 6);
        u.transport_piece = static_cast<std::int8_t>(g() % 4);
        t.last_attacker_slot = 10;
        t.health_percent = t.prev_health_percent = 100;
        world.bucket_heights[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(g());
        world.off_map_units[static_cast<std::size_t>(i)] = g() % 12 == 0;
        weapon_types[static_cast<std::size_t>(i)].range = g() % 4 == 0 ? 0 : static_cast<i32>(g() % 900);
        u.weapons[0].type = &weapon_types[static_cast<std::size_t>(i)];
        const u32 at = unit_addr(i);
        const u32 owner_at = L.gs + 0x1B63 + static_cast<u32>(t.owner) * kPlayerSize;
        const std::uint8_t owner_slot = static_cast<std::uint8_t>(t.owner);
        if (world.off_map_units[static_cast<std::size_t>(i)]) world.bucket_heights[static_cast<std::size_t>(i)] = off_map_height;  // one shared bucket
        const u32 bucket = world.off_map_units[static_cast<std::size_t>(i)] ? L.buckets : L.buckets + static_cast<u32>(i) * 16;
        r.write(bucket + 1, &world.bucket_heights[static_cast<std::size_t>(i)], 1);
        const u32 wt = L.weapons + static_cast<u32>(i) * 0x100;
        r.w32(wt + 0xDC, static_cast<u32>(weapon_types[static_cast<std::size_t>(i)].range));
        r.w32(at + 4 + 0xC, wt);
        r.write(at + uPitch, &u.pitch, 2);
        r.write(at + uHeading, &u.heading, 2);
        r.write(at + uBank, &u.bank, 2);
        r.write(at + uPosX, &u.pos[0], 4);
        r.write(at + uPosY, &u.pos[1], 4);
        r.write(at + uPosZ, &u.pos[2], 4);
        r.write(at + uCell, &u.cell_x, 2);
        r.write(at + uCell + 2, &u.cell_z, 2);
        r.write(at + uFootprint, &u.footprint_x, 2);
        r.write(at + uFootprint + 2, &u.footprint_z, 2);
        r.w32(at + uBucket, bucket);
        r.write(at + uTransportPiece, &u.transport_piece, 1);
        r.w32(at + uWaterOcc, static_cast<u32>(u.water_occupancy));
        r.w32(at + uDef, lt.remote_def);
        r.w32(at + uOwner, owner_at);
        r.write(at + uOwnerSlot, &owner_slot, 1);
        r.w32(at + uResourcePlayer, owner_at);
        r.write(at + uDefIndex, &t.type_index, 2);
        r.write(at + uId, &u.id, 2);
        r.write(at + uHoverPhase, &u.hover_phase, 2);
        r.write(at + uLastAttackerSlot, &t.last_attacker_slot, 1);
        r.write(at + uHealth, &t.health_percent, 1);
        r.write(at + uPrevHealth, &t.prev_health_percent, 1);
        r.write(at + uBuildLeft, &u.build_fraction_left, 4);
        r.write(at + uHp, &u.hit_points, 2);
        r.w32(at + uFlags, u.flags);
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
        u.pose.build(lt.pose_model, lt.cob.program.piece_names);
        hosts[static_cast<std::size_t>(i)] = std::make_unique<sim::UnitScriptHost>(u, world);
        u.script = std::make_unique<sim::ScriptInstance>(lt.cob.program, *hosts[static_cast<std::size_t>(i)], 30);
        u.script->set_step_limit(50000000);
    }
    ++st.scenarios;
    // Carriers: a few units ride on another unit (never a chain).
    for (int i : present) {
        if (g() % 8) continue;
        const int ci = present[g() % present.size()];
        if (ci == i || units[static_cast<std::size_t>(ci)].carrier || units[static_cast<std::size_t>(i)].cargo_head) continue;
        bool carries = false;
        for (int j : present) carries |= units[static_cast<std::size_t>(j)].carrier == &units[static_cast<std::size_t>(i)];
        if (carries) continue;
        units[static_cast<std::size_t>(i)].carrier = &units[static_cast<std::size_t>(ci)];
        r.w32(unit_addr(i) + uCarrier, unit_addr(ci));
    }

    auto set_env = [&](int tick) {
        env.game_tick = 1000 + static_cast<u32>(tick);
        env.water_damage_enabled = g() % 4 == 0;
        env.water_damage = env.water_damage_enabled ? static_cast<i32>(g() % 3) * 50 : 0;
        env.broadcast_unit_states = false;
        mc.env.game_tick = env.game_tick;
        world.place_result = g() % 4 ? 1u : 0u;
        r.w32(L.gs + 0x38A47, env.game_tick);
        r.w32(L.schema + 0xD4C, env.water_damage_enabled ? 1u : 0u);
        r.w32(L.schema + 0xD50, static_cast<u32>(env.water_damage));
        r.w32(L.gs + 0x2A44, 0);
        r.w32(L.gs + 0x14373, 0);
        r.w32(L.gs + 0x37EE2, 0);
        r.w32(L.ret_val, world.place_result);
    };
    set_env(0);
    for (int i : present) {
        r.write(L.names, "Create", 7);
        if (!r.call(kCallNamed, remote_script[static_cast<std::size_t>(i)], {L.names, 0, 1})) {
            std::printf("scenario %u: Create failed\n", seed);
            return false;
        }
        units[static_cast<std::size_t>(i)].script->call("Create", nullptr, true);
    }
    // Motion objects, built by the original constructor.
    world.next_address = r.r32(L.bump_ptr);  // object states took their memory before
    for (int i : present) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        u32 block = 0;
        if (!r.call(kAlloc, 0, {0x2F}, &block)) return false;
        const u32 mine = world.allocate_object(0x2F);
        if (block != mine) {
            std::printf("scenario %u: allocator out of step\n", seed);
            return false;
        }
        motion_block[static_cast<std::size_t>(i)] = block;
        r.write(block, zero.data(), 0x30);
        tstate[static_cast<std::size_t>(i)].motion.address = block;
        if (!r.call(kMotionCtor, block, {unit_addr(i)})) return false;
        sim::create_motion(mc, u);
        r.w32(unit_addr(i) + uMotion, block);
    }
    // Fake mission orders (two per unit); the first is the unit's primary head.
    std::vector<std::vector<std::unique_ptr<sim::MissionOrder>>> orders(units.size());
    std::map<u32, sim::MissionOrder*> order_at;
    for (int i : present) {
        for (int k = 0; k < 2; ++k) {
            u32 block = 0;
            if (!r.call(kAlloc, 0, {0x56}, &block) || block != world.allocate_object(0x56)) return false;
            r.write(block, zero.data(), 0x58);
            auto o = std::make_unique<sim::MissionOrder>();
            o->address = block;
            o->unit = &units[static_cast<std::size_t>(i)];
            o->flags = g() % 3 == 0 ? sim::order_flags::kLastOrder : 0u;
            o->wake_tick = 0;
            r.w32(block + oUnit, unit_addr(i));
            r.w32(block + oFlags, o->flags);
            order_at[block] = o.get();
            orders[static_cast<std::size_t>(i)].push_back(std::move(o));
        }
        units[static_cast<std::size_t>(i)].orders = orders[static_cast<std::size_t>(i)][0].get();
        r.w32(unit_addr(i) + uOrders, orders[static_cast<std::size_t>(i)][0]->address);
    }
    std::set<u32> goal_blocks;  // every order goal block ever made (addresses)
    std::vector<u32> pre_flags(units.size(), 0);

    auto norm_ptr = [&](u32& w) {
        if (w >= L.units && w < L.units + (kMaxUnits + 2) * kUnitSize) {
            const u32 off = (w - L.units) % kUnitSize, idx = (w - L.units) / kUnitSize;
            if (off == 0) w = 0x40000000u | idx;
        } else if (w >= L.gs + 0x1B63 && w < L.gs + 0x1B63 + 10 * kPlayerSize && (w - L.gs - 0x1B63) % kPlayerSize == 0) {
            w = 0x20000000u | ((w - L.gs - 0x1B63) / kPlayerSize);
        } else if (w >= L.heap + (64u << 20) && w < L.heap + (64u << 20) + 0x4000u * (kMaxUnits + 2) && (w - L.heap - (64u << 20)) % 0x4000 == 0) {
            w = 0x40000000u | ((w - L.heap - (64u << 20)) / 0x4000);
        } else if (w >= L.defs && w < L.defs + 0x400u * ntypes && (w - L.defs) % 0x400 == 0) {
            w = 0x43000000u | ((w - L.defs) / 0x400 + 1);
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
            if (remote && (id < 10 || id > 13)) rec[1] = 0;
            if (id == 20) rec[3] &= 0xFFFF;  // the id is pushed with a stale upper half
            if (id == 53 && !goal_blocks.count(rec[2]) && rec[2] < L.scenario_heap) continue;
            if (id == 16) {
                rec[3] = 0;
                const u32 size = rec[4];
                if (size == 4) rec[2] = rec[5] = rec[6] = rec[7] = 0;
                std::uint8_t bytes[12];
                std::memcpy(bytes, &rec[5], 12);
                for (u32 b = size < 12 ? size : 12; b < 12; ++b) bytes[b] = 0;
                std::memcpy(&rec[5], bytes, 12);
            }
            out.insert(out.end(), rec.begin(), rec.end());
        }
        return out;
    };

    // Engine order goals by address.
    auto goal_of = [&](u32 address) -> sim::OrderGoal* {
        for (int i : present)
            for (auto& o : orders[static_cast<std::size_t>(i)])
                if (o->goal && o->goal->address == address) return o->goal.get();
        return nullptr;
    };
    auto compare = [&](const std::string& what) -> bool {
        bool ok = true;
        int reported = 0;
        auto bad = [&](const std::string& m) {
            if (reported++ < 8) std::printf("scenario %u %s: %s\n", seed, what.c_str(), m.c_str());
            ok = false;
        };
        const u32 end = r.r32(L.log_ptr);
        std::vector<u32> log((end - L.log_base) / 4);
        if (!log.empty()) r.read(L.log_base, log.data(), log.size() * 4);
        r.w32(L.log_ptr, L.log_base);
        for (auto& w : log) norm_ptr(w);
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
        if (r.r32(L.bump_ptr) != world.next_address) bad("allocations");
        if (r.r32(L.clock) != world.clock) bad("clock reads");
        if (r.r32(kBattleRngState) != world.rng.state()) bad("battle RNG state");
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
            if (f32(uFlags) != u.flags) bad(un + "flags " + std::to_string(f32(uFlags)) + " vs " + std::to_string(u.flags));
            if (f8(uActivate) != u.activate_flags) bad(un + "activate flags");
            if (static_cast<i32>(f32(uPosX)) != u.pos[0] || static_cast<i32>(f32(uPosY)) != u.pos[1] || static_cast<i32>(f32(uPosZ)) != u.pos[2])
                bad(un + "position");
            if (static_cast<std::int16_t>(f16(uPitch)) != u.pitch || f16(uHeading) != u.heading || static_cast<std::int16_t>(f16(uBank)) != u.bank)
                bad(un + "pitch/heading/bank");
            if (static_cast<std::int16_t>(f16(uCell)) != u.cell_x || static_cast<std::int16_t>(f16(uCell + 2)) != u.cell_z) bad(un + "cell");
            if (static_cast<i32>(f32(uWaterOcc)) != u.water_occupancy) bad(un + "water occupancy");
            // Motion object.
            const u32 mb = motion_block[static_cast<std::size_t>(i)];
            std::vector<std::uint8_t> rm(0x30);
            r.read(mb, rm.data(), rm.size());
            auto m16 = [&](u32 off) { std::uint16_t v; std::memcpy(&v, &rm[off], 2); return v; };
            auto m32 = [&](u32 off) { u32 v; std::memcpy(&v, &rm[off], 4); return v; };
            const sim::MotionState& m = t.motion;
            for (int k = 0; k < 3; ++k) {
                if (static_cast<i32>(m32(mVel + 4u * static_cast<u32>(k))) != t.velocity[static_cast<std::size_t>(k)]) bad(un + "velocity");
                if (static_cast<i32>(m32(mTilt + 4u * static_cast<u32>(k))) != m.tilt[static_cast<std::size_t>(k)]) bad(un + "tilt");
            }
            if (static_cast<i32>(m32(mSpeed)) != m.speed) bad(un + "speed");
            if (static_cast<std::int16_t>(m16(mTurn)) != m.turn) bad(un + "turn");
            if (m32(mMove) != m.move_tick) bad(un + "move tick");
            if ((rm[mFlags] & 7) != (m.flags & 7)) bad(un + "motion flags");
            // Motion goal.
            const u32 ga = m32(mGoal);
            const sim::MotionGoal* mg = m.goal.get();
            if (!mg || ga != mg->address) {
                bad(un + "motion goal address");
            } else {
                std::vector<std::uint8_t> rg(0x68);
                r.read(ga, rg.data(), rg.size());
                auto g32 = [&](u32 off) { u32 v; std::memcpy(&v, &rg[off], 4); return v; };
                auto g16 = [&](u32 off) { std::uint16_t v; std::memcpy(&v, &rg[off], 2); return v; };
                if (g32(4) != (mg->attached ? mg->attached->address : 0u)) bad(un + "motion goal attachment");
                if (auto* rgl = dynamic_cast<const sim::RouteGoal*>(mg)) {
                    if (static_cast<i32>(g32(0x5C)) != rgl->count) bad(un + "route count");
                    for (i32 k = 0; k < rgl->count && k < 20; ++k)
                        if (g32(0xC + 4u * static_cast<u32>(k)) != rgl->points[static_cast<std::size_t>(k)]) bad(un + "route point");
                    if (g32(0x60) != rgl->search_tick) bad(un + "route search tick");
                    if ((rg[0x64] & 0xF) != (rgl->flags & 0xF)) bad(un + "route flags");
                } else if (auto* fg = dynamic_cast<const sim::FlightGoal*>(mg)) {
                    for (int k = 0; k < 3; ++k) {
                        if (static_cast<i32>(g32(0xC + 4u * static_cast<u32>(k))) != fg->pos[static_cast<std::size_t>(k)])
                            bad(un + "flight target " + std::to_string(k) + ": orig " + std::to_string(static_cast<i32>(g32(0xC + 4u * static_cast<u32>(k)))) +
                                " ours " + std::to_string(fg->pos[static_cast<std::size_t>(k)]) + " (unit at " + std::to_string(u.pos[0]) + "," +
                                std::to_string(u.pos[1]) + "," + std::to_string(u.pos[2]) + ", attached kind " +
                                std::to_string(fg->attached ? fg->attached->kind() : 0) + ")");
                        if (static_cast<i32>(g32(0x18 + 4u * static_cast<u32>(k))) != fg->delta[static_cast<std::size_t>(k)]) bad(un + "flight delta");
                    }
                    if (g16(0x24) != fg->target_heading) bad(un + "flight heading");
                    if ((rg[0x27] & 7) != (fg->net_flags & 7)) bad(un + "flight net flags");
                }
            }
            // Orders and their goals.
            for (auto& o : orders[static_cast<std::size_t>(i)]) {
                if (r.r32(o->address + oEvents) != o->events) bad(un + "order events");
                const u32 att = r.r32(o->address + oAttached);
                if (att != o->attached) {
                    bad(un + "order goal address");
                    continue;
                }
                const sim::OrderGoal* og = o->goal.get();
                if (!att || !og) continue;
                std::vector<std::uint8_t> rg(0x40);
                r.read(att, rg.data(), rg.size());
                auto g32 = [&](u32 off) { u32 v; std::memcpy(&v, &rg[off], 4); return v; };
                auto g16 = [&](u32 off) { std::uint16_t v; std::memcpy(&v, &rg[off], 2); return v; };
                if (g32(4) != o->address) bad(un + "goal order");
                if (auto* p = dynamic_cast<const sim::PointGoal*>(og)) {
                    u32 lu = g32(0x1A);
                    norm_ptr(lu);
                    if (g16(8) != p->flags || static_cast<std::int16_t>(g16(0xA)) != p->radius || static_cast<std::int16_t>(g16(0xC)) != p->altitude ||
                        static_cast<std::int16_t>(g16(0xE)) != p->goal_heading || static_cast<std::int16_t>(g16(0x10)) != p->piece)
                        bad(un + "point goal fields");
                    if (lu != (p->link.unit ? World::tok(*p->link.unit) : 0u)) bad(un + "point goal link");
                    for (int k = 0; k < 3; ++k)
                        if (static_cast<i32>(g32(0x26 + 4u * static_cast<u32>(k))) != p->pos[static_cast<std::size_t>(k)]) bad(un + "point goal position");
                    if (static_cast<i32>(g32(0x32)) != p->offset) bad(un + "point goal offset");
                } else if (auto* mv = dynamic_cast<const sim::ManoeuvreGoal*>(og)) {
                    if (g16(8) != mv->flags || g16(0x24) != mv->target_heading) bad(un + "manoeuvre fields");
                    for (int k = 0; k < 3; ++k) {
                        if (static_cast<i32>(g32(0xA + 4u * static_cast<u32>(k))) != mv->pos[static_cast<std::size_t>(k)]) bad(un + "manoeuvre position");
                        if (static_cast<i32>(g32(0x16 + 4u * static_cast<u32>(k))) != mv->vel[static_cast<std::size_t>(k)]) bad(un + "manoeuvre velocity");
                    }
                } else if (auto* cg = dynamic_cast<const sim::CircleGoal*>(og)) {
                    if (static_cast<std::int16_t>(g16(8)) != cg->cell_x || static_cast<std::int16_t>(g16(0xA)) != cg->cell_z ||
                        static_cast<i32>(g32(0xC)) != cg->radius || static_cast<i32>(g32(0x10)) != cg->radius_cells_sq)
                        bad(un + "circle goal");
                } else if (auto* rgg = dynamic_cast<const sim::RingGoal*>(og)) {
                    if (static_cast<std::int16_t>(g16(8)) != rgg->cell_x || static_cast<std::int16_t>(g16(0xA)) != rgg->cell_z ||
                        static_cast<i32>(g32(0xC)) != rgg->inner || static_cast<i32>(g32(0x10)) != rgg->outer ||
                        static_cast<i32>(g32(0x14)) != rgg->inner_cells_sq || static_cast<i32>(g32(0x18)) != rgg->outer_cells_sq)
                        bad(un + "ring goal");
                } else if (auto* eg = dynamic_cast<const sim::EdgeGoal*>(og)) {
                    if (static_cast<i32>(g32(8)) != eg->min_x || static_cast<i32>(g32(0xC)) != eg->max_x || static_cast<i32>(g32(0x10)) != eg->min_z ||
                        static_cast<i32>(g32(0x14)) != eg->max_z)
                        bad(un + "edge goal");
                }
            }
            // Links pointing at the unit (point goals following it).
            {
                u32 ra = f32(uReferrers);
                const sim::UnitLink* l = u.referrers;
                for (int n = 0; (ra || l) && n < 1000; ++n) {
                    const u32 mine_at = l && l->goal ? l->goal->address + 0x16 : 0u;
                    if (!ra || !l || ra != mine_at) {
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
                    bad(un + "script thread word " + std::to_string(w) + ": orig " + std::to_string(blk[w]) + " ours " + std::to_string(mem[w]));
                    break;
                }
            std::vector<i32> an(u.script->piece_anims().size());
            if (!an.empty()) r.read(so + 0x1000, an.data(), an.size() * 4);
            if (an != u.script->piece_anims()) bad(un + "script piece animations");
            std::vector<i32> sv(u.script->statics().size());
            if (!sv.empty()) r.read(so + 0x600, sv.data(), sv.size() * 4);
            if (sv != u.script->statics()) {
                std::string d;
                for (std::size_t k = 0; k < sv.size(); ++k)
                    if (sv[k] != u.script->statics()[k]) d += " [" + std::to_string(k) + "] " + std::to_string(sv[k]) + "/" + std::to_string(u.script->statics()[k]);
                if (std::getenv("MH_VERBOSE")) {
                    const auto& pr = u.script->program();
                    for (std::size_t f = 0; f < pr.function_names.size(); ++f) {
                        std::printf("    fn %s:", pr.function_names[f].c_str());
                        const std::size_t e0 = pr.entry[f], e1 = f + 1 < pr.entry.size() ? pr.entry[f + 1] : pr.code.size();
                        for (std::size_t w = e0; w < e1 && w < e0 + 40; ++w) std::printf(" %x", pr.code[w]);
                        std::printf("\n");
                    }
                }
                bad(un + "script statics (orig/ours)" + d + "; speed " + std::to_string(t.motion.speed) + " turn " + std::to_string(t.motion.turn) +
                    " flags " + std::to_string(u.flags) + " carrier " + std::to_string(u.carrier ? u.carrier->id : 0) + " occ " + std::to_string(u.water_occupancy) + " flags before the pass " + std::to_string(pre_flags[static_cast<std::size_t>(i)]) +
                    " r32 " + std::to_string(r.r32(at + uFlags)) + " type " + (t.tick_type ? t.tick_type->name : std::string("?")));
            }
            if (!u.script->fault().empty()) bad(un + "script fault: " + u.script->fault());
        }
        if (!ok) ++st.mismatches;
        return ok;
    };
    auto engine_faulted = [&]() {
        if (!mc.fault.empty()) {
            ++st.faults;
            std::printf("scenario %u: stopped, engine motion fault: %s\n", seed, mc.fault.c_str());
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
    auto fail = [&](const std::string& what) {
        std::printf("scenario %u: the original failed during %s\n", seed, what.c_str());
        return false;
    };
    auto any_unit = [&]() { return present[g() % present.size()]; };
    auto on_map_unit = [&]() -> int {  // a unit that may be followed (never off the map)
        for (int tries = 0; tries < 20; ++tries) {
            const int i = any_unit();
            if (!world.off_map_units[static_cast<std::size_t>(i)]) return i;
        }
        return 0;
    };
    auto near_point = [&](const sim::Unit& u, i32 spread) -> Vec3 {
        return {static_cast<i32>(u.pos[0] + static_cast<i32>(g() % static_cast<u32>(2 * spread)) - spread),
                static_cast<i32>(g() % 3 == 0 ? static_cast<i32>(g() % 300) << 16 : u.pos[1]),
                static_cast<i32>(u.pos[2] + static_cast<i32>(g() % static_cast<u32>(2 * spread)) - spread)};
    };
    auto write_unit_motion = [&](int i) {  // push engine-side unit/motion state changes to the original
        const sim::Unit& u = units[static_cast<std::size_t>(i)];
        const sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
        const u32 at = unit_addr(i), mb = motion_block[static_cast<std::size_t>(i)];
        r.write(at + uPitch, &u.pitch, 2);
        r.write(at + uHeading, &u.heading, 2);
        r.write(at + uBank, &u.bank, 2);
        r.write(at + uPosX, &u.pos[0], 4);
        r.write(at + uPosY, &u.pos[1], 4);
        r.write(at + uPosZ, &u.pos[2], 4);
        r.w32(at + uFlags, u.flags);
        r.write(mb + mVel, t.velocity.data(), 12);
        r.write(mb + mTilt, t.motion.tilt.data(), 12);
        r.w32(mb + mSpeed, static_cast<u32>(t.motion.speed));
        r.write(mb + mTurn, &t.motion.turn, 2);
        r.w32(mb + mMove, t.motion.move_tick);
        std::uint8_t f = r.rd<std::uint8_t>(mb + mFlags);
        f = static_cast<std::uint8_t>((f & 0xF8) | (t.motion.flags & 7));
        r.write(mb + mFlags, &f, 1);
    };
    // Random motion state (speed, velocity, lean, layer, blocked bit, attitude, small position jumps).
    auto tweak_motion = [&](int i) {
        sim::Unit& u = units[static_cast<std::size_t>(i)];
        sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
        sim::MotionState& m = t.motion;
        const sim::MotionType& ty = motion_types[t.type_index];
        if (g() % 2) m.speed = g() % 6 == 0 ? static_cast<i32>(g()) : static_cast<i32>(g() % static_cast<u32>(ty.max_velocity * 2 + 1));
        if (g() % 2)
            for (auto& v : t.velocity) v = g() % 5 == 0 ? 0 : static_cast<i32>(g() % 0x80000) - 0x40000;
        if (g() % 3 == 0)
            for (auto& v : m.tilt) v = static_cast<i32>(g() % 0x100000) - 0x80000;
        if (g() % 3 == 0) m.turn = static_cast<std::int16_t>(g() % 3 ? 0 : g());
        if (g() % 3 == 0) m.flags = static_cast<std::uint8_t>((m.flags & ~7) | (g() % 8));
        if (g() % 3 == 0) {
            const bool flies = (ty.type_flags & sim::motion_flags::kTypeFlies) != 0;
            m.flags = static_cast<std::uint8_t>((m.flags & ~3) | (flies && g() % 4 ? 2 : 1));
        }
        if (g() % 4 == 0) u.heading = static_cast<std::uint16_t>(g());
        if (g() % 4 == 0) u.bank = static_cast<std::int16_t>(g() % 30000 - 15000);
        if (g() % 6 == 0) u.flags ^= g() % 2 ? 0xCu : static_cast<u32>(g() % 4);
        if (g() % 5 == 0) {
            u.pos[0] += static_cast<i32>(g() % 0x200000) - 0x100000;
            u.pos[2] += static_cast<i32>(g() % 0x200000) - 0x100000;
        }
        if (g() % 5 == 0) u.pos[1] = static_cast<i32>((g() % 120) << 16 | (g() & 0xFFFF));
        if (g() % 8 == 0) m.move_tick = env.game_tick - g() % 90;
        write_unit_motion(i);
    };
    auto direct = [&](u32 fn, u32 self, const std::vector<u32>& args, const std::string& what) {
        if (!r.call(fn, self, args)) return fail(what);
        return true;
    };

    if (engine_faulted()) return true;
    if (!compare("after setup")) return true;
    for (int i : present) {  // start with some motion
        tweak_motion(i);
    }
    if (!compare("after first tweak")) return true;

    for (int tick = 0; tick < ticks_to_run; ++tick) {
        set_env(tick);
        const int nops = static_cast<int>(g() % 7);
        for (int op = 0; op < nops; ++op) {
            const int i = any_unit();
            sim::Unit& u = units[static_cast<std::size_t>(i)];
            sim::UnitTickState& t = tstate[static_cast<std::size_t>(i)];
            sim::MissionOrder& o = *orders[static_cast<std::size_t>(i)][g() % 2];
            const u32 mb = motion_block[static_cast<std::size_t>(i)];
            std::string what;
            int kind = static_cast<int>(g() % 16);
            if (const char* focus = std::getenv("MH_FOCUS")) {  // debugging: only these operation kinds (comma separated)
                std::vector<int> kinds;
                for (const char* p = focus; *p;) {
                    kinds.push_back(std::atoi(p));
                    while (*p && *p != ',') ++p;
                    if (*p) ++p;
                }
                kind = kinds[g() % kinds.size()];
            }
            if (kind <= 3) {  // a new order goal
                const int gk = static_cast<int>(g() % 9);
                u32 block = 0;
                std::shared_ptr<sim::OrderGoal> mine;
                const Vec3 p = near_point(u, 0x4000000);
                r.write(L.args, p.data(), 12);
                if (gk == 0) {
                    if (!r.call(kAlloc, 0, {0x36}, &block)) return fail("alloc");
                    mine = sim::make_point_goal(mc, o, p);
                    if (!direct(kPointCtor, block, {o.address, L.args}, "point ctor")) return false;
                    what = "point goal";
                } else if (gk == 1 || gk == 2) {
                    const int ti = on_map_unit();
                    if (!ti) continue;
                    if (!r.call(kAlloc, 0, {0x36}, &block)) return fail("alloc");
                    if (gk == 1) {
                        mine = sim::make_follow_goal(mc, o, &units[static_cast<std::size_t>(ti)]);
                        if (!direct(kFollowCtor, block, {o.address, unit_addr(ti)}, "follow ctor")) return false;
                        what = "follow goal";
                    } else {
                        const std::int16_t piece = static_cast<std::int16_t>(g() % 3 == 0 ? -1 : static_cast<int>(g() % 6));
                        mine = sim::make_dock_goal(mc, o, &units[static_cast<std::size_t>(ti)], piece);
                        if (!direct(kDockCtor, block, {o.address, unit_addr(ti), static_cast<u32>(static_cast<i32>(piece))}, "dock ctor")) return false;
                        what = "dock goal";
                    }
                } else if (gk == 3) {
                    const int ti = on_map_unit();
                    if (!ti) continue;
                    if (!r.call(kAlloc, 0, {0x36}, &block)) return fail("alloc");
                    mine = sim::make_face_goal(mc, o, &units[static_cast<std::size_t>(ti)], p);
                    if (!direct(kFaceCtor, block, {o.address, unit_addr(ti), L.args}, "face ctor")) return false;
                    what = "face goal";
                } else if (gk == 4) {
                    const Vec3 v{static_cast<i32>(g() % 0x80000) - 0x40000, static_cast<i32>(g() % 0x20000) - 0x10000, static_cast<i32>(g() % 0x80000) - 0x40000};
                    r.write(L.args + 0x10, v.data(), 12);
                    if (!r.call(kAlloc, 0, {0x2C}, &block)) return fail("alloc");
                    mine = sim::make_manoeuvre_goal(mc, o, p, v);
                    if (!direct(kManoeuvreCtor, block, {o.address, L.args, L.args + 0x10}, "manoeuvre ctor")) return false;
                    what = "manoeuvre goal";
                } else if (gk == 5) {
                    const u32 cell = (static_cast<u32>(u.cell_x + static_cast<i32>(g() % 20) - 10) & 0xFFFF) |
                                     (static_cast<u32>(u.cell_z + static_cast<i32>(g() % 20) - 10) << 16);
                    const u32 size = (1 + g() % 6) | ((1 + g() % 6) << 16);
                    if (!r.call(kAlloc, 0, {0x18}, &block)) return fail("alloc");
                    mine = sim::make_edge_goal(mc, o, cell, size);
                    if (!direct(kEdgeCtor, block, {o.address, cell, size}, "edge ctor")) return false;
                    what = "edge goal";
                } else if (gk == 6 || gk == 7) {  // the wrappers make the area goal themselves
                    const i32 radius = static_cast<i32>(g() % 600);
                    if (gk == 6) {
                        sim::attach_circle_goal(mc, o, p, radius);
                        if (!direct(kAttachCircle, o.address, {L.args, static_cast<u32>(radius)}, "attach circle")) return false;
                        what = "circle goal";
                    } else {
                        const i32 inner = static_cast<i32>(g() % 400);
                        sim::attach_ring_goal(mc, o, p, radius, inner);
                        if (!direct(kAttachRing, o.address, {L.args, static_cast<u32>(radius), static_cast<u32>(inner)}, "attach ring")) return false;
                        what = "ring goal";
                    }
                    if (o.goal) goal_blocks.insert(o.goal->address);
                } else {  // detach
                    sim::attach_order_goal(mc, o, nullptr);
                    if (!direct(kAttachGoal, o.address, {0}, "detach")) return false;
                    what = "detach goal";
                }
                if (block) {
                    if (block != mine->address) {
                        std::printf("scenario %u: goal allocation out of step\n", seed);
                        return false;
                    }
                    goal_blocks.insert(block);
                    sim::attach_order_goal(mc, o, mine);
                    if (!direct(kAttachGoal, o.address, {block}, "attach")) return false;
                }
            } else if (kind == 4) {  // goal setters
                sim::OrderGoal* og = o.goal.get();
                if (!og) continue;
                if (auto* p = dynamic_cast<sim::PointGoal*>(og)) {
                    const int s = static_cast<int>(g() % 3);
                    if (s == 0) {
                        const i32 alt = static_cast<i32>(g() % 400) - 50;
                        p->set_altitude(mc, alt);
                        if (!direct(kSetAltitude, og->address, {static_cast<u32>(alt)}, "set altitude")) return false;
                    } else if (s == 1) {
                        const std::int16_t h = static_cast<std::int16_t>(g());
                        p->set_heading(h);
                        if (!direct(kSetHeading, og->address, {static_cast<u32>(static_cast<i32>(h))}, "set heading")) return false;
                    } else {
                        const std::int16_t rad = static_cast<std::int16_t>(g() % 300);
                        p->set_radius(rad);
                        if (!direct(kSetRadius, og->address, {static_cast<u32>(static_cast<i32>(rad))}, "set radius")) return false;
                    }
                    what = "point goal setter";
                } else if (auto* mv = dynamic_cast<sim::ManoeuvreGoal*>(og)) {
                    const std::uint16_t h = static_cast<std::uint16_t>(g());
                    mv->set_target_heading(h);
                    if (!direct(kSetTargetHeading, og->address, {h}, "set target heading")) return false;
                    what = "manoeuvre setter";
                } else {
                    continue;
                }
            } else if (kind == 5) {  // the route search answers
                auto* rg = dynamic_cast<sim::RouteGoal*>(t.motion.goal.get());
                if (!rg) continue;
                const int n = g() % 6 == 0 ? 0 : static_cast<int>(1 + g() % 24);
                std::vector<u32> pts(static_cast<std::size_t>(n) + 1);
                i32 x = u.pos[0] >> 16, z = u.pos[2] >> 16;
                for (auto& p : pts) {
                    p = (static_cast<u32>(x) & 0xFFFF) | (static_cast<u32>(z) << 16);
                    x += static_cast<i32>(g() % 120) - 60;
                    z += static_cast<i32>(g() % 120) - 60;
                }
                r.write(L.args, pts.data(), pts.size() * 4);
                rg->set_route(mc, pts.data(), n);
                if (!direct(kSetRoute, rg->address, {L.args, static_cast<u32>(n)}, "set route")) return false;
                what = "route answer";
            } else if (kind == 6) {  // the route search asks
                sim::MotionGoal* mg = t.motion.goal.get();
                if (g() % 2) {
                    if (auto* rg = dynamic_cast<sim::RouteGoal*>(mg)) rg->search_tick = env.game_tick - g() % 100;
                    if (dynamic_cast<sim::RouteGoal*>(mg)) r.w32(mg->address + 0x60, dynamic_cast<sim::RouteGoal*>(mg)->search_tick);
                }
                const bool mine_wants = mg->wants_route(mc);
                u32 theirs = 0;
                const u32 fn = r.r32(r.r32(mg->address) + 0x18);
                if (!r.call(fn, mg->address, {}, &theirs)) return fail("wants route");
                if ((theirs != 0) != mine_wants || (theirs && theirs != mg->address)) {
                    std::printf("scenario %u: wants-route differs (orig %08x ours %d)\n", seed, theirs, mine_wants ? 1 : 0);
                    ++st.mismatches;
                    return true;
                }
                what = "route request";
            } else if (kind == 7) {  // order goal queries
                sim::OrderGoal* og = o.goal.get();
                if (!og) continue;
                const u32 vt = r.r32(og->address);
                const int q = static_cast<int>(g() % 4);
                u32 theirs = 0;
                if (q == 0) {
                    const i32 cx = u.cell_x + static_cast<i32>(g() % 30) - 15, cz = u.cell_z + static_cast<i32>(g() % 30) - 15;
                    const bool mine_in = og->contains_cell(cx, cz);
                    if (!r.call(r.r32(vt + 0x14), og->address, {static_cast<u32>(cx), static_cast<u32>(cz)}, &theirs)) return fail("contains");
                    u32 dist = 0;
                    if (!r.call(r.r32(vt + 0x1C), og->address, {static_cast<u32>(cx), static_cast<u32>(cz)}, &dist)) return fail("distance");
                    if ((theirs & 0xFF) != (mine_in ? 1u : 0u) || static_cast<i32>(dist) != og->distance(cx, cz)) {
                        std::printf("scenario %u: goal kind %d cell query differs\n", seed, og->kind());
                        ++st.mismatches;
                        return true;
                    }
                    what = "goal cell query";
                } else if (q == 1) {
                    Vec3 mine_p{0x11111111, 0x22222222, 0x33333333};
                    r.write(L.args, mine_p.data(), 12);
                    const bool ok_mine = og->position(mc, mine_p);
                    if (!r.call(r.r32(vt + 0x20), og->address, {L.args}, &theirs)) return fail("position");
                    Vec3 their_p{};
                    r.read(L.args, their_p.data(), 12);
                    if ((theirs != 0) != ok_mine || their_p != mine_p) {
                        std::printf("scenario %u: goal kind %d position differs\n", seed, og->kind());
                        ++st.mismatches;
                        return true;
                    }
                    what = "goal position";
                } else if (q == 2) {
                    std::uint16_t mine_h = 0x5A5A;
                    r.write(L.args, &mine_h, 2);
                    const bool ok_mine = og->heading(mc, mine_h);
                    if (!r.call(r.r32(vt + 0x24), og->address, {L.args}, &theirs)) return fail("heading");
                    const std::uint16_t their_h = r.rd<std::uint16_t>(L.args);
                    if ((theirs != 0) != ok_mine || their_h != mine_h) {
                        std::printf("scenario %u: goal kind %d heading differs\n", seed, og->kind());
                        ++st.mismatches;
                        return true;
                    }
                    what = "goal heading";
                } else {
                    const bool mine_r = og->reached(mc, u);
                    if (!r.call(r.r32(vt + 0x10), og->address, {unit_addr(i)}, &theirs)) return fail("reached");
                    u32 keep = 0;
                    if (!r.call(r.r32(vt + 0x2C), og->address, {}, &keep)) return fail("keep");
                    if ((theirs & 0xFF) != (mine_r ? 1u : 0u) || (keep != 0) != og->keep_when_reached()) {
                        std::printf("scenario %u: goal kind %d reached/keep differs\n", seed, og->kind());
                        ++st.mismatches;
                        return true;
                    }
                    what = "goal reached";
                }
            } else if (kind == 8) {  // layer change
                const u32 layer = g() % 2 ? 1u : 2u;
                sim::set_motion_layer(mc, u, layer);
                if (!direct(kSetLayer, mb, {unit_addr(i), layer}, "set layer")) return false;
                what = "set layer";
            } else if (kind == 9) {  // destroy-path release of the order's goal
                sim::release_order_goal(mc, u, o);
                o.flags |= sim::order_flags::kNotCurrent;  // the destroy routine then leaves the weapon targets alone
                r.w32(o.address + oFlags, o.flags);
                if (!direct(kOrderDestroy, o.address, {}, "order destroy")) return false;
                what = "release goal";
            } else if (kind == 10) {
                tweak_motion(i);
                what = "tweak";
            } else if (kind == 11) {  // the motion step, called directly
                const u32 stale = static_cast<u32>(g()), ebx = static_cast<u32>(g());
                r.w32(L.ret_val + kStaleIn, stale);
                r.w32(L.ret_val + kEbx, ebx);
                r.w32(L.ret_val + kMotionArg, mb);
                r.w32(L.ret_val + kUnitArg, unit_addr(i));
                sim::run_motion(mc, u);
                if (engine_faulted()) return true;
                if (!r.call(L.caller_motion, 0, {})) return fail("motion step");
                if (r.r32(L.ret_val + kStaleOut) != ebx) {
                    std::printf("scenario %u: motion step stale word %08x, expected the caller's EBX %08x\n", seed, r.r32(L.ret_val + kStaleOut), ebx);
                    ++st.mismatches;
                    return true;
                }
                ++st.motion_steps;
                ++((motion_types[t.type_index].type_flags & sim::motion_flags::kTypeFlies) ? st.air_steps : st.ground_steps);
                what = "motion step";
            } else {  // parts of the step, called directly
                const int part = static_cast<int>(g() % 6);
                if (part == 0) {
                    const i32 delta = static_cast<i32>(g() % 0x20000) - 0x10000;
                    sim::accelerate(mc, u, delta);
                    if (!direct(kAccelerate, mb, {unit_addr(i), static_cast<u32>(delta)}, "accelerate")) return false;
                    what = "accelerate";
                } else if (part == 1) {
                    const Vec3 imp{static_cast<i32>(g() % 0x40000) - 0x20000, static_cast<i32>(g() % 0x40000) - 0x20000,
                                   static_cast<i32>(g() % 0x40000) - 0x20000};
                    r.write(L.args, imp.data(), 12);
                    sim::lean(mc, u, imp);
                    if (!direct(kLean, mb, {unit_addr(i), L.args}, "lean")) return false;
                    what = "lean";
                } else if (part == 2) {
                    const Vec3 p = near_point(u, 0x300000);
                    const u32 layer = 1 + g() % 2;
                    sim::place_unit(mc, u, p, layer);
                    if (!direct(kPlace, 0, {unit_addr(i), static_cast<u32>(p[0]), static_cast<u32>(p[1]), static_cast<u32>(p[2]), layer}, "place"))
                        return false;
                    what = "place";
                } else if (part == 3) {
                    sim::notify_move_rate(mc, u);
                    if (!direct(kMoveRate, mb, {unit_addr(i)}, "move rate")) return false;
                    what = "move rate";
                } else if (part == 4) {
                    sim::notify_water_occupancy(mc, u);
                    if (!direct(kWaterOcc, 0, {unit_addr(i)}, "water occupancy")) return false;
                    what = "water occupancy";
                } else {
                    sim::move_unit(mc, u);
                    if (!direct(kMoveUnit, mb, {unit_addr(i)}, "move")) return false;
                    what = "move";
                }
            }
            ++st.ops;
            ++st.by_op[what];
            if (std::getenv("MH_VERBOSE")) std::printf("  tick %d: %s on unit %d\n", tick, what.c_str(), i);
            if (engine_faulted()) return true;
            if (!compare(what + " on unit " + std::to_string(i) + " tick " + std::to_string(tick))) return true;
        }
        // One unit pass.
        for (int i : present) pre_flags[static_cast<std::size_t>(i)] = units[static_cast<std::size_t>(i)].flags;
        env.stale_word = static_cast<u32>(g());
        r.w32(L.ret_val + kStaleIn, env.stale_word);
        std::int16_t camera = 0;
        sim::run_unit_pass(units, tstate, players, env, world, camera);
        if (engine_faulted()) return true;
        if (!r.call(L.caller_pass, 0, {})) return fail("unit pass");
        const u32 theirs = r.r32(L.ret_val + kStaleOut);
        if (theirs != env.stale_word) {
            std::printf("scenario %u tick %d: unit pass stale word orig %08x ours %08x\n", seed, tick, theirs, env.stale_word);
            ++st.mismatches;
            return true;
        }
        ++st.pass_stale[env.stale_word == 30 ? "30 (water damage on)" : env.stale_word == L.gs + 0x14353 ? "live-count address" : "settle/other"];
        ++st.ticks;
        if (!compare("unit pass tick " + std::to_string(tick))) return true;
        st.pass_steps += world.pass_steps;
        st.routed += world.routed;
        st.flying += world.flying;
        st.carried += world.carried;
        st.place_asks += world.place_asks;
        st.refused += world.refused;
        st.rate_changes += world.rate_changes;
        world.pass_steps = world.routed = world.flying = world.carried = world.place_asks = world.refused = world.rate_changes = 0;
    }
    for (int i : present)
        for (auto& o : orders[static_cast<std::size_t>(i)]) st.events += (o->events & sim::goal_events::kAll) != 0;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: motion_harness <TotalA.exe> <game_dir> [scenarios] [ticks] [first seed]\n");
        return 2;
    }
    const int scenarios = argc > 3 ? std::atoi(argv[3]) : 20;
    const int ticks = argc > 4 ? std::atoi(argv[4]) : 30;
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
        if (d.max_velocity == 0 && !(d.type_flags & sim::motion_flags::kTypeFlies)) continue;  // mobile units only
        LoadedType t;
        t.def = &d;
        std::string err;
        if (!vfs.read("objects3d\\" + d.object_name + ".3DO", t.model_file) || !vfs.read("scripts\\" + d.unit_name + ".COB", t.cob_file))
            continue;
        const auto m = ta::load_3do(t.model_file, true);
        if (!m.root || !ta::load_cob(t.cob_file, t.cob, err)) continue;
        t.pose_model = ta::to_pose_model(m.root.get());
        t.settle.has_footprint = ta::selection_footprint(t.model_file, m.root.get(), t.settle.footprint);
        t.info.max_damage = d.max_damage ? d.max_damage : 1;
        t.info.model_max_y = d.model_max_y;
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
        if (!run_scenario(r, L, types, hm, ticks, static_cast<unsigned>(first + s), st)) {
            ++st.failures;
            std::printf("scenario %d: original call failed - restarting\n", first + s);
            r.stop();
            if (!r.start(argv[1]) || !setup(r, L, hm)) return 1;
        }
    }
    std::printf("mobile unit types %zu\n", types.size());
    std::printf("scenarios %zu, unit passes %zu, operations %zu, direct motion steps %zu (ground %zu, air %zu)\n", st.scenarios, st.ticks, st.ops,
                st.motion_steps, st.ground_steps, st.air_steps);
    std::printf("pass motion steps %zu: ground with a route %zu, flying %zu, carried %zu; occupancy asks %zu (%zu refused, with direct calls); "
                "move-rate changes %zu; orders left with goal events %zu\n",
                st.pass_steps, st.routed, st.flying, st.carried, st.place_asks, st.refused, st.rate_changes, st.events);
    std::printf("operations:\n");
    for (const auto& [k, v] : st.by_op) std::printf("  %s: %zu\n", k.c_str(), v);
    std::printf("unit pass, stale word left:\n");
    for (const auto& [k, v] : st.pass_stale) std::printf("  %s: %zu\n", k.c_str(), v);
    std::printf("mismatching scenarios %zu, engine faults %zu, original failures %zu\n", st.mismatches, st.faults, st.failures);
    return st.mismatches || st.failures ? 1 : 0;
}
