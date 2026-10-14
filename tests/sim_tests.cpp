// Simulation core tests. Expected values marked "golden" were produced by
// running the original routines in Ghidra's emulator (see docs/RE_MEMORY.md).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include <string>

#include "sim/battle.h"
#include "sim/combat.h"
#include "sim/fixed.h"
#include "sim/ground_settle.h"
#include "sim/heightmap.h"
#include "sim/orders.h"
#include "sim/projectile.h"
#include "sim/unit.h"
#include "sim/rng.h"
#include "sim/script_vm.h"
#include "sim/step_budget.h"
#include "sim/trig.h"
#include "sim/weapons.h"
#include "sim/world.h"
#include "sim/x87.h"
#include "sim/x87_math.h"
#include "sim/x87_trig.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK_EQ(actual, expected)                                                         \
    do {                                                                                   \
        ++g_checks;                                                                        \
        const auto a_ = (actual);                                                          \
        const auto e_ = (expected);                                                        \
        if (!(a_ == e_)) {                                                                 \
            ++g_failures;                                                                  \
            std::printf("FAIL %s:%d  %s == %s  (got 0x%llx, want 0x%llx)\n", __FILE__,     \
                        __LINE__, #actual, #expected, static_cast<unsigned long long>(a_), \
                        static_cast<unsigned long long>(e_));                              \
        }                                                                                  \
    } while (0)

using namespace sim;

void test_sin_table_matches_fixture() {
    std::ifstream f(FIXTURES_DIR "/sin_table_0x509F00.bin", std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK_EQ(bytes.size(), std::size_t{1280});
    if (bytes.size() != 1280) return;
    const auto t = trig::table();
    for (std::size_t i = 0; i < t.size(); ++i) {
        const auto v = static_cast<std::int16_t>(bytes[2 * i] | (bytes[2 * i + 1] << 8));
        CHECK_EQ(t[i], v);
    }
}

void test_sin_table_formula() {
    const auto t = trig::table();
    for (std::size_t i = 0; i < t.size(); ++i) {
        const double v = 8192.0 * std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 512.0);
        CHECK_EQ(static_cast<std::int32_t>(t[i]), static_cast<std::int32_t>(std::lround(v)));
    }
}

void test_battle_rng_golden() {
    struct Case { std::uint32_t state, n, result; };
    const Case cases[] = {
        {0x12345679u, 1000u, 0x225u},
        {0x00000001u, 0x7FFFFFFFu, 0x41A7u},
        {0x80000000u, 0x7FFFFFFFu, 0x41A7u},
        {0xFFFFFFFFu, 0x7FFFFFFFu, 0x41A7u},
        {0x0001F31Du, 0x7FFFFFFFu, 0x7FFFF4EBu},
        {0x66E29573u, 0x7FFFFFFFu, 0x25C1E5CAu},
        {0xDEADBEEFu, 0x7FFFFFFFu, 0x60D2AF1Fu},
        {0x7FFFFFFFu, 0x7FFFFFFFu, 0x0u},
    };
    for (const auto& c : cases) {
        BattleRng rng;
        rng.set_state(c.state);
        CHECK_EQ(rng.next_mod(c.n), c.result);
    }
}

void test_battle_rng_edges() {
    BattleRng rng;
    rng.set_state(0xDEADBEEFu);
    CHECK_EQ(rng.next_mod(1), 0u);           // n < 2: no draw
    CHECK_EQ(rng.next_mod(0x80000000u), 0u); // signed compare: no draw
    CHECK_EQ(rng.state(), 0xDEADBEEFu);
    rng.seed(0);
    CHECK_EQ(rng.state(), 0x66E29573u);      // (0 ^ 0x66E29572) | 1
}

void test_lcg15() {
    Lcg15 r;
    r.seed(1); // MSVC rand() default seed: 41, 18467, 6334, 26500, 19169
    CHECK_EQ(r.next(), 41u);
    CHECK_EQ(r.next(), 18467u);
    CHECK_EQ(r.next(), 6334u);
    CHECK_EQ(r.next(), 26500u);
    CHECK_EQ(r.next(), 19169u);
}

void test_mul_sin_cos_golden() {
    struct Case { Angle a; std::int32_t mag; std::uint32_t sin, cos; bool has_sin, has_cos; };
    const Case cases[] = {
        {0x1234, 100000, 0xA709u, 0x16119u, true, true},
        {0x7FF0, -70000, 0, 0x11170u, false, true},
        {0x8000, 0x12345, 0, 0xFFFEDCBBu, false, true},
        {0xFFF0, 5000, 0, 0x1388u, false, true},
        {0xC123, -123456789, 0x075B5758u, 0xFFD1C760u, true, true},
    };
    for (const auto& c : cases) {
        if (c.has_sin) CHECK_EQ(static_cast<std::uint32_t>(trig::mul_sin(c.a, c.mag)), c.sin);
        if (c.has_cos) CHECK_EQ(static_cast<std::uint32_t>(trig::mul_cos(c.a, c.mag)), c.cos);
    }
}

void test_ftol() {
    CHECK_EQ(x87::ftol64(1.9), std::int64_t{1});
    CHECK_EQ(x87::ftol64(-1.9), std::int64_t{-1});
    CHECK_EQ(x87::ftol64(std::nan("")), INT64_MIN);
    CHECK_EQ(x87::ftol64(1e30), INT64_MIN);
    CHECK_EQ(x87::ftol64(-9223372036854775808.0), INT64_MIN);
    CHECK_EQ(x87::ftol32(4294967297.5), std::int32_t{1});            // low 32 bits wrap
    CHECK_EQ(x87::ftol32(2147483648.0), static_cast<std::int32_t>(0x80000000u));
    CHECK_EQ(x87::ftol32(1e30), std::int32_t{0});                    // indefinite -> low half 0
}

void test_fixed() {
    CHECK_EQ(to_fixed(3), Fixed16{0x30000});
    CHECK_EQ(wrap_add(INT32_MAX, 1), INT32_MIN);
    CHECK_EQ(sar(-3, 1), -2);
}

