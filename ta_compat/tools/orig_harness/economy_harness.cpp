// Differential tester for the economy (sim/economy.h). Inside a suspended TotalA.exe it
// builds a fake game state with ten player records, their own resource slots, a unit table
// and unit definitions full of random resource values, and logging stubs for the activate
// routine (the settlement's cloak decision) and the network packets. Then it runs random
// operations on the original and on the engine and compares after each one:
//   - the settlement (0x401360) of a random player,
//   - the slot bookings (0x4011C0, 0x401180) and immediate spending (0x401220, 0x401260),
//   - transfers sent and received (0x464B30, 0x464C60),
//   - automatic sharing in a network game (0x457D30),
//   - the wind stage (0x490C40), with the battle RNG and rand() streams,
// plus random changes to units, players and the environment between operations.
// Compared: every economy field of every player record and its own slot, every unit's
// slot, the call log, both random streams and the wind.
//
// Usage: economy_harness <TotalA.exe> [scenarios] [operations] [first seed]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/economy.h"
#include "sim/rng.h"

namespace {

using harness::u32;
using i32 = std::int32_t;
using u16 = std::uint16_t;
using u8 = std::uint8_t;

constexpr u32 kGameStatePtr = 0x00511DE8, kBattleRngState = 0x0051FC88;
constexpr u32 kSettle = 0x00401360, kAddUse = 0x004011C0, kAddEnergyUse = 0x00401180, kConsumeEnergy = 0x00401220,
              kConsumeMetal = 0x00401260, kTransferEnergy = 0x00464B30, kTransferMetal = 0x00464C60, kAutoShare = 0x00457D30,
              kWind = 0x00490C40;
constexpr u32 kUnitSize = 0x118, kPlayerSize = 0x14B, kDefSize = 0x249, kSlotSize = 0x34;
constexpr int kUnits = 48, kTypes = 12;

// Game state offsets.
enum : u32 {
    gPlayers = 0x1B63, gNetFlags = 0x2A44, gWindMin = 0x1425B, gWindMax = 0x1425F, gTidal = 0x14267, gWindNext = 0x37EC4,
    gWindDenom = 0x37EC8, gWindVelX = 0x37ECC, gWindVelY = 0x37ED0, gWindVelZ = 0x37ED4, gWindHeading = 0x37ED8,
    gWindSpeed = 0x37EDA, gWindFraction = 0x37EDE, gWindChanged = 0x37EE2, gAiDifficulty = 0x37EEE, gTick = 0x38A47,
};
// Player record offsets.
enum : u32 {
    pActive = 0x00, pNetId = 0x04, pSide = 0x27, pFirst = 0x67, pLast = 0x6B, pController = 0x73, pEnergy = 0x8C,
    pEnergyIncome = 0x90, pEnergyUsage = 0x94, pMetal = 0x98, pMetalIncome = 0x9C, pMetalUsage = 0xA0, pEnergyStorage = 0xA4,
    pMetalStorage = 0xA8, pEnergyProduced = 0xAC, pMetalProduced = 0xB4, pEnergyConsumed = 0xBC, pMetalConsumed = 0xC4,
    pEnergyWasted = 0xCC, pMetalWasted = 0xD4, pEnergyBonus = 0xDC, pMetalBonus = 0xE0, pShareMetal = 0xE4, pShareEnergy = 0xE8,
    pOwnSlot = 0xEC, pAllied = 0x108, pCreated = 0x140, pAlive = 0x144, pState = 0x146, pFlags = 0x149,
};
// Unit offsets.
enum : u32 {
    uExtract = 0x58, uDef = 0x92, uHoldoff = 0xB0, uSlot = 0xBC, uSlotPlayer = 0xEC, uBuildLeft = 0x104, uActivate = 0x10E,
    uFlags = 0x110,
};
// Unit definition offsets.
enum : u32 {
    dEnergyMake = 0x1C2, dEnergyUse = 0x1C6, dMetalMake = 0x1CA, dExtracts = 0x1CE, dWind = 0x1D2, dTidal = 0x1D6, dCloak = 0x1DA,
    dCloakMoving = 0x1DE, dEnergyStorage = 0x1E2, dMetalStorage = 0x1E6, dMakesMetal = 0x22D,
};

u32 fbits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}
std::uint64_t dbits(double d) {
    std::uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8_(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
};

// Logs {id, ECX, first `logged` stack arguments} at *log_ptr, then returns popping `args` arguments.
void emit_stub(Emitter& e, u32 id, int logged, int args, u32 log_ptr) {
    e.u8_(0xA1); e.u32le(log_ptr);                       // mov eax,[log_ptr]
    e.u8_(0xC7); e.u8_(0x00); e.u32le(id);               // mov dword [eax],id
    e.u8_(0x89); e.u8_(0x48); e.u8_(0x04);               // mov [eax+4],ecx
    for (int i = 0; i < logged; ++i) {
        e.u8_(0x8B); e.u8_(0x54); e.u8_(0x24); e.u8_(static_cast<std::uint8_t>(4 + 4 * i));  // mov edx,[esp+4+4i]
        e.u8_(0x89); e.u8_(0x50); e.u8_(static_cast<std::uint8_t>(8 + 4 * i));              // mov [eax+8+4i],edx
    }
    e.u8_(0x83); e.u8_(0x05); e.u32le(log_ptr); e.u8_(static_cast<std::uint8_t>(4 * (logged + 2)));  // add [log_ptr],n
    e.u8_(0x33); e.u8_(0xC0);                            // xor eax,eax
    e.u8_(0xC2); e.u8_(static_cast<std::uint8_t>(4 * args)); e.u8_(0x00);
}

enum : u32 { sActivate = 1, sEnergyPacket = 2, sMetalPacket = 3, sNetSend = 4 };

struct Layout {
    u32 stubs = 0, log_ptr = 0, log_base = 0, rand_state = 0, gs = 0, units = 0, defs = 0, slots = 0, sides = 0;
    u32 player(int p) const { return gs + gPlayers + static_cast<u32>(p) * kPlayerSize; }
    u32 unit(int id) const { return units + static_cast<u32>(id) * kUnitSize; }
    u32 def(int k) const { return defs + static_cast<u32>(k) * kDefSize; }
    u32 slot(int p) const { return slots + static_cast<u32>(p) * 0x40; }
    u32 side(int p) const { return sides + static_cast<u32>(p) * 0x100; }
};

bool setup(harness::Remote& r, Layout& L) {
    L.stubs = r.alloc(0x1000, PAGE_EXECUTE_READWRITE);
    L.log_ptr = r.alloc(0x1000);
    L.log_base = r.alloc(1u << 20);
    L.rand_state = L.log_ptr + 0x100;
    L.gs = r.alloc(0x40000);
    L.units = r.alloc(static_cast<SIZE_T>(kUnits) * kUnitSize);
    L.defs = r.alloc(static_cast<SIZE_T>(kTypes) * kDefSize + 0x100);
    L.slots = r.alloc(0x1000);
    L.sides = r.alloc(0x1000);
    if (!L.stubs || !L.log_base || !L.gs || !L.units || !L.defs || !L.slots || !L.sides) return false;
    Emitter e;
    struct S {
        u32 at, id;
        int logged, args;
    };
    const S stubs[] = {
        {0x0048B090, sActivate, 2, 2},      // Unit_SetActivateState(unit; mask, on) thiscall
        {0x00456EE0, sEnergyPacket, 3, 3},  // energy transfer packet (from, to, amount)
        {0x00457050, sMetalPacket, 3, 3},   // metal transfer packet (from, to, amount)
        {0x00451BC0, sNetSend, 2, 4},       // packet send (from net id, to net id, buffer, size)
    };
    for (const S& s : stubs) {
        const u32 at = L.stubs + static_cast<u32>(e.b.size());
        emit_stub(e, s.id, s.logged, s.args, L.log_ptr);
        while (e.b.size() % 16) e.u8_(0xCC);
        if (!r.patch_jump(s.at, at)) return false;
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
    if (!r.write(L.stubs, e.b.data(), e.b.size())) return false;
    r.w32(L.log_ptr, L.log_base);
    r.w32(kGameStatePtr, L.gs);
    return true;
}

struct Stats {
    long scenarios = 0, ops = 0, mismatches = 0, failures = 0;
    long settles = 0, settled_units = 0, stalled_slots = 0, short_settles = 0, wasted = 0, cloak_calls = 0, cloak_paid = 0;
    long add_uses = 0, add_uses_refused = 0, consumes = 0, transfers = 0, transfers_sent = 0, shares = 0, share_packets = 0,
         map_shares = 0, winds = 0, wind_changes = 0;
};

// The engine's view of the stubbed routines.
struct Hooks final : sim::EconomySystems {
    std::vector<u32> log;
    u32 unit_table = 0;
    long* cloak_calls = nullptr;
    void set_cloaked(sim::Unit& u, bool on) override {
        log.insert(log.end(), {sActivate, unit_table + u.id * kUnitSize, 4u, on ? 1u : 0u});
        ++*cloak_calls;
    }
};

struct Scenario {
    harness::Remote& r;
    const Layout& L;
    std::mt19937& g;
    Stats& st;
    std::vector<sim::Unit> units;
    std::vector<sim::UnitTickState> ticks;
    std::vector<sim::PlayerSlot> players;
    std::vector<sim::PlayerEconomy> accounts;
    std::vector<sim::EconomyType> types;
    std::vector<std::array<std::uint8_t, 16>> alliances;
    sim::EconomyEnvironment env;
    sim::WindState wind;
    sim::BattleRng rng;
    sim::Lcg15 crt;
    Hooks hooks;
    u32 tick = 0;

    Scenario(harness::Remote& rr, const Layout& l, std::mt19937& gg, Stats& s) : r(rr), L(l), g(gg), st(s) {}

    sim::Economy economy() { return sim::Economy{units, ticks, players, accounts, types, env, hooks}; }

    // Random values with plenty of zeros, exact ties and stalls.
    float amount(u32 scale) {
        switch (g() % 6) {
        case 0: return 0.0f;
        case 1: return static_cast<float>(g() % 50);
        case 2: return static_cast<float>(g() % (scale + 1)) / 7.0f;
        case 3: return static_cast<float>(g() % (scale * 10 + 1)) / 3.0f;
        default: return static_cast<float>(g() % (scale + 1)) / 9.0f;
        }
    }

    void write_unit(int id) {
        const sim::Unit& u = units[static_cast<std::size_t>(id)];
        const sim::UnitTickState& t = ticks[static_cast<std::size_t>(id)];
        const u32 at = L.unit(id);
        r.w32(at + uFlags, u.flags);
        r.write(at + uActivate, &u.activate_flags, 1);
        r.write(at + uBuildLeft, &u.build_fraction_left, 4);
        r.w32(at + uHoldoff, u.cloak_holdoff_tick);
        r.write(at + uExtract, &u.metal_extraction, 4);
        r.w32(at + uDef, L.def(t.type_index));
        const float slot[12] = {t.energy_make, t.energy_use, t.energy_use_paid, t.energy_stall, t.energy_make_prev, t.energy_use_prev,
                                t.metal_make,  t.metal_use,  t.metal_use_paid,  t.metal_stall,  t.metal_make_prev,  t.metal_use_prev};
        r.write(at + uSlot, slot, sizeof slot);
        r.w32(at + uSlotPlayer, L.player(t.owner));
    }
    void write_own_slot(int p) {
        const sim::ResourceSlot& s = accounts[static_cast<std::size_t>(p)].own;
        const float f[12] = {s.energy_make, s.energy_use, s.energy_use_paid, s.energy_stall, s.energy_make_prev, s.energy_use_prev,
                             s.metal_make,  s.metal_use,  s.metal_use_paid,  s.metal_stall,  s.metal_make_prev,  s.metal_use_prev};
        r.write(L.slot(p), f, sizeof f);
        r.w32(L.slot(p) + 0x30, L.player(s.player));
    }
    void write_player(int p) {
        const sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        const sim::PlayerEconomy& a = accounts[static_cast<std::size_t>(p)];
        const u32 at = L.player(p);
        r.w32(at + pActive, s.active ? 1u : 0u);
        r.w32(at + pNetId, static_cast<u32>(s.net_id));
        r.write(at + pController, &s.controller, 1);
        r.write(at + pState, &s.state, 1);
        r.write(at + pFlags, &s.flags, 2);
        r.w32(at + pCreated, s.units_created);
        r.write(at + pAlive, &s.alive_count, 2);
        r.w32(at + pFirst, L.unit(s.first_unit));
        r.w32(at + pLast, L.unit(s.last_unit));
        r.write(at + pEnergy, &s.energy_stored, 4);
        r.write(at + pMetal, &s.metal_stored, 4);
        const float f[8] = {a.energy_income, a.energy_usage, 0, a.metal_income, a.metal_usage, a.energy_storage, a.metal_storage, 0};
        r.write(at + pEnergyIncome, &f[0], 8);
        r.write(at + pMetalIncome, &f[3], 16);
        const double d[6] = {a.energy_produced, a.metal_produced, a.energy_consumed, a.metal_consumed, a.energy_wasted, a.metal_wasted};
        r.write(at + pEnergyProduced, d, sizeof d);
        r.write(at + pEnergyBonus, &a.energy_storage_bonus, 4);
        r.write(at + pMetalBonus, &a.metal_storage_bonus, 4);
        r.write(at + pShareMetal, &a.share_metal_above, 4);
        r.write(at + pShareEnergy, &a.share_energy_above, 4);
        r.w32(at + pOwnSlot, L.slot(p));
        r.w32(at + pSide, L.side(p));
        r.write(L.side(p) + 0x94, &a.side_controller, 1);
        r.write(L.side(p) + 0x97, &a.share_flags, 1);
        r.write(at + pAllied, alliances[static_cast<std::size_t>(p)].data(), 10);
        write_own_slot(p);
    }
    void write_def(int k) {
        const sim::EconomyType& t = types[static_cast<std::size_t>(k)];
        const u32 at = L.def(k);
        const float f[10] = {t.energy_make, t.energy_use, t.metal_make, t.extracts_metal, t.wind_generator, t.tidal_generator,
                             t.cloak_cost,  t.cloak_cost_moving, t.energy_storage, t.metal_storage};
        r.write(at + dEnergyMake, f, sizeof f);
        r.write(at + dMakesMetal, &t.makes_metal, 1);
    }
    void write_env() {
        r.w32(L.gs + gTick, tick);
        r.w32(L.gs + gAiDifficulty, static_cast<u32>(env.ai_difficulty));
        r.write(L.gs + gWindFraction, &wind.fraction, 4);
        r.write(L.gs + gTidal, &env.tidal_strength, 4);
        u8 net = 1;
        r.write(L.gs + gNetFlags, &net, 1);
        r.w32(L.gs + gWindNext, wind.next_change_tick);
        r.w32(L.gs + gWindDenom, static_cast<u32>(wind.denominator));
        r.w32(L.gs + gWindMin, static_cast<u32>(wind.min_speed));
        r.w32(L.gs + gWindMax, static_cast<u32>(wind.max_speed));
        r.w32(L.gs + gWindSpeed, static_cast<u32>(wind.speed));
        r.write(L.gs + gWindHeading, &wind.heading, 2);
        r.w32(L.gs + gWindVelX, static_cast<u32>(wind.velocity[0]));
        r.w32(L.gs + gWindVelY, static_cast<u32>(wind.velocity[1]));
        r.w32(L.gs + gWindVelZ, static_cast<u32>(wind.velocity[2]));
        r.w32(L.gs + gWindChanged, wind.changed ? 1u : 0u);
        r.w32(kBattleRngState, rng.state());
        r.w32(L.rand_state, crt.state());
    }

    void random_type(int k) {
        sim::EconomyType& t = types[static_cast<std::size_t>(k)];
        t = sim::EconomyType{};
        t.energy_make = g() % 2 ? amount(2000) : 0.0f;
        t.energy_use = g() % 3 ? amount(400) : 0.0f;
        if (g() % 4 == 0) t.energy_use = -t.energy_use;
        t.metal_make = g() % 3 == 0 ? amount(30) : 0.0f;
        switch (g() % 5) {
        case 0: t.extracts_metal = static_cast<float>(g() % 2000) / 1000.0f; break;
        case 1: t.makes_metal = static_cast<u8>(g() % 3); break;
        case 2: t.wind_generator = amount(600); break;
        case 3: t.tidal_generator = amount(600); break;
        default: break;
        }
        if (g() % 3 == 0) {
            t.cloak_cost = amount(300);
            t.cloak_cost_moving = g() % 2 ? amount(600) : static_cast<float>(static_cast<i32>(t.cloak_cost));
        }
        t.energy_storage = g() % 3 == 0 ? amount(6000) : 0.0f;
        t.metal_storage = g() % 3 == 0 ? amount(3000) : 0.0f;
    }
    void random_unit(int id, int owner) {
        sim::Unit& u = units[static_cast<std::size_t>(id)];
        sim::UnitTickState& t = ticks[static_cast<std::size_t>(id)];
        u.id = static_cast<u16>(id);
        u.flags = (g() % 8 ? sim::unit_flags::kAlive : 0u) | (g() % 2 ? 0x20000000u : 0u) | (g() % 4 == 0 ? 0x4u : 0u) |
                  (g() % 6 == 0 ? 0x8u : 0u) | (g() % 3 == 0 ? 0x800u : 0u) | (g() % 8 == 0 ? 0x1000u : 0u) | (g() & 0x01030000u);
        u.activate_flags = static_cast<u8>(g() % 3 ? 1 : 0);
        u.build_fraction_left = g() % 5 == 0 ? static_cast<float>(g() % 1000) / 999.0f : 0.0f;
        u.cloak_holdoff_tick = g() % 4 == 0 ? tick + g() % 60 - 20 : 0u;
        u.metal_extraction = static_cast<float>(g() % 40) * 0.37f;
        t.type_index = static_cast<u16>(g() % kTypes);
        t.owner = g() % 10 == 0 ? static_cast<int>(g() % 10) : owner;
        auto f = [&](u32 scale) { return g() % 2 ? amount(scale) : 0.0f; };
        t.energy_make = f(500); t.energy_use = f(500); t.energy_use_paid = f(500); t.energy_stall = g() % 3 == 0 ? f(800) : 0.0f;
        t.energy_make_prev = f(50); t.energy_use_prev = f(50);
        t.metal_make = f(50); t.metal_use = f(50); t.metal_use_paid = f(50); t.metal_stall = g() % 3 == 0 ? f(80) : 0.0f;
        t.metal_make_prev = f(50); t.metal_use_prev = f(50);
    }
    void random_player(int p, int first, int last) {
        sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
        sim::PlayerEconomy& a = accounts[static_cast<std::size_t>(p)];
        s = sim::PlayerSlot{};
        a = sim::PlayerEconomy{};
        s.active = g() % 6 != 0;
        s.controller = static_cast<u8>(g() % 5 == 0 ? g() % 5 : 1 + g() % 3);
        s.state = static_cast<u8>(g() % 12 == 0 ? 10 : (g() % 8 == 0 ? g() % 10 : p));
        s.net_id = static_cast<i32>(100 + p);
        s.flags = static_cast<u16>(g() % 3 ? 1 : 0);
        s.units_created = g() % 4 ? 5 + g() % 20 : 0;
        s.alive_count = static_cast<u16>(g() % 4 ? 1 + g() % 9 : 0);
        s.first_unit = first;
        s.last_unit = last;
        s.energy_stored = amount(20000);
        s.metal_stored = amount(3000);
        a.energy_income = amount(100); a.energy_usage = amount(100); a.metal_income = amount(10); a.metal_usage = amount(10);
        a.energy_storage = amount(20000); a.metal_storage = amount(3000);
        a.energy_produced = static_cast<double>(g() % 100000) / 3.0; a.metal_produced = static_cast<double>(g() % 10000) / 7.0;
        a.energy_consumed = static_cast<double>(g() % 100000) / 11.0; a.metal_consumed = static_cast<double>(g() % 10000) / 13.0;
        a.energy_wasted = static_cast<double>(g() % 1000) / 3.0; a.metal_wasted = static_cast<double>(g() % 1000) / 9.0;
        a.energy_storage_bonus = static_cast<float>(g() % 3 ? 200 + g() % 3000 : g() % 200);
        a.metal_storage_bonus = static_cast<float>(g() % 3 ? 200 + g() % 1000 : g() % 200);
        a.share_metal_above = amount(1000);
        a.share_energy_above = amount(5000);
        a.share_flags = static_cast<u8>(g() & 0x26);
        a.side_controller = static_cast<u8>(g() % 3 ? 1 : g() % 3);
        a.own = sim::ResourceSlot{};
        a.own.player = s.state < 10 ? s.state : p;
        a.own.energy_make = g() % 3 == 0 ? amount(300) : 0.0f;
        a.own.metal_make = g() % 3 == 0 ? amount(30) : 0.0f;
        a.own.energy_stall = g() % 6 == 0 ? amount(30) : 0.0f;
        for (int i = 0; i < 10; ++i) alliances[static_cast<std::size_t>(p)][static_cast<std::size_t>(i)] = static_cast<u8>(g() % 2);
    }

    // Reads the original's state and compares it with the engine's.
    bool compare(const char* what) {
        bool ok = true;
        auto bad = [&](const std::string& m) {
            if (ok) std::printf("  mismatch after %s (tick %u):\n", what, tick);
            ok = false;
            std::printf("    %s\n", m.c_str());
        };
        for (int p = 0; p < 10; ++p) {
            const sim::PlayerSlot& s = players[static_cast<std::size_t>(p)];
            const sim::PlayerEconomy& a = accounts[static_cast<std::size_t>(p)];
            const u32 at = L.player(p);
            const std::string pn = "player " + std::to_string(p) + " ";
            auto cf = [&](u32 off, float v, const char* n) {
                const u32 o = r.r32(at + off);
                if (o != fbits(v)) {
                    float of;
                    std::memcpy(&of, &o, 4);
                    bad(pn + n + ": original " + std::to_string(of) + " engine " + std::to_string(v));
                }
            };
            auto cd = [&](u32 off, double v, const char* n) {
                if (r.rd<std::uint64_t>(at + off) != dbits(v)) bad(pn + n + ": original " + std::to_string(r.rd<double>(at + off)) + " engine " + std::to_string(v));
            };
            cf(pEnergy, s.energy_stored, "energy");
            cf(pMetal, s.metal_stored, "metal");
            cf(pEnergyIncome, a.energy_income, "energy income");
            cf(pEnergyUsage, a.energy_usage, "energy usage");
            cf(pMetalIncome, a.metal_income, "metal income");
            cf(pMetalUsage, a.metal_usage, "metal usage");
            cf(pEnergyStorage, a.energy_storage, "energy storage");
            cf(pMetalStorage, a.metal_storage, "metal storage");
            cd(pEnergyProduced, a.energy_produced, "energy produced");
            cd(pMetalProduced, a.metal_produced, "metal produced");
            cd(pEnergyConsumed, a.energy_consumed, "energy consumed");
            cd(pMetalConsumed, a.metal_consumed, "metal consumed");
            cd(pEnergyWasted, a.energy_wasted, "energy wasted");
            cd(pMetalWasted, a.metal_wasted, "metal wasted");
            if (r.rd<u16>(at + pFlags) != s.flags) bad(pn + "flags");
            const sim::ResourceSlot& o = a.own;
            const float f[12] = {o.energy_make, o.energy_use, o.energy_use_paid, o.energy_stall, o.energy_make_prev, o.energy_use_prev,
                                 o.metal_make,  o.metal_use,  o.metal_use_paid,  o.metal_stall,  o.metal_make_prev,  o.metal_use_prev};
            for (int k = 0; k < 12; ++k)
                if (r.r32(L.slot(p) + 4u * static_cast<u32>(k)) != fbits(f[k])) bad(pn + "own slot field " + std::to_string(k));
        }
        for (int id = 1; id < kUnits; ++id) {
            const sim::UnitTickState& t = ticks[static_cast<std::size_t>(id)];
            const float f[12] = {t.energy_make, t.energy_use, t.energy_use_paid, t.energy_stall, t.energy_make_prev, t.energy_use_prev,
                                 t.metal_make,  t.metal_use,  t.metal_use_paid,  t.metal_stall,  t.metal_make_prev,  t.metal_use_prev};
            for (int k = 0; k < 12; ++k)
                if (r.r32(L.unit(id) + uSlot + 4u * static_cast<u32>(k)) != fbits(f[k]))
                    bad("unit " + std::to_string(id) + " slot field " + std::to_string(k));
        }
        // Call log. Packet routines get their player indices as whole registers whose upper
        // bytes are left over, and ECX is whatever the caller had: only meaningful parts compare.
        const u32 end = r.r32(L.log_ptr);
        std::vector<u32> orig((end - L.log_base) / 4);
        if (!orig.empty()) r.read(L.log_base, orig.data(), orig.size() * 4);
        for (std::size_t i = 0; i < orig.size();) {
            const u32 id = orig[i];
            if (id == sActivate) i += 4;
            else if (id == sEnergyPacket || id == sMetalPacket) {
                if (i + 5 > orig.size()) break;
                orig[i + 1] = 0;
                orig[i + 2] &= 0xFF;
                orig[i + 3] &= 0xFF;
                i += 5;
            } else if (id == sNetSend) {
                if (i + 4 > orig.size()) break;
                orig[i + 1] = 0;
                i += 4;
            } else break;
        }
        if (orig != hooks.log) {
            bad("call log: original " + std::to_string(orig.size()) + " words, engine " + std::to_string(hooks.log.size()));
            for (std::size_t i = 0; i < orig.size() || i < hooks.log.size(); ++i) {
                const u32 a = i < orig.size() ? orig[i] : 0xDEADDEAD, b = i < hooks.log.size() ? hooks.log[i] : 0xDEADDEAD;
                if (a != b) {
                    std::printf("      word %zu: original %08X engine %08X\n", i, a, b);
                    break;
                }
            }
        }
        r.w32(L.log_ptr, L.log_base);
        hooks.log.clear();
        if (r.r32(kBattleRngState) != rng.state()) bad("battle RNG");
        if (r.r32(L.rand_state) != crt.state()) bad("rand() state");
        if (r.r32(L.gs + gWindNext) != wind.next_change_tick) bad("wind next change");
        if (static_cast<i32>(r.r32(L.gs + gWindSpeed)) != wind.speed) bad("wind speed");
        if (r.rd<u16>(L.gs + gWindHeading) != wind.heading) bad("wind heading");
        if (static_cast<i32>(r.r32(L.gs + gWindVelX)) != wind.velocity[0] || static_cast<i32>(r.r32(L.gs + gWindVelZ)) != wind.velocity[2])
            bad("wind velocity");
        if (r.r32(L.gs + gWindFraction) != fbits(wind.fraction)) bad("wind fraction");
        if ((r.r32(L.gs + gWindChanged) != 0) != wind.changed) bad("wind changed");
        if (!ok) ++st.mismatches;
        return ok;
    }

    bool run(int ops) {
        units.clear();
        units.resize(kUnits);
        ticks.assign(kUnits, sim::UnitTickState{});
        players.assign(11, sim::PlayerSlot{});
        accounts.assign(11, sim::PlayerEconomy{});
        types.assign(kTypes, sim::EconomyType{});
        alliances.assign(11, {});
        hooks.unit_table = L.units;
        hooks.cloak_calls = &st.cloak_calls;
        tick = 1000 + g() % 100000;
        env.ai_difficulty = static_cast<i32>(g() % 4);
        env.tidal_strength = g() % 3 ? static_cast<float>(g() % 100) / 37.0f : 0.0f;
        wind = sim::WindState{};
        wind.min_speed = static_cast<i32>(g() % 3 ? g() % 500 : g() % 3000);
        wind.max_speed = static_cast<i32>(g() % 4 ? wind.min_speed + g() % 5000 : g() % 300);
        wind.denominator = g() % 10 ? 5000 : static_cast<i32>(g() % 3000);
        wind.next_change_tick = tick - 5 + g() % 10;
        wind.fraction = static_cast<float>(g() % 1000) / 999.0f;
        rng.set_state(g() | 1u);
        crt.set_state(g());
        for (int k = 0; k < kTypes; ++k) {
            random_type(k);
            write_def(k);
        }
        // Units 1..40 split over players 0..3; the others own none.
        for (int p = 0; p < 10; ++p) {
            const int first = p < 4 ? 1 + 10 * p : kUnits;
            const int last = p < 4 ? 10 + 10 * p : kUnits - 1;
            random_player(p, first, last);
        }
        for (int p = 0; p < 10; ++p) {
            if (players[static_cast<std::size_t>(p)].first_unit > players[static_cast<std::size_t>(p)].last_unit) continue;
            for (int id = players[static_cast<std::size_t>(p)].first_unit; id <= players[static_cast<std::size_t>(p)].last_unit; ++id)
                random_unit(id, p);
        }
        // Empty ranges: the original compares unit addresses, the engine ids.
        for (int p = 0; p < 10; ++p) write_player(p);
        for (int id = 1; id < kUnits; ++id) write_unit(id);
        write_env();
        if (!compare("setup")) return true;
        for (int op = 0; op < ops; ++op) {
            ++st.ops;
            sim::Economy e = economy();
            env.game_tick = tick;
            env.wind_fraction = wind.fraction;
            const u32 kind = g() % 20;
            const char* what = "?";
            if (kind < 7) {
                what = "settlement";
                const int p = static_cast<int>(g() % 5);
                ++st.settles;
                for (int id = players[static_cast<std::size_t>(p)].first_unit; id <= players[static_cast<std::size_t>(p)].last_unit; ++id)
                    if (units[static_cast<std::size_t>(id)].flags & sim::unit_flags::kAlive) ++st.settled_units;
                const float before_e = players[static_cast<std::size_t>(p)].energy_stored;
                const double before_waste = accounts[static_cast<std::size_t>(p)].energy_wasted + accounts[static_cast<std::size_t>(p)].metal_wasted;
                sim::settle_player(e, p);
                if (!r.call(kSettle, 0, {L.player(p)})) return fail(what);
                const auto& a = accounts[static_cast<std::size_t>(p)];
                if (players[static_cast<std::size_t>(p)].energy_stored < before_e) ++st.short_settles;
                if (a.energy_wasted + a.metal_wasted != before_waste) ++st.wasted;
                for (int id = 1; id < kUnits; ++id) st.stalled_slots += ticks[static_cast<std::size_t>(id)].energy_stall > 0.0f;
            } else if (kind < 10) {
                what = "slot booking";
                const int id = 1 + static_cast<int>(g() % (kUnits - 1));
                const float en = amount(50), me = amount(5);
                ++st.add_uses;
                u32 res = 0;
                bool ours;
                if (g() % 2) {
                    ours = sim::add_use_if_not_stalled(ticks[static_cast<std::size_t>(id)], en, me);
                    if (!r.call(kAddUse, L.unit(id) + uSlot, {fbits(en), fbits(me)}, &res)) return fail(what);
                } else {
                    ours = sim::add_energy_use_if_not_stalled(ticks[static_cast<std::size_t>(id)], en);
                    if (!r.call(kAddEnergyUse, 0, {L.unit(id) + uSlot, fbits(en)}, &res)) return fail(what);
                }
                st.add_uses_refused += !ours;
                if ((res != 0) != ours) {
                    std::printf("  booking result differs (unit %d)\n", id);
                    ++st.mismatches;
                }
            } else if (kind < 12) {
                what = "immediate spending";
                const int p = static_cast<int>(g() % 10);
                sim::ResourceSlot& s = accounts[static_cast<std::size_t>(p)].own;
                const float am = amount(5000);
                u32 res = 0;
                bool ours;
                ++st.consumes;
                if (g() % 2) {
                    ours = sim::try_consume_energy(players[static_cast<std::size_t>(s.player)], s, am);
                    if (!r.call(kConsumeEnergy, L.slot(p), {fbits(am)}, &res)) return fail(what);
                } else {
                    ours = sim::try_consume_metal(players[static_cast<std::size_t>(s.player)], s, am);
                    if (!r.call(kConsumeMetal, L.slot(p), {fbits(am)}, &res)) return fail(what);
                }
                if ((res != 0) != ours) {
                    std::printf("  spending result differs (player %d)\n", p);
                    ++st.mismatches;
                }
            } else if (kind < 14) {
                what = "transfer";
                const int from = g() % 12 == 0 ? 10 : static_cast<int>(g() % 10), to = g() % 12 == 0 ? 10 : static_cast<int>(g() % 10);
                const float am = g() % 8 == 0 ? 0.0f : amount(5000);
                const bool send = g() % 2;
                const bool energy = g() % 2;
                ++st.transfers;
                const float sent = energy ? sim::transfer_energy(e, from, to, am, send) : sim::transfer_metal(e, from, to, am, send);
                if (send && sent != 0.0f) {
                    hooks.log.insert(hooks.log.end(), {energy ? sEnergyPacket : sMetalPacket, 0u, static_cast<u32>(from), static_cast<u32>(to), fbits(sent)});
                    ++st.transfers_sent;
                }
                if (!r.call(energy ? kTransferEnergy : kTransferMetal, 0, {static_cast<u32>(from), static_cast<u32>(to), fbits(am), send ? 1u : 0u}))
                    return fail(what);
            } else if (kind < 16) {
                what = "automatic sharing";
                const int p = static_cast<int>(g() % 10);
                if (g() % 2) {
                    // A sharing player and remote allies that may receive.
                    accounts[static_cast<std::size_t>(p)].share_flags = 0x26;
                    r.write(L.side(p) + 0x97, &accounts[static_cast<std::size_t>(p)].share_flags, 1);
                    for (int i = 0; i < 10; ++i)
                        if (g() % 3 == 0) {
                            players[static_cast<std::size_t>(i)].controller = 3;
                            r.write(L.player(i) + pController, &players[static_cast<std::size_t>(i)].controller, 1);
                        }
                }
                if (g() % 2) tick = (tick / 60 + 1) * 60 * (g() % 4 == 0 ? 15 : 1);  // a sharing step (sometimes a map-sharing one too)
                env.game_tick = tick;
                r.w32(L.gs + gTick, tick);
                ++st.shares;
                sim::auto_share(
                    e, p, alliances,
                    [&](int k, int to, float am) {
                        hooks.log.insert(hooks.log.end(), {k == 1 ? sEnergyPacket : sMetalPacket, 0u, static_cast<u32>(players[static_cast<std::size_t>(p)].state),
                                                           static_cast<u32>(to), fbits(am)});
                        ++st.share_packets;
                    },
                    [&](int to) {
                        const sim::PlayerSlot& a = players[static_cast<std::size_t>(players[static_cast<std::size_t>(p)].state)];
                        const sim::PlayerSlot& b = players[static_cast<std::size_t>(players[static_cast<std::size_t>(to)].state)];
                        const u32 from_id = a.controller ? static_cast<u32>(a.net_id) : 0xFFFFFFFFu;
                        const u32 to_id = b.controller ? static_cast<u32>(b.net_id) : 0xFFFFFFFFu;
                        hooks.log.insert(hooks.log.end(), {sNetSend, 0u, from_id, to_id});
                        ++st.map_shares;
                    });
                if (!r.call(kAutoShare, 0, {L.player(p)})) return fail(what);
            } else if (kind < 18) {
                what = "wind";
                tick += g() % 200;
                r.w32(L.gs + gTick, tick);
                ++st.winds;
                sim::step_wind(wind, tick, rng, crt);
                st.wind_changes += wind.changed;
                if (!r.call(kWind, 0, {})) return fail(what);
            } else {
                what = "changes";
                // Random changes written to both sides.
                const int id = 1 + static_cast<int>(g() % (kUnits - 1));
                sim::Unit& u = units[static_cast<std::size_t>(id)];
                switch (g() % 5) {
                case 0: u.activate_flags ^= 1; break;
                case 1: u.flags ^= (g() % 2 ? 0xCu : 0x800u); break;
                case 2: u.build_fraction_left = 0.0f; break;
                case 3: players[static_cast<std::size_t>(g() % 10)].energy_stored = amount(20000); break;
                default: tick += 1 + g() % 40; break;
                }
                write_unit(id);
                for (int p = 0; p < 10; ++p) {
                    const u32 at = L.player(p);
                    r.write(at + pEnergy, &players[static_cast<std::size_t>(p)].energy_stored, 4);
                }
                r.w32(L.gs + gTick, tick);
            }
            r.write(L.gs + gWindFraction, &wind.fraction, 4);
            if (!compare(what)) return true;
        }
        return true;
    }
    bool fail(const char* what) {
        std::printf("  the original failed in %s\n", what);
        ++st.failures;
        return false;
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: economy_harness <TotalA.exe> [scenarios] [operations] [first seed]\n");
        return 2;
    }
    const int scenarios = argc > 2 ? std::atoi(argv[2]) : 10;
    const int ops = argc > 3 ? std::atoi(argv[3]) : 100;
    const std::uint32_t first = argc > 4 ? static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 0)) : 1;
    Stats st;
    for (int s = 0; s < scenarios; ++s) {
        harness::Remote r;
        Layout L;
        if (!r.start(argv[1]) || !setup(r, L)) {
            std::fprintf(stderr, "cannot start the original\n");
            return 1;
        }
        std::mt19937 g(first + static_cast<std::uint32_t>(s));
        ++st.scenarios;
        Scenario sc(r, L, g, st);
        if (!sc.run(ops)) std::printf("scenario %u stopped\n", first + static_cast<std::uint32_t>(s));
        r.stop();
    }
    std::printf("economy_harness: %ld scenarios, %ld operations, %ld mismatches, %ld failures\n", st.scenarios, st.ops, st.mismatches,
                st.failures);
    std::printf("  settlements %ld (%ld unit slots, %ld left short, %ld with waste), stalled slot-checks %ld, cloak decisions %ld\n",
                st.settles, st.settled_units, st.short_settles, st.wasted, st.stalled_slots, st.cloak_calls);
    std::printf("  bookings %ld (%ld refused), immediate spends %ld, transfers %ld (%ld sent), sharing calls %ld (%ld packets, %ld map shares)\n",
                st.add_uses, st.add_uses_refused, st.consumes, st.transfers, st.transfers_sent, st.shares, st.share_packets, st.map_shares);
    std::printf("  wind steps %ld (%ld changes)\n", st.winds, st.wind_changes);
    return st.mismatches || st.failures ? 1 : 0;
}