void test_world_stage_order() {
    World w;
    std::string log;
    for (const Stage s : kStageOrder) {
        w.set_stage(s, [&log, s](World& world) {
            log += std::to_string(world.tick()) + ":" + std::to_string(static_cast<int>(s)) + " ";
        });
    }
    w.set_frame_end([&log](World&) { log += "end "; });
    w.run_frame(2);
    // Offline: network stages (0 and 10) are skipped.
    CHECK_EQ(log == "1:1 1:2 1:3 1:4 1:5 1:6 1:7 1:8 1:9 2:1 2:2 2:3 2:4 2:5 2:6 2:7 2:8 2:9 end ", true);
    log.clear();
    w.set_networked(true);
    w.run_frame(0);
    CHECK_EQ(log == "end ", true);
    w.run_frame(1);
    CHECK_EQ(log == "end 3:0 3:1 3:2 3:3 3:4 3:5 3:6 3:7 3:8 3:9 3:10 end ", true);
}

void test_step_budget() {
    // Normal speed: 1 step per 1/30 s, fractions carried.
    StepBudget b;
    b.update(0);                  // 0 steps: hysteresis -1
    CHECK_EQ(b.update(1000), 5);  // 30 steps wanted, capped at 5 (excess dropped): hysteresis 0
    CHECK_EQ(b.hysteresis, std::int16_t{0});
    CHECK_EQ(b.update(1034), 1);  // 1034*30/1000 = 31 -> elapsed 1
    CHECK_EQ(b.update(1100), 2);  // 33 -> elapsed 2
    CHECK_EQ(b.hysteresis, std::int16_t{-2});

    // Half speed: 0.5 step per tick, remainder carried as float.
    StepBudget h;
    h.game_speed = h.effective_speed = 5;
    h.update(0);
    CHECK_EQ(h.update(34), 0);  // 1 tick * 0.5
    CHECK_EQ(h.step_fraction, 0.5f);
    CHECK_EQ(h.update(67), 1);  // 0.5 + 0.5
    CHECK_EQ(h.step_fraction, 0.0f);

    // Overload backs the speed off once hysteresis exceeds 10.
    StepBudget o;
    o.update(0);  // hysteresis -1
    std::uint32_t t = 0;
    for (int i = 0; i < 11; ++i) o.update(t += 1000);  // -> 10
    CHECK_EQ(o.effective_speed, std::uint16_t{10});
    o.update(t += 1000);  // 11 > 10: reset and slow down
    CHECK_EQ(o.effective_speed, std::uint16_t{9});
    CHECK_EQ(o.hysteresis, std::int16_t{0});

    // Remote lag throttle: 3600+ ticks behind -> rate * 0.01.
    StepBudget l;
    l.update(0);
    CHECK_EQ(l.update(10000, 5000), 3);  // 300 ticks * 1.0 * 0.01
    CHECK_EQ(l.lag_throttled, true);

    // Paused: no steps, no hysteresis change.
    StepBudget p;
    p.paused = true;
    p.update(0);
    CHECK_EQ(p.update(1000), 0);
    CHECK_EQ(p.hysteresis, std::int16_t{0});
}

// Minimal host: remembers piece turns, records calls.
struct TestHost : sim::ScriptHost {
    std::int32_t turn[4][3] = {};
    int calls = 0;
    void set_piece_move(std::int32_t, std::int32_t, std::int32_t) override { ++calls; }
    void set_piece_turn(std::int32_t p, std::int32_t a, std::int32_t v) override { turn[p][a] = v; ++calls; }
    void set_piece_visible(std::int32_t, std::int32_t) override {}
    void set_piece_cached(std::int32_t, std::int32_t) override {}
    void set_piece_shaded(std::int32_t, std::int32_t) override {}
    std::int32_t get_piece_move(std::int32_t, std::int32_t) override { return 0; }
    std::int32_t get_piece_turn(std::int32_t p, std::int32_t a) override { return turn[p][a]; }
    void legacy_explode(std::int32_t, std::int32_t, std::int32_t) override {}
    void play_sound(std::int32_t) override {}
    void emit_sfx(std::int32_t, std::int32_t) override {}
    void explode_piece(std::int32_t, std::int32_t) override {}
    void attach_unit(std::int32_t, std::int32_t, std::int32_t) override {}
    void drop_unit(std::int32_t) override {}
    void set_value(std::int32_t, std::int32_t) override {}
    std::int32_t get_value(std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t) override { return 0; }
    std::int32_t get_transporting(std::int32_t) override { return 0; }
    std::int32_t get_carrier() override { return 0; }
    std::uint32_t random_mod(std::uint32_t) override { return 0; }
};

void test_x87_rotation() {
    std::int32_t x = 1000, y = -7;
    sim::x87::rotate_point_2d(0, x, y);  // angle 0: untouched
    CHECK_EQ(x, 1000);
    CHECK_EQ(y, -7);
    x = 1000, y = 0;
    sim::x87::rotate_point_2d(16384, x, y);  // quarter turn
    CHECK_EQ(x, 0);
    CHECK_EQ(y, 1000);
    CHECK_EQ(sim::x87::fist_nearest(2.5), 2);    // round half to even
    CHECK_EQ(sim::x87::fist_nearest(-3.5), -4);
    CHECK_EQ(sim::x87::fist_nearest(3e9), INT32_MIN);  // integer indefinite
    // The FPU's pi is 66 bits: sin of the "half turn" is not 0 at extended precision.
    sim::x87::Ext80 s, c;
    sim::x87::sincos_for_angle(-32768, s, c);
    CHECK_EQ(s.mantissa != 0, true);

    // FPU profiles: the captured profile needs a capture; the documented model
    // differs from Intel silicon only in the last bit of some values.
    CHECK_EQ(sim::x87::fpu_profile() == sim::x87::FpuProfile::Model66Pi, true);
    const std::uint8_t junk[16] = {};
    CHECK_EQ(sim::x87::load_fsincos_capture(junk, sizeof junk), false);
    CHECK_EQ(sim::x87::set_fpu_profile(sim::x87::FpuProfile::Captured), false);
    int differing = 0;
    for (int k = 1; k < 4096; ++k) {
        sim::x87::Ext80 s1, c1, s2, c2;
        sim::x87::set_fpu_profile(sim::x87::FpuProfile::IntelP6);
        sim::x87::sincos_for_angle(static_cast<std::int16_t>(k), s1, c1);
        sim::x87::set_fpu_profile(sim::x87::FpuProfile::Model66Pi);
        sim::x87::sincos_for_angle(static_cast<std::int16_t>(k), s2, c2);
        const long long ds = static_cast<long long>(s1.mantissa - s2.mantissa), dc = static_cast<long long>(c1.mantissa - c2.mantissa);
        if (ds || dc) ++differing;
        CHECK_EQ(ds >= -1 && ds <= 1 && dc >= -1 && dc <= 1, true);
    }
    CHECK_EQ(differing > 0, true);
    sim::x87::set_fpu_profile(sim::x87::FpuProfile::Model66Pi);
}

void test_heightmap_and_packing() {
    sim::HeightMap hm(3, 3);
    hm.at(0, 0) = 0;
    hm.at(1, 0) = 16;
    hm.at(0, 1) = 32;
    hm.at(1, 1) = 48;
    CHECK_EQ(hm.sample(0, 0), 0);
    CHECK_EQ(hm.sample(8, 0), 8);     // halfway along x
    CHECK_EQ(hm.sample(8, 8), 24);    // centre of the cell
    CHECK_EQ(hm.sample(-1, 0), -1);   // outside
    CHECK_EQ(hm.sample(32, 0), -1);   // the +1 neighbour would be outside
    CHECK_EQ(hm.cell_height(5, 5), 0u);
    // X integer part high, Z integer part low; a negative Z borrows from X.
    CHECK_EQ(sim::pack_xz(10 << 16, 3 << 16), (10 << 16) + 3);
    CHECK_EQ(sim::pack_xz(10 << 16, -(3 << 16)), (10 << 16) - 3);
}

void test_script_vm() {
    using sim::ScriptOp;
    sim::ScriptProgram prog;
    auto emit = [&](ScriptOp op, std::initializer_list<std::uint32_t> operands) {
        prog.code.push_back(0);
        prog.ops.push_back(op);
        for (std::uint32_t w : operands) {
            prog.code.push_back(w);
            prog.ops.push_back(ScriptOp::Invalid);
        }
    };
    prog.piece_names = {"base", "turret"};
    prog.function_names = {"Create"};
    prog.entry = {0};
    // turn turret around y-axis to 4000 speed 30000 (1000 per tick); wait-for-turn; sleep 100; return
    emit(ScriptOp::PushConst, {30000});
    emit(ScriptOp::PushConst, {4000});
    emit(ScriptOp::Turn, {1, 1});
    emit(ScriptOp::WaitTurn, {1, 1});
    emit(ScriptOp::PushConst, {100});
    emit(ScriptOp::Sleep, {});
    emit(ScriptOp::Return, {});

    TestHost host;
    sim::ScriptInstance vm(prog, host, 30);
    CHECK_EQ(vm.call("Create", nullptr, true), true);
    CHECK_EQ(vm.thread_word(0, sim::ScriptInstance::kState), sim::ScriptInstance::kWaitingTurn);
    CHECK_EQ(vm.piece_anims()[sim::ScriptInstance::kPieceWords + sim::ScriptInstance::kTurnSpeed + 1], 1000);
    for (int k = 1; k <= 4; ++k) {
        vm.tick(1);
        CHECK_EQ(host.turn[1][1], k * 1000);  // last step snaps exactly onto the target
    }
    vm.tick(1);  // wakes (speed 0), sleeps 100 ms = 3 ticks
    CHECK_EQ(vm.thread_word(0, sim::ScriptInstance::kState), sim::ScriptInstance::kSleeping);
    CHECK_EQ(vm.thread_word(0, sim::ScriptInstance::kSleep), 3u);
    vm.tick(1);
    vm.tick(1);
    CHECK_EQ(vm.thread_count(), 1);
    vm.tick(1);  // sleep reaches 0: runs and returns
    CHECK_EQ(vm.thread_count(), 0);
    CHECK_EQ(vm.call("create", nullptr, true), false);  // names are case-sensitive
    CHECK_EQ(vm.fault().empty(), true);
}

struct CombatLog : sim::CombatSystems {
    std::vector<std::string> calls;
    bool pay = true;
    std::vector<std::uint8_t> last_packet;
    void retarget(Unit*, Unit&, std::uint16_t) override { calls.push_back("retarget"); }
    void add_paralysis(Unit&, std::uint16_t) override { calls.push_back("paralysis"); }
    bool pay_energy(Unit&, float) override { return pay; }
    void stats_activity(std::int32_t) override {}
    void network_send(std::int32_t, const std::uint8_t* p, std::int32_t size) override { last_packet.assign(p, p + size); }
    void reveal_dead_unit(Unit&) override {}
    void victory_unit_destroyed(Unit&) override {}
    void clear_orders(Unit&) override {}
    void forget_speech(Unit&) override {}
    void leave_group(Unit&) override {}
    void remove_projectiles(Unit&) override {}
    void detach_from_carrier(Unit&) override {}
    void clear_footprint(Unit&) override {}
    void forget_fog(Unit&) override {}
    void kill_counted(std::uint8_t, Unit&) override { calls.push_back("kill counted"); }
    void detonate(Unit&, bool) override { calls.push_back("detonate"); }
    void spawn_wreck(Unit&, std::int32_t level, bool burn) override { calls.push_back("wreck " + std::to_string(level) + (burn ? " burn" : "")); }
    void release_unit(Unit&) override {}
    void release_motion(Unit&) override {}
    void player_out_of_units(int) override { calls.push_back("out of units"); }
    void commander_died(int) override { calls.push_back("commander died"); }
};

void test_combat() {
    sim::UnitTypeInfo type;
    type.max_damage = 1000;
    sim::UnitTickTypeInfo tick_type;
    tick_type.damage_modifier = 0x8000;  // armor halves damage
    tick_type.build_time = 100;
    tick_type.name = "TANK";
    std::vector<Unit> units(4);
    std::vector<UnitTickState> ticks(4);
    std::vector<PlayerSlot> players(10);
    for (int p = 0; p < 2; ++p) {
        players[static_cast<std::size_t>(p)].active = true;
        players[static_cast<std::size_t>(p)].controller = static_cast<std::uint8_t>(p + 1);
        players[static_cast<std::size_t>(p)].net_id = 100 + p;
        players[static_cast<std::size_t>(p)].alive_count = 2;
    }
    players[1].commander_name = "tank";  // matched without regard to case
    for (std::uint16_t i = 1; i <= 3; ++i) {
        units[i].id = i;
        units[i].type = &type;
        units[i].hit_points = 500;
        units[i].flags = unit_flags::kAlive;
        ticks[i].type_index = 1;
        ticks[i].tick_type = &tick_type;
        ticks[i].owner = i == 1 ? 0 : 1;
    }
    sim::CombatEnvironment env;
    env.local_player = 5;
    CombatLog log;
    sim::Combat c{units, ticks, players, env, log};

    // Veterancy of the target: 12 kills = tier 2, so 92% of the damage.
    ticks[2].kill_count = 12;
    sim::apply_damage(c, &units[1], units[2], 100, sim::damage_kind::kWeapon, 0x1234);
    CHECK_EQ(units[2].hit_points, 500 - 92);
    CHECK_EQ(ticks[2].last_attacker == &units[1], true);
    CHECK_EQ(ticks[2].last_attacker_slot, 0);
    CHECK_EQ(ticks[2].damaged_timer, 0xF0);
    units[2].activate_flags = unit_flags::kArmored;
    sim::apply_damage(c, nullptr, units[2], 100, 0, 0);
    CHECK_EQ(units[2].hit_points, 408 - 46);  // 50 after armor, then 92%
    // A negative heal wraps to a huge unsigned amount and fills the unit up.
    sim::apply_damage(c, nullptr, units[2], -5, sim::damage_kind::kHeal, 0);
    CHECK_EQ(units[2].hit_points, 1000);
    // Repair and self-heal give at most 1 hit point per step.
    units[3].hit_points = 10;
    CHECK_EQ(sim::repair_heal(c, units[3], units[3], 8.0f), true);
    CHECK_EQ(units[3].hit_points, 11);
    log.pay = false;
    CHECK_EQ(sim::repair_heal(c, units[3], units[3], 8.0f), false);
    CHECK_EQ(units[3].hit_points, 11);

    // Lethal damage marks a playing owner's unit as dying; the kill happens later.
    sim::apply_damage(c, &units[1], units[3], 2000, sim::damage_kind::kWeapon, 0);
    CHECK_EQ((units[3].flags & unit_tick_flags::kDying) != 0, true);
    CHECK_EQ(units[3].hit_points < 0, true);
    ticks[3].prev_health_percent = 40;
    log.calls.clear();
    // No script: the wreck level is the low nibble of the caller's stale stack word (here a
    // return address the ground-settle routine leaves).
    std::uint32_t stale = 0x0048A8FF;
    sim::kill_unit(c, units[3], ticks[3].last_damage_kind, stale);
    CHECK_EQ(stale, 0x0048A8FFu);  // left as the final wreck level
    CHECK_EQ(log.last_packet.size(), 11u);
    CHECK_EQ(log.last_packet[0], 0x0C);
    CHECK_EQ(log.last_packet[3], 100);  // attacker's net id
    CHECK_EQ(log.last_packet[10], 0x1F);
    CHECK_EQ(players[1].units_lost, 1);
    CHECK_EQ(players[0].units_killed, 1);
    CHECK_EQ(players[0].commanders_killed, 1);
    CHECK_EQ(players[1].commanders_lost, 1);
    CHECK_EQ(ticks[1].kill_count, 1);
    CHECK_EQ(players[1].alive_count, 1);
    CHECK_EQ(ticks[3].type_index, 0);
    CHECK_EQ((units[3].flags & unit_flags::kAlive) != 0, false);
    CHECK_EQ(log.calls.size(), 3u);  // no "commander died": game mode 0
    CHECK_EQ(log.calls[0] == "kill counted" && log.calls[1] == "detonate" && log.calls[2] == "wreck 15 burn", true);
    // Corpse-only death: wreck level 1, no burning, no severity.
    log.calls.clear();
    sim::kill_unit(c, units[2], sim::damage_kind::kCorpseOnly, stale);
    CHECK_EQ(stale, 1u);
    CHECK_EQ(log.last_packet[10], 0x71);
    CHECK_EQ(log.last_packet[9], 0);
    CHECK_EQ(log.calls.size(), 2u);
    CHECK_EQ(log.calls[0] == "wreck 1" && log.calls[1] == "out of units", true);
    CHECK_EQ(sim::player_slot_by_net_id(players, 101), 1);
    CHECK_EQ(sim::player_slot_by_net_id(players, -1), 10);
    CHECK_EQ(sim::player_net_id(players, 10), -1);
}

struct WeaponLog : sim::WeaponSystems {
    std::vector<std::string> calls;
    std::int32_t result = 1;
    sim::WorldPos last_pos{};
    std::int32_t fire(Unit&, int slot, Unit* target, sim::WorldPos& pos) override {
        calls.push_back("fire " + std::to_string(slot) + (target ? " unit " + std::to_string(target->id) : std::string()));
        last_pos = pos;
        return result;
    }
    void network_send(std::int32_t, const std::uint8_t*, std::int32_t size) override { calls.push_back("send " + std::to_string(size)); }
    void stockpile_used(Unit&) override { calls.push_back("stockpile"); }
};

void test_weapons() {
    sim::HeightMap hm(4, 4);
    for (int z = 0; z < 4; ++z)
        for (int x = 0; x < 4; ++x) hm.at(x, z) = static_cast<std::uint8_t>(20 + x);
    sim::UnitTypeInfo type;
    type.max_damage = 1000;
    std::vector<Unit> units(4);
    std::vector<UnitTickState> ticks(4);
    std::vector<PlayerSlot> players(10);
    players[0].energy_stored = 100.0f;
    players[0].metal_stored = 50.0f;
    for (std::uint16_t i = 1; i <= 3; ++i) {
        units[i].id = i;
        units[i].type = &type;
        units[i].hit_points = 500;
        units[i].flags = unit_flags::kAlive;
        ticks[i].type_index = 1;
    }
    sim::WeaponType gun;
    gun.fire = sim::WeaponFireRoutine::LineOfSight;
    gun.range = 100;
    gun.reload_time = 30;
    gun.energy_per_shot = 10.0f;
    gun.metal_per_shot = 5.0f;
    gun.velocity = 0x80000;
    sim::WeaponEnvironment env;
    env.heightmap = &hm;
    env.sea_level = 10;
    WeaponLog log;
    sim::Weapons w{units, ticks, players, env, log};
    Unit& shooter = units[1];
    sim::WeaponSlot& slot = shooter.weapons[0];
    slot.type = &gun;
    slot.flags = sim::slot_flags::kEnabled;
    slot.target_x = 16;  // ground point (16, 16): terrain height 21
    slot.target_z = 16;
    ticks[1].kill_count = 12;
    shooter.pos[1] = 100 << 16;  // the gun must be above the water

    std::uint32_t stale = 0x12345678;
    sim::process_weapon_slots(w, shooter, stale);
    CHECK_EQ(log.calls.size(), 1u);
    CHECK_EQ(log.last_pos[1], 21 << 16);
    CHECK_EQ(stale, 21u << 16);  // the last target height is left in the caller's stack word
    // Reload: (100 - 2 x 6)% of 30 = 26, then x (120 - 20 x 500/1000) / 100 = 28.
    CHECK_EQ(slot.reload, 28);
    CHECK_EQ(shooter.net_dirty, 0x400);
    CHECK_EQ(players[0].energy_stored, 90.0f);
    CHECK_EQ(players[0].metal_stored, 45.0f);
    CHECK_EQ(ticks[1].energy_use, 10.0f);
    CHECK_EQ(ticks[1].metal_use, 5.0f);
    // Reloading: no shot, the countdown drops by one.
    sim::process_weapon_slots(w, shooter, stale);
    CHECK_EQ(slot.reload, 27);
    CHECK_EQ(log.calls.size(), 1u);
    // Below sea level the ground point is raised to the sea surface; out of range sets net bit 0x1000.
    env.sea_level = 40;
    slot.reload = 0;
    slot.target_x = 3000;
    slot.target_z = 3000;
    stale = 0;
    sim::process_weapon_slots(w, shooter, stale);
    CHECK_EQ(stale, 40u << 16);  // outside the map the sample is -1: sea level
    CHECK_EQ(shooter.net_dirty, 0x1400);
    CHECK_EQ(log.calls.size(), 1u);
    // Not enough energy: no shot, nothing paid.
    players[0].energy_stored = 5.0f;
    slot.target_x = 16;
    slot.target_z = 16;
    env.sea_level = 0;
    sim::process_weapon_slots(w, shooter, stale);
    CHECK_EQ(log.calls.size(), 1u);
    CHECK_EQ(sim::use_resources(players[0], ticks[1], 10.0f, 1.0f), false);
    players[0].energy_stored = 100.0f;

    // A dead target is forgotten and the slot stops aiming; the stack word is untouched.
    slot.target_x = 2;
    slot.target_z = sim::kUnitTarget;
    slot.flags |= sim::slot_flags::kAiming;
    ticks[2].type_index = 0;
    stale = 7;
    sim::process_weapon_slots(w, shooter, stale);
    CHECK_EQ(slot.target_x, 0);
    CHECK_EQ(slot.flags & sim::slot_flags::kAiming, 0);
    CHECK_EQ(stale, 7u);

    // Veterans (more than 5 kills) lead a moving target: 100 units away at 8 units per
    // tick is 12.5 ticks, 5/6 of which (10.4166 ticks, 655350 in 16.16) times its velocity.
    ticks[2].type_index = 1;
    ticks[2].has_motion = true;
    ticks[2].velocity = {0x10000, 0, 0};
    units[2].pos = {100 << 16, 0, 0};
    shooter.pos = {0, 0, 0};
    slot.target_x = 2;
    sim::WorldPos pos{};
    CHECK_EQ(sim::weapon_target_position(w, shooter, 0, pos), true);
    CHECK_EQ(pos[0], (100 << 16) + 655350);
    ticks[1].kill_count = 5;
    CHECK_EQ(sim::weapon_target_position(w, shooter, 0, pos), true);
    CHECK_EQ(pos[0], 100 << 16);

    // Aim tolerance: 150 (2000 while bits 2-3 of the unit flags are set) unless the type has its own.
    slot.aim_heading = 1000;
    slot.aim_pitch = 0;
    CHECK_EQ(sim::aim_within_tolerance(shooter, slot, 1150, 0xFF6A), true);
    CHECK_EQ(sim::aim_within_tolerance(shooter, slot, 1151, 0), false);
    shooter.flags |= 0x4;
    CHECK_EQ(sim::aim_within_tolerance(shooter, slot, 3000, 0), true);
    gun.tolerance = 10;
    CHECK_EQ(sim::aim_within_tolerance(shooter, slot, 1010, 10), true);
    CHECK_EQ(sim::aim_within_tolerance(shooter, slot, 1010, 11), false);
    gun.pitch_tolerance = 20;
    CHECK_EQ(sim::aim_within_tolerance(shooter, slot, 1010, 11), true);

    // Stockpile weapons spend a stored shot instead of reloading and paying.
    gun.flags = sim::weapon_flags::kStockpile | sim::weapon_flags::kCommandFire;
    slot.stockpile = 2;
    slot.reload = 0;
    shooter.pos[1] = 100 << 16;
    shooter.net_dirty = 0;
    log.calls.clear();
    sim::process_weapon_slots(w, shooter, stale);
    CHECK_EQ(slot.stockpile, 1);
    CHECK_EQ(slot.reload, 0);
    CHECK_EQ(shooter.net_dirty, 0x800);
    CHECK_EQ(log.calls.size(), 2u);
    CHECK_EQ(log.calls[0] == "fire 0 unit 2" && log.calls[1] == "stockpile", true);
    CHECK_EQ(players[0].energy_stored, 100.0f);
}

// Mission behaviours scripted as a list of results; order memory from a fake heap.
struct ScriptedOrders : sim::OrderSystems {
    std::vector<std::int32_t> results;
    std::size_t at = 0;
    std::uint32_t next = 0x01000000;
    int freed = 0, weapon_releases = 0, cleared_slots = 0;
    std::int32_t run_mission(Unit&, MissionOrder& o, std::uint32_t&) override {
        if (at < results.size()) return results[at++];
        o.wait |= order_wait::kTimer;  // nothing scripted: sleep until tick 1000
        o.wake_tick = 1000;
        return mission_result::kContinue;
    }
    void clear_weapon_target(Unit&, int) override { ++cleared_slots; }
    void release_weapon_targets(Unit&) override { ++weapon_releases; }
    void release_attached(Unit&, MissionOrder& o) override { o.attached = 0; }
    void broadcast_script_call(Unit&, std::int16_t) override {}
    MissionOrder* allocate_order() override {
        auto* o = new MissionOrder;
        o->address = next;
        next += 0x58;
        return o;
    }
    void free_order(MissionOrder* o) override {
        ++freed;
        delete o;
    }
};

void test_orders() {
    CHECK_EQ(mission::default_flags()[mission::kParalyze], 0x24u);
    CHECK_EQ(mission::default_flags()[mission::kBuildWeapon], 0xC0140u);
    CHECK_EQ(mission::default_flags()[mission::kWaitForAttack], 0x204u);
    std::vector<Unit> units(3);
    std::vector<UnitTickState> ticks(3);
    std::vector<PlayerSlot> players(10);
    players[0].active = true;
    players[0].controller = 1;
    UnitTickTypeInfo type;
    type.default_mission = mission::kStandby;
    for (std::uint16_t i = 1; i <= 2; ++i) {
        units[i].id = i;
        ticks[i].type_index = 1;
        ticks[i].tick_type = &type;
    }
    BattleRng rng;
    rng.set_state(12345);
    ScriptedOrders sys;
    OrderContext c{ticks, players, rng, sys, 100};

    // Construction: a target makes a link in the target's list; no position drops kHasPosition.
    MissionOrder* a = sys.allocate_order();
    construct_order(c, *a, mission::kAttackChase, &units[2], nullptr, 7, 0, 0);
    CHECK_EQ(a->flags, 0x280u);
    CHECK_EQ(units[2].referrers == &a->target, true);
    CHECK_EQ(a->created_tick, 100u);
    CHECK_EQ(a->wake_tick, 0xFFFFFFFFu);
    // A kind without kHasTarget drops the target again.
    MissionOrder* b = sys.allocate_order();
    construct_order(c, *b, mission::kStop, &units[2], nullptr, 0, 0, 0);
    CHECK_EQ(b->target.unit == nullptr, true);
    prepend_order(units[1], *a);
    b->flags |= order_flags::kIdle;
    insert_order(units[1], *b, nullptr);  // at the end
    CHECK_EQ(units[1].orders == a && a->next == b && b->unit == &units[1], true);

    // Primary: result 3 sleeps 30 + random(15) steps; the order stays.
    sys.results = {mission_result::kSnooze};
    run_primary_orders(c, units[1]);
    BattleRng check;
    check.set_state(12345);
    CHECK_EQ(a->wake_tick, 100u + 30u + check.next_mod(15));
    CHECK_EQ(rng.state(), check.state());
    // Woken by its timer, it finishes; the next order runs and sleeps (nothing scripted).
    c.game_tick = a->wake_tick;
    sys.results = {mission_result::kFinished};
    sys.at = 0;
    run_primary_orders(c, units[1]);
    CHECK_EQ(units[1].orders == b, true);
    CHECK_EQ(sys.freed, 1);
    CHECK_EQ(sys.weapon_releases, 1);  // it was the current order
    CHECK_EQ(units[2].referrers == nullptr, true);
    CHECK_EQ(b->wake_tick, 1000u);
    // An empty list gets the type's default mission, marked idle.
    clear_orders(c, units[1], true);
    run_primary_orders(c, units[1]);
    CHECK_EQ(units[1].orders != nullptr && units[1].orders->kind == mission::kStandby, true);
    CHECK_EQ((units[1].orders->flags & order_flags::kIdle) != 0, true);

    // Paralysis adds to an existing paralyze order at the head.
    add_paralysis(c, units[2], 40);
    add_paralysis(c, units[2], 2);
    CHECK_EQ(units[2].orders->kind == mission::kParalyze && units[2].orders->param0 == 42 && !units[2].orders->next, true);

    // Secondary: the stale word is 15 after a random sleep and the order's address after it is destroyed.
    MissionOrder* s = sys.allocate_order();
    construct_order(c, *s, mission::kBuildWeapon, nullptr, nullptr, 0, 0, 0);
    prepend_order(units[2], *s);
    CHECK_EQ(units[2].secondary_orders == s, true);
    std::uint32_t stale = 0xAAAA;
    sys.results = {mission_result::kSnooze};
    sys.at = 0;
    run_secondary_orders(c, units[2], stale);
    CHECK_EQ(stale, 15u);
    c.game_tick = s->wake_tick;
    const std::uint32_t addr = s->address;
    sys.results = {mission_result::kFinished};
    sys.at = 0;
    run_secondary_orders(c, units[2], stale);
    CHECK_EQ(stale, addr);
    CHECK_EQ(units[2].secondary_orders == nullptr, true);
    clear_orders(c, units[1], true);
    clear_orders(c, units[2], true);
}

struct FlatSettle : sim::GroundSettleSystems {
    std::int32_t motion_speed(Unit&) override { return 0; }
    std::uint32_t motion_start_tick(Unit&) override { return 0; }
    std::uint32_t clock_ticks() override { return 0; }
};

void test_ground_settle() {
    HeightMap hm(8, 8);
    for (int z = 0; z < 8; ++z)
        for (int x = 0; x < 8; ++x) hm.at(x, z) = static_cast<std::uint8_t>(10 + 2 * x);  // slopes up along X
    FlatSettle sys;
    GroundSettleType type;
    type.has_footprint = true;
    type.footprint = {{{-8 << 16, -8 << 16}, {8 << 16, -8 << 16}, {-8 << 16, 8 << 16}, {8 << 16, 8 << 16}}};
    Unit u;
    u.pos = {64 << 16, 0x12345, 64 << 16};
    u.flags = unit_flags::kAlive | settle_flags::kResettle | settle_flags::kLayerGround;
    std::uint32_t stale = 7;
    settle_on_ground(u, true, type, 0, 0, hm, sys, stale);
    CHECK_EQ(stale, settle_stale::kAfterSlopeFit);
    CHECK_EQ(u.flags & settle_flags::kResettle, 0u);
    // Height 18 at x = 64 (cell 4); only the integer part of Y changes.
    CHECK_EQ(u.pos[1], (18 << 16) | 0x2345);
    CHECK_EQ(u.bank, 0);
    CHECK_EQ(u.pitch, x87::atan2_scaled(-2, 16));  // corner 0 (17) is 2 below corner 1 (19), 16 apart
    // Without the resettle flag (and not hovering) nothing happens.
    stale = 7;
    settle_on_ground(u, true, type, 0, 0, hm, sys, stale);
    CHECK_EQ(stale, 7u);
    // Terrain-only types take the sampled height (fraction cleared); waterline types sit below the sea.
    u.flags |= settle_flags::kResettle;
    type.type_flags = settle_flags::kTypeTerrainOnly;
    settle_on_ground(u, true, type, 0, 0, hm, sys, stale);
    CHECK_EQ(stale, settle_stale::kAfterTerrainSample);
    CHECK_EQ(u.pos[1], 18 << 16);
    u.flags |= settle_flags::kResettle;
    type.type_flags = settle_flags::kTypeWaterline;
    type.waterline = 5;
    settle_on_ground(u, true, type, 30, 0, hm, sys, stale);
    CHECK_EQ(u.pos[1], 25 << 16);
}

struct ProjectileLog : sim::ProjectileSystems {
    std::vector<std::string> calls;
    const sim::WeaponType* weapon = nullptr;
    const sim::WeaponType* slot_weapon(const Unit&, int slot) override { return slot == 0 ? weapon : nullptr; }
    std::uint16_t slot_fire_heading(const Unit&, int) override { return 0; }
    std::int32_t motion_speed(const Unit&) override { return 0; }
    void play_sound(std::uint16_t, const sim::Vec3i&) override { calls.push_back("sound"); }
    void spawn_smoke(const sim::Vec3i&) override { calls.push_back("smoke"); }
    void spawn_muzzle_smoke(const sim::Vec3i&) override { calls.push_back("muzzle smoke"); }
    void spawn_explosion(const sim::Vec3i&, const sim::WeaponType&, bool, bool) override { calls.push_back("explosion"); }
    void camera_shake(std::int32_t, std::int32_t, std::int32_t) override {}
    void damage_feature(sim::MapCell&, std::int32_t, std::int32_t, const sim::WeaponType&) override { calls.push_back("feature"); }
    void network_send(std::int32_t, const std::uint8_t*, std::int32_t) override { calls.push_back("network"); }
    sim::Vec3i feature_anim_pos(std::uint16_t) override { return {}; }
};

void test_projectiles() {
    sim::UnitTypeInfo type;
    type.max_damage = 1000;
    type.model_min_x = type.model_min_z = -8 << 16;
    type.model_max_x = type.model_max_z = 8 << 16;
    type.model_max_y = 20 << 16;
    sim::UnitTickTypeInfo tick_type;
    tick_type.name = "TANK";
    std::vector<Unit> units(4);
    std::vector<UnitTickState> ticks(4);
    std::vector<PlayerSlot> players(11);
    for (int p = 0; p < 2; ++p) {
        players[static_cast<std::size_t>(p)].active = true;
        players[static_cast<std::size_t>(p)].controller = 1;
    }
    const std::int32_t xs[4] = {0, 40, 100, 140};
    for (std::uint16_t i = 1; i <= 3; ++i) {
        units[i].id = i;
        units[i].type = &type;
        units[i].hit_points = 500;
        units[i].flags = unit_flags::kAlive;
        units[i].pos = {xs[i] << 16, 10 << 16, 100 << 16};
        ticks[i].tick_type = &tick_type;
        ticks[i].owner = i == 1 ? 0 : 1;
    }
    sim::CombatEnvironment cenv;
    CombatLog clog;
    sim::Combat c{units, ticks, players, cenv, clog};
    sim::HeightMap hm(16, 16);
    sim::MapGrid map;
    map.width = map.height = 16;
    map.cells.resize(256);
    map.cells[6 * 16 + 6].unit = 2;
    const std::vector<sim::FeatureTypeInfo> features;
    sim::ProjectileTable table;
    sim::ProjectileEnvironment env;
    ProjectileLog log;
    sim::BattleRng rng;
    sim::ProjectileWorld w{table, c, hm, map, features, env, log, rng};

    // A line-of-sight shot flies 16 units a step along +x and hits unit 2 on the fourth step.
    sim::WeaponType gun;
    gun.flags = sim::weapon_flags::kLineOfSight;
    gun.velocity = 16 << 16;
    gun.range = 1000;
    gun.default_damage = 100;
    log.weapon = &gun;
    env.game_tick = 1;
    const sim::FireSlot slot{&gun, 0, 0, 0, 0};
    CHECK_EQ(sim::fire_directed(w, slot, units[1], units[1].pos, units[2].pos, &units[2]), true);
    CHECK_EQ(table.count, 1);
    CHECK_EQ(table.slots[0].yaw, 0xC000);
    CHECK_EQ(table.slots[0].vel[0], 16 << 16);
    CHECK_EQ(table.slots[0].expire_tick, 1u + (1000u << 16) / (16u << 16));
    CHECK_EQ(units[1].cloak_holdoff_tick, 601u);
    for (std::uint32_t t = 2; t <= 4; ++t) {
        env.game_tick = t;
        sim::update_projectiles(w);
    }
    CHECK_EQ(table.count, 1);
    CHECK_EQ(units[2].hit_points, 500);
    env.game_tick = 5;
    sim::update_projectiles(w);
    CHECK_EQ(table.count, 0);
    CHECK_EQ(units[2].hit_points, 400);
    CHECK_EQ(units[1].net_dirty, 0x4000);  // more damage to enemies than to friends

    // Area damage falls off with the distance to the unit's box: 32 of a 64 radius with
    // edge effectiveness 0.5 gives 0.25 * 0.5 + 0.5 = 0.625.
    sim::WeaponType blast;
    blast.area_of_effect = 128;
    blast.default_damage = 100;
    blast.edge_effectiveness = 0.5f;
    sim::UnitTypeInfo bomb = type;
    bomb.explode_as = &blast;
    units[3].type = &bomb;
    sim::detonate_unit_death(w, units[3], false);
    CHECK_EQ(units[2].hit_points, 400 - 62);
    CHECK_EQ(units[1].hit_points, 500);  // 92 units from the box: out of reach

    // Compaction keeps order; a moved chaser has its chased slot renumbered, an unmoved one keeps the old slot.
    for (int i = 0; i < 5; ++i) sim::alloc_projectile(table)->weapon = &gun;
    table.slots[1].flags = sim::projectile_flags::kDeleted;
    table.slots[3].flags = sim::projectile_flags::kDeleted;
    table.slots[4].intercepted = 2;
    table.slots[0].intercepted = 4;
    table.slots[4].speed = 44;
    sim::compact_projectiles(table);
    CHECK_EQ(table.count, 3);
    CHECK_EQ(table.slots[2].speed, 44);
    CHECK_EQ(table.slots[2].intercepted, 1);
    CHECK_EQ(table.slots[0].intercepted, 4);
    // The table is full at 300.
    while (sim::alloc_projectile(table)) {
    }
    CHECK_EQ(table.count, sim::kMaxProjectiles);
}

// Records what reaches the outside of a battle; everything else does nothing.
struct NullOutside : sim::BattleOutside {
    int sounds = 0, explosions = 0, missions = 0;
    void motion_step(Unit&, std::uint32_t&) override {}
    std::int32_t motion_speed(const Unit&) override { return 0; }
    std::uint32_t motion_start_tick(const Unit&) override { return 0; }
    void release_motion(Unit&) override {}
    void release_attached(Unit&, sim::MissionOrder&) override {}
    std::int32_t run_mission(Unit&, sim::MissionOrder&, std::uint32_t&) override {
        ++missions;
        return sim::mission_result::kContinue;
    }
    void retarget(Unit*, Unit&, std::uint16_t) override {}
    void attach_unit(Unit&, std::int32_t, std::int32_t, std::int32_t) override {}
    void drop_unit(Unit&, std::int32_t) override {}
    void detach_from_carrier(Unit&) override {}
    bool yard_cells_free(Unit&, std::int32_t) override { return false; }
    void claim_yard_cells(Unit&) override {}
    void clear_footprint(Unit&) override {}
    void release_unit(Unit&) override {}
    bool pay_energy(Unit&, float) override { return true; }
    void damage_feature(sim::MapCell&, std::int32_t, std::int32_t, const sim::WeaponType&) override {}
    sim::Vec3i feature_anim_pos(std::uint16_t) override { return {}; }
    void spawn_wreck(Unit&, std::int32_t, bool) override {}
    void explode_piece(Unit&, std::int32_t, std::int32_t) override {}
    void emit_sfx(Unit&, std::int32_t, std::int32_t) override {}
    void reveal_dead_unit(Unit&) override {}
    void forget_fog(Unit&) override {}
    void victory_unit_destroyed(Unit&) override {}
    void player_out_of_units(int) override {}
    void commander_died(int) override {}
    std::uint32_t clock_ticks() override { return 0; }
    void network_send(std::int32_t, const std::uint8_t*, std::int32_t) override {}
    void broadcast_player_units(int) override {}
    void broadcast_script_call(Unit&, std::int16_t) override {}
    void on_activate_flags_changed(Unit&, std::uint8_t, std::uint8_t) override {}
    void forget_speech(Unit&) override {}
    void leave_group(Unit&) override {}
    void stats_activity(std::int32_t) override {}
    void kill_counted(std::uint8_t, Unit&) override {}
    void stockpile_used(Unit&) override {}
    void camera_follow_step() override {}
    void play_sound(std::uint16_t, const sim::Vec3i&) override { ++sounds; }
    void spawn_smoke(const sim::Vec3i&) override {}
    void spawn_muzzle_smoke(const sim::Vec3i&) override {}
    void spawn_explosion(const sim::Vec3i&, const sim::WeaponType&, bool, bool) override { ++explosions; }
    void camera_shake(std::int32_t, std::int32_t, std::int32_t) override {}
};

// A shooter and a target wired through a whole battle: the unit pass aims and
// fires, the projectile update flies the shot until it hits the target.
void test_battle() {
    sim::HeightMap hm(16, 16);
    NullOutside outside;
    sim::BattleRng rng;
    rng.set_state(12345);
    sim::Battle b(outside, hm, rng);
    b.map.width = 16;
    b.map.height = 16;
    b.map.cells.resize(256);
    b.types.resize(2);
    b.types[1].info.max_damage = 1000;
    b.types[1].tick.damage_modifier = 0x10000;
    b.types[1].tick.name = "BOX";
    // A 32-unit cube model; the target occupies its map cells (the footprint system is not built yet).
    b.types[1].info.model_min_x = b.types[1].info.model_min_z = -(16 << 16);
    b.types[1].info.model_max_x = b.types[1].info.model_max_z = 16 << 16;
    b.types[1].info.model_min_y = 0;
    b.types[1].info.model_max_y = 32 << 16;
    for (int z = 5; z <= 7; ++z)
        for (int x = 6; x <= 8; ++x) b.map.cells[static_cast<std::size_t>(z * 16 + x)].unit = 2;
    b.units.resize(3);
    b.ticks.resize(3);
    sim::PlayerSlot& p = b.players[0];
    p.active = true;
    p.controller = 1;
    p.first_unit = 1;
    p.last_unit = 1;
    p.alive_count = 1;
    sim::PlayerSlot& enemy = b.players[1];  // shots pass through their own player's units
    enemy.active = true;
    enemy.controller = 2;
    enemy.first_unit = 2;
    enemy.last_unit = 2;
    enemy.alive_count = 1;
    for (std::uint16_t i = 1; i <= 2; ++i) {
        Unit& u = b.units[i];
        u.id = i;
        u.type = &b.types[1].info;
        u.hit_points = 1000;
        u.flags = unit_flags::kAlive;
        u.pos = {(i == 1 ? 40 : 120) << 16, 40 << 16, 100 << 16};
        b.ticks[i].type_index = 1;
        b.ticks[i].tick_type = &b.types[1].tick;
        b.ticks[i].owner = i - 1;
    }
    sim::WeaponType gun;
    gun.fire = sim::WeaponFireRoutine::LineOfSight;
    gun.flags = sim::weapon_flags::kLineOfSight;
    gun.range = 400;
    gun.reload_time = 30;
    gun.velocity = 0x80000;  // 8 units per step
    gun.default_damage = 100;
    gun.tolerance = 0x7FFF;  // the shooter need not turn
    gun.pitch_tolerance = 0x7FFF;
    gun.weapon_timer = 100;
    sim::WeaponSlot& slot = b.units[1].weapons[0];
    slot.type = &gun;
    slot.flags = sim::slot_flags::kEnabled;
    slot.target_x = 2;  // unit 2
    slot.target_z = sim::kUnitTarget;
    b.settings.sea_level = 0;
    b.settings.gravity = 112;

    b.advance_tick();
    b.step_units_and_projectiles();
    CHECK_EQ(outside.sounds >= 1, true);  // the launch sound: a shot left
    CHECK_EQ(slot.reload > 0, true);
    for (int k = 0; k < 20; ++k) {
        b.advance_tick();
        b.step_units_and_projectiles();
    }
    CHECK_EQ(b.units[2].hit_points < 1000, true);  // the shot arrived
    CHECK_EQ(b.game_tick(), 21u);
}

} // namespace

int main() {
    test_sin_table_matches_fixture();
    test_sin_table_formula();
    test_battle_rng_golden();
    test_battle_rng_edges();
    test_lcg15();
    test_mul_sin_cos_golden();
    test_ftol();
    test_fixed();
    test_world_stage_order();
    test_step_budget();
    test_script_vm();
    test_x87_rotation();
    test_heightmap_and_packing();
    test_combat();
    test_weapons();
    test_orders();
    test_ground_settle();
    test_projectiles();
    test_battle();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
