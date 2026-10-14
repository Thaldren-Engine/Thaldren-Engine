// Simulation core tests. Expected values marked "golden" were produced by
// running the original routines in Ghidra's emulator (see docs/RE_MEMORY.md).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include <string>

#include "sim/battle.h"
#include "sim/battle_quiet.h"
#include "sim/combat.h"
#include "sim/motion.h"
#include "sim/fixed.h"
#include "sim/ground_settle.h"
#include "sim/heightmap.h"
#include "sim/orders.h"
#include "sim/projectile.h"
#include "sim/missions.h"
#include "sim/route_search.h"
#include "sim/unit.h"
#include "sim/rng.h"
#include "sim/script_vm.h"
#include "sim/step_budget.h"
#include "sim/targeting.h"
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
struct NullOutside : sim::QuietBattleOutside {
    int sounds = 0, explosions = 0, packets = 0;
    void network_send(std::int32_t, const std::uint8_t*, std::int32_t) override { ++packets; }
    void play_sound(std::uint16_t, const sim::Vec3i&) override { ++sounds; }
    void explosion_smoke(const sim::Vec3i&) override { ++explosions; }
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

// Target acquisition through the battle: the players' stage refreshes the enemy
// lists every 30 steps and the weapon-target pass points a released weapon at an
// enemy in range; an idle unit resolves and issues an attack order.
void test_targeting() {
    sim::HeightMap hm(16, 16);
    NullOutside outside;
    sim::BattleRng rng;
    rng.set_state(4242);
    sim::Battle b(outside, hm, rng);
    b.map.width = 16;
    b.map.height = 16;
    b.map.cells.resize(256);
    b.init_unit_table(4);
    b.assign_unit_range(0, 1, 1);
    b.assign_unit_range(1, 2, 3);
    b.types.resize(2);
    sim::BattleUnitType& ty = b.types[1];
    ty.info.max_damage = 100;
    ty.info.model_max_y = 20 << 16;
    ty.tick.type_flags = sim::targeting_type_flags::kShootMe | sim::targeting_type_flags::kHasWeapons;
    ty.mission.abilities = sim::targeting_abilities::kAttack | sim::targeting_abilities::kMove;
    ty.targeting.category_bit = 1;
    ty.targeting.sight_distance = 300;
    sim::WeaponType gun;
    gun.flags = sim::weapon_flags::kLineOfSight;
    gun.range = 250;
    ty.targeting.first_weapon = &gun;
    for (int p = 0; p < 2; ++p) {
        b.players[static_cast<std::size_t>(p)].active = true;
        b.players[static_cast<std::size_t>(p)].controller = static_cast<std::uint8_t>(1 + p);
        b.players[static_cast<std::size_t>(p)].state = static_cast<std::uint8_t>(p);
        b.alliances[static_cast<std::size_t>(p)][static_cast<std::size_t>(p)] = 1;  // every player is allied with itself
    }
    const std::int32_t xs[] = {0, 100, 300, 600};
    for (std::uint16_t i = 1; i <= 3; ++i) {
        Unit& u = b.units[i];
        u.type = &b.types[1].info;
        u.hit_points = 100;
        u.flags = unit_flags::kAlive | 0x80000000u | 0x200000u | 0x40000u | 1u;  // weapons, fire at will, maneuver
        u.pos = {xs[i] << 16, 0, 100 << 16};
        u.weapons[0].type = &gun;
        u.weapons[0].flags = sim::slot_flags::kEnabled | sim::slot_flags::kReleased;
        b.ticks[i].type_index = 1;
        b.ticks[i].tick_type = &b.types[1].tick;
        b.ticks[i].has_motion = true;
    }
    b.settings.unit_limit = 0;  // one unit per player and step
    b.start_players();
    CHECK_EQ(b.targeting().state.tables[0].exists && b.targeting().state.controllers[1].exists, true);
    // Before the first refresh the lists are empty: nothing to aim at.
    b.players_stage();
    CHECK_EQ(b.units[1].weapons[0].target_x, 0);
    for (int k = 0; k < 30; ++k) b.advance_tick();
    const std::uint32_t before = rng.state();
    b.players_stage();
    CHECK_EQ(rng.state() != before, true);  // the refresh draws
    CHECK_EQ(b.targeting().state.tables[0].seen.size(), 2u);
    // The weapon-target pass ran before this refresh; the next stage aims.
    CHECK_EQ(b.units[1].weapons[0].target_x, 0);
    b.advance_tick();
    b.players_stage();
    // Player 0's unit at x 100: unit 2 (200 away) is in range, unit 3 (500) is not.
    CHECK_EQ(b.units[1].weapons[0].target_x, 2);
    CHECK_EQ(b.units[1].weapons[0].target_z, sim::kUnitTarget);
    std::vector<Unit*> near;
    sim::enemies_in_radius(b.targeting(), 0, b.units[1].pos, 250, near);
    CHECK_EQ(near.size(), 1u);
    // Attack orders: a mobile unit chases; on "maneuver" a move back goes behind the attack.
    CHECK_EQ(sim::resolve_mission(b.targeting(), sim::intent::kAttack, b.units[1], &b.units[2], nullptr), sim::mission::kAttackChase);
    CHECK_EQ(sim::resolve_mission(b.targeting(), sim::intent::kStop, b.units[1], nullptr, nullptr), sim::mission::kStop);
    CHECK_EQ(sim::issue_attack_order(b.targeting(), b.units[1], b.units[2], false), true);
    const MissionOrder* head = b.units[1].orders;
    CHECK_EQ(head != nullptr && head->kind == sim::mission::kAttackChase && head->target.unit == &b.units[2], true);
    CHECK_EQ(head && head->next && head->next->kind == sim::mission::kMoveGround, true);
    CHECK_EQ(head ? head->scratch0 : 0u, (100u) | (100u << 16));
    CHECK_EQ(b.targeting().fault.empty(), true);
}

// Unit creation and footprints through the battle: a structure claims its yard
// cells, a mobile unit its ground cells, limits and table ranges are respected,
// and death releases the cells.
void test_unit_create() {
    sim::HeightMap hm(32, 32);
    NullOutside outside;
    sim::BattleRng rng;
    rng.set_state(777);
    sim::Battle b(outside, hm, rng);
    b.map.width = 32;
    b.map.height = 32;
    b.map.cells.resize(32 * 32);
    for (auto& c : b.map.cells) c.metal = 3;
    b.init_buckets();
    CHECK_EQ(b.buckets.width, 4);
    b.init_unit_table(10);
    b.assign_unit_range(0, 1, 4);
    b.assign_unit_range(1, 5, 8);
    b.players[0].active = b.players[1].active = true;
    b.players[0].controller = 1;
    b.players[1].controller = 2;
    b.types.resize(3);
    // Type 1: a 2 x 2 structure that occupies its cells while closed and extracts metal.
    sim::BattleUnitType& st = b.types[1];
    st.info.max_damage = 500;
    st.info.yard_map = {0x05, 0x04, 0x04, 0x01};
    st.create.type_flags = sim::create_flags::kLoadable;
    st.create.movement = 0;
    st.create.footprint_x = st.create.footprint_z = 2;
    st.create.build_angle = 0x1000;
    st.create.extracts_metal = 0.5f;
    st.create.max_per_player = 2;
    // Type 2: a mobile 1 x 1 unit.
    sim::BattleUnitType& mt = b.types[2];
    mt.info.max_damage = 100;
    mt.create.type_flags = sim::create_flags::kLoadable;
    mt.create.movement = 1;
    mt.create.footprint_x = mt.create.footprint_z = 1;
    mt.create.build_angle = 0x4000;
    // One movement class of 1 x 1 cells: its grid is built from the empty flat map.
    b.move_classes[0].named = true;
    b.move_classes[0].footprint_x = b.move_classes[0].footprint_z = 1;
    b.init_routes();
    CHECK_EQ(b.move_classes[0].grade(5, 3), 3u);

    const std::int32_t x = (100 << 16), z = (60 << 16);
    Unit* s = b.create_unit(0, 1, x, 0, z, true, 1);
    CHECK_EQ(s != nullptr, true);
    if (!s) return;
    CHECK_EQ(s->id, 1);
    CHECK_EQ(s->hit_points, 500);
    CHECK_EQ(s->cell_x, 5);  // (100 - 16 + 8) / 16
    CHECK_EQ(s->cell_z, 3);
    CHECK_EQ((s->flags & sim::footprint_flags::kStructure) != 0, true);
    CHECK_EQ(b.map.cells[3 * 32 + 5].unit, 1);  // 0x05: occupied while closed, marked
    CHECK_EQ(b.map.cells[3 * 32 + 6].unit, 1);
    CHECK_EQ(b.map.cells[4 * 32 + 6].unit, 0);  // 0x01: only marked
    CHECK_EQ(b.map.cells[4 * 32 + 6].flags & 2, 2);
    CHECK_EQ(s->metal_extraction, 0.5f * 16.0f);  // four cells of metal 3 (+1 each)
    CHECK_EQ(s->bucket, 0);
    CHECK_EQ(b.buckets.heads[0] == s, true);
    CHECK_EQ(b.move_classes[0].grade(5, 3), 0u);  // the structure's cells block the class now
    CHECK_EQ(b.move_classes[0].grade(4, 2), 1u);  // next to it: rough
    CHECK_EQ(b.players[0].alive_count, 1);
    CHECK_EQ(b.players[0].units_created, 1u);
    // A second structure on the same spot: the first keeps the cells (its owner is not watched).
    CHECK_EQ(b.create_unit(0, 1, x, 0, z, true, 1, 3) != nullptr, true);
    CHECK_EQ(b.create_unit(0, 1, x, 0, z, true, 1) == nullptr, true);  // limit of 2 reached
    CHECK_EQ(b.units[3].flags & sim::footprint_flags::kWantsCells, sim::footprint_flags::kWantsCells);
    CHECK_EQ(s->flags & sim::footprint_flags::kHoldsWanted, sim::footprint_flags::kHoldsWanted);
    CHECK_EQ(b.map.cells[3 * 32 + 5].unit, 1);
    // A mobile unit gets a motion object and faces its build angle.
    Unit* m = b.create_unit(1, 2, (200 << 16), 0, (200 << 16), false, 1);
    CHECK_EQ(m != nullptr, true);
    if (!m) return;
    CHECK_EQ(m->id, 5);
    CHECK_EQ(b.ticks[5].has_motion, true);
    CHECK_EQ(m->heading, 0x4000);
    CHECK_EQ(m->hit_points, 0);
    CHECK_EQ(m->build_fraction_left, 1.0f);
    CHECK_EQ(b.map.cells[12 * 32 + 12].unit, 5);
    CHECK_EQ(b.create_unit(1, 2, 0, 0, 0, true, 1, 2) == nullptr, true);  // outside player 1's range
    // Death releases the cells; the unit that wanted them claims them.
    sim::clear_footprint(b.footprint_world(), *s);
    CHECK_EQ(b.map.cells[3 * 32 + 5].unit, 3);
    CHECK_EQ(s->bucket, sim::kNoBucket);
    CHECK_EQ(b.footprint_world().fault.empty(), true);
    // Unit 3's closed cells are its own now, so they are free for it.
    CHECK_EQ(sim::yard_cells_free(b.footprint_world(), b.units[3], 0), true);
}

struct TestMotionSystems : sim::MotionSystems {
    int forgets = 0, claims = 0;
    bool place = true;
    std::uint32_t next = 0x1000;
    bool can_place(sim::Unit&, std::int16_t, std::int16_t, std::uint32_t) override { return place; }
    void clear_footprint(sim::Unit&) override {}
    void claim_footprint(sim::Unit&) override { ++claims; }
    void update_sight(sim::Unit&) override {}
    bool off_map(const sim::Unit&) override { return false; }
    std::uint8_t bucket_height(const sim::Unit&) override { return 10; }
    void route_search_forget(sim::MotionGoal&) override { ++forgets; }
    void set_activated(sim::Unit&, bool) override {}
    std::uint32_t allocate_object(std::uint32_t size) override {
        const std::uint32_t a = next;
        next += (size + 3) & ~3u;
        return a;
    }
    void free_object(std::uint32_t) override {}
};

// A unit ordered to move to the far side of a wall: the route search gives it a
// route of corners around the wall and the unit drives there and finishes the order.
void test_route_search() {
    constexpr int kW = 48;
    sim::HeightMap hm(kW, kW);
    NullOutside outside;
    sim::BattleRng rng;
    rng.set_state(4242);
    sim::Battle b(outside, hm, rng);
    b.map.width = b.map.height = kW;
    b.map.cells.resize(kW * kW);
    b.feature_blocks = {1};  // feature type 0 blocks
    for (int z = 6; z <= 40; ++z) b.map.cells[static_cast<std::size_t>(z * kW + 24)].feature = 0;
    b.init_buckets();
    b.init_unit_table(10);
    b.assign_unit_range(0, 1, 4);
    b.players[0].active = true;
    b.players[0].controller = 1;
    b.settings.player_count = 1;
    b.settings.units_per_player = 4;
    sim::MoveClass& mc = b.move_classes[3];
    mc.named = true;
    mc.footprint_x = mc.footprint_z = 2;
    b.types.resize(2);
    sim::BattleUnitType& t = b.types[1];
    t.info.max_damage = 100;
    t.create.type_flags = sim::create_flags::kLoadable;
    t.create.movement = 1;
    t.create.footprint_x = t.create.footprint_z = 2;
    t.motion.max_velocity = 0x20000;
    t.motion.acceleration = 0x2000;
    t.motion.brake_rate = 0x4000;
    t.motion.turn_rate = 0x800;
    t.motion.has_move_class = true;
    t.route.move_class = 3;
    b.init_routes();
    CHECK_EQ(mc.grade(10, 20), 3u);
    CHECK_EQ(mc.grade(23, 20), 0u);  // a 2 x 2 footprint at x 23 covers the wall
    CHECK_EQ(mc.grade(22, 20), 1u);  // next to it: rough
    sim::Unit* u = b.create_unit(0, 1, (10 * 16 + 16) << 16, 0, (20 * 16 + 16) << 16, true, 1);
    CHECK_EQ(u != nullptr, true);
    if (!u) return;
    CHECK_EQ(b.ticks[u->id].has_motion, true);
    const std::int32_t goal[3] = {(38 * 16 + 16) << 16, 0, (20 * 16 + 16) << 16};
    CHECK_EQ(sim::enqueue_order(b.order_context(), *u, sim::mission::kMoveGround, false, nullptr, goal, 0, 0), true);
    std::int32_t most_points = 0, steps = 0;
    bool crossed_wall = false;
    for (; steps < 2000 && u->orders; ++steps) {
        b.advance_tick();
        b.step_units_and_projectiles();
        b.run_route_search();
        auto* rg = dynamic_cast<sim::RouteGoal*>(b.ticks[u->id].motion.goal.get());
        if (rg && rg->count > most_points) most_points = rg->count;
        if (u->cell_x <= 24 && u->cell_x + u->footprint_x > 24 && u->cell_z >= 6 && u->cell_z <= 40) crossed_wall = true;
    }
    CHECK_EQ(b.route_context().fault.empty(), true);
    CHECK_EQ(u->orders == nullptr, true);   // the move finished
    CHECK_EQ(most_points >= 3, true);       // a route with corners around the wall
    CHECK_EQ(crossed_wall, false);          // never stood on the wall
    CHECK_EQ(u->cell_x, 38);
    CHECK_EQ(u->cell_z, 20);
    std::printf("route search test: arrived after %d steps, longest route %d points\n", static_cast<int>(steps), static_cast<int>(most_points));
}

void test_motion() {
    using sim::footprint_cell;
    // Corner cell of a 2-cell footprint centred at x = 100.
    CHECK_EQ(footprint_cell(100 << 16, 2), 5);
    std::vector<sim::Unit> units(3);
    std::vector<sim::UnitTickState> ticks(3);
    std::vector<sim::PlayerSlot> players(1);
    players[0].active = true;
    players[0].controller = 1;
    std::vector<sim::MotionType> types(2);
    types[1].max_velocity = 0x20000;
    types[1].brake_rate = 0x1000;
    types[1].acceleration = 0x800;
    types[1].turn_rate = 0x400;
    sim::HeightMap hm(8, 8);
    TestMotionSystems sys;
    sim::MotionContext c{units, ticks, players, types, hm, sys, {}, {}};
    for (int i = 1; i < 3; ++i) {
        units[static_cast<std::size_t>(i)].id = static_cast<std::uint16_t>(i);
        ticks[static_cast<std::size_t>(i)].type_index = 1;
        units[static_cast<std::size_t>(i)].footprint_x = units[static_cast<std::size_t>(i)].footprint_z = 2;
        units[static_cast<std::size_t>(i)].pos = {40 << 16, 20 << 16, 40 << 16};
    }
    sim::Unit& u = units[1];
    sim::create_motion(c, u);
    CHECK_EQ(ticks[1].has_motion, true);
    auto* route = dynamic_cast<sim::RouteGoal*>(ticks[1].motion.goal.get());
    CHECK_EQ(route != nullptr, true);
    // Speed is capped by the max velocity, scaled down on slopes and halved under water.
    sim::accelerate(c, u, 0x30000);
    CHECK_EQ(ticks[1].motion.speed, 0x20000);
    CHECK_EQ(ticks[1].velocity[2], -0x20000);  // heading 0 moves toward -z
    u.bank = 0x3000;  // past the steepest slope entry: 15%
    sim::accelerate(c, u, 0);
    CHECK_EQ(ticks[1].motion.speed, 19660);
    c.env.sea_level = 30;
    sim::accelerate(c, u, 0);
    CHECK_EQ(ticks[1].motion.speed, 9830);
    // Stale stack word left by the motion step.
    sim::UnitTickEnvironment env;
    env.addresses.game_state = 0x100;
    CHECK_EQ(sim::motion_stale_word(env), 0x100u + 0x14353u);
    env.water_damage_enabled = true;
    env.water_damage = 5;
    CHECK_EQ(sim::motion_stale_word(env), 30u);
    // Routes keep at most 20 waypoints; reaching the next one drops the previous one.
    std::vector<std::uint32_t> pts;
    for (int k = 0; k < 25; ++k) pts.push_back(static_cast<std::uint32_t>(40 + k) | (40u << 16));
    route->set_route(c, pts.data(), 25);
    CHECK_EQ(route->count, 20);
    CHECK_EQ(route->has_route(), true);
    u.pos[0] = 41 << 16;  // at waypoint 1
    route->step(c);
    CHECK_EQ(route->count, 19);
    CHECK_EQ(route->points[0], pts[1]);
    route->set_route(c, pts.data(), 0);
    CHECK_EQ(route->has_route(), false);
    // Order goals report to their order.
    sim::MissionOrder order;
    order.unit = &u;
    u.cell_x = 10;
    u.cell_z = 10;
    auto circle = sim::make_circle_goal(c, order, (10 * 16 + 16) << 16, (10 * 16 + 16) << 16, 50);
    CHECK_EQ(circle->radius_cells_sq, 9);
    CHECK_EQ(circle->contains_cell(circle->cell_x + 3, circle->cell_z), true);
    CHECK_EQ(circle->contains_cell(circle->cell_x + 3, circle->cell_z + 1), false);
    CHECK_EQ(circle->distance(circle->cell_x + 10, circle->cell_z), 130);
    sim::attach_order_goal(c, order, circle);
    CHECK_EQ(order.goal.get() == circle.get(), true);
    CHECK_EQ(route->attached == circle.get(), true);
    sim::release_order_goal(c, u, order);
    CHECK_EQ(order.attached, 0u);
    CHECK_EQ(order.events & sim::goal_events::kReplaced, sim::goal_events::kReplaced);
    CHECK_EQ(sys.forgets >= 2, true);
}

// Map features through the battle: a tree catches fire, burns out into its burnt
// type, a wreck lands on dry ground and another sinks; a script blows a piece off
// (battle RNG draws, the debris cache) and shatters another into a shard.
void test_features_and_effects() {
    sim::HeightMap hm(16, 16);
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) hm.at(x, z) = static_cast<std::uint8_t>(x < 10 ? 50 : 5);
    NullOutside outside;
    sim::BattleRng rng;
    rng.set_state(4242);
    sim::Battle b(outside, hm, rng);
    b.map.width = 16;
    b.map.height = 16;
    b.map.cells.resize(256);
    for (int i = 0; i < 256; ++i) b.map.cells[static_cast<std::size_t>(i)].height = hm.at(i % 16, i / 16);
    sim::refresh_height_range(b.map, 0, 0, 16, 16);
    b.settings.sea_level = 20;
    b.settings.gravity = 112;
    b.settings.damage_options = 8;
    // A three-frame burning animation (shown 1, 2 and 1 steps), not looping.
    sim::AnimSequence burn;
    for (int d : {1, 2, 1}) burn.frames.push_back({static_cast<std::uint16_t>(d), 20, 30, 10, 25});
    const std::uint32_t burn_seq = b.anims.add(burn);
    b.features.resize(3);
    sim::FeatureTypeInfo& tree = b.features[0];  // 2D, flammable, burns into a stump
    tree.footprint_x = tree.footprint_z = 1;
    tree.flags = sim::feature_type_flags::kFlat | sim::feature_type_flags::kFlammable;
    tree.spark_time = 4;
    tree.damage = 100;
    tree.burnt = 1;
    tree.sequences[sim::kSeqBurn] = burn_seq;
    sim::FeatureTypeInfo& stump = b.features[1];
    stump.footprint_x = stump.footprint_z = 1;
    stump.flags = sim::feature_type_flags::kFlat;
    stump.damage = 50;
    sim::FeatureTypeInfo& heap = b.features[2];  // 3D, 2 x 2
    heap.footprint_x = heap.footprint_z = 2;
    heap.damage = 300;
    heap.dead = 1;
    heap.model = 7;
    b.reset_features_and_effects();
    sim::FeatureWorld& fw = b.feature_world();
    CHECK_EQ(sim::place_feature(fw, 3 * 16 + 3, 0, nullptr, nullptr, 10), -1);  // 2D: no slot
    CHECK_EQ(b.map.cells[3 * 16 + 3].feature, 0);
    CHECK_EQ((b.map.cells[3 * 16 + 3].flags >> 3) & 0xF, 10);
    // Weapon damage below its strength accumulates in the cell.
    sim::WeaponType gun;
    gun.default_damage = 40;
    sim::damage_feature(fw, b.map.cells[3 * 16 + 3], 3, 3, gun);
    CHECK_EQ(b.map.cells[3 * 16 + 3].feature_aux, 40);
    // A fire starter sets it alight: slot 0, burning, spreads after 2 or 3 steps.
    gun.fire_starter = 50;
    const std::uint32_t before = rng.state();
    sim::damage_feature(fw, b.map.cells[3 * 16 + 3], 3, 3, gun);
    CHECK_EQ(b.map.cells[3 * 16 + 3].flags & 1, 1);
    CHECK_EQ(b.map.cells[3 * 16 + 3].feature_aux, 0);
    const sim::FeatureSlot& slot = b.feature_state.slots[0];
    CHECK_EQ(slot.get<std::uint8_t>(sim::FeatureSlot::kFlags) & sim::feature_slot_flags::kBurning, 1);
    CHECK_EQ(slot.get<std::uint8_t>(sim::FeatureSlot::kFireCountdown) >= 2, true);
    CHECK_EQ(rng.state() != before, true);  // the countdown's draw
    CHECK_EQ(b.feature_state.active, 0);
    CHECK_EQ(b.feature_state.free, 1);
    // It burns for 4 steps, then the stump replaces it and the slot is free again.
    for (int k = 0; k < 4; ++k) {
        b.advance_tick();
        b.step_features();
    }
    CHECK_EQ(b.map.cells[3 * 16 + 3].feature, 1);
    CHECK_EQ(b.map.cells[3 * 16 + 3].flags & 1, 0);
    CHECK_EQ(b.feature_state.active, -1);
    CHECK_EQ(b.feature_state.free, 0);
    CHECK_EQ(b.feature_state.fault.empty(), true);

    // Wrecks: a 3D corpse at the unit's cells, place and attitude; it comes to rest at once on
    // dry ground and sinks in the deep water (x >= 10 is below the sea).
    sim::UnitTypeInfo dead_type;
    dead_type.corpse = 2;
    b.types.resize(2);
    b.init_unit_table(4);
    b.assign_unit_range(0, 1, 3);
    sim::Unit& u = b.units[1];
    u.type = &dead_type;
    u.cell_x = 5;
    u.cell_z = 6;
    u.pos = {(5 * 16 + 16) << 16, 50 << 16, (6 * 16 + 16) << 16};
    u.heading = 0x4000;
    sim::spawn_wreck(fw, u, {2, false, 0}, 1, false);
    CHECK_EQ(b.map.cells[6 * 16 + 5].feature, 2);
    CHECK_EQ(b.map.cells[6 * 16 + 6].feature, sim::kFeaturePart);
    CHECK_EQ(b.map.cells[6 * 16 + 5].feature_aux, 0);
    CHECK_EQ(b.feature_state.slots[0].get<std::uint16_t>(sim::FeatureSlot::kAngles + 2), 0x4000);
    CHECK_EQ(b.feature_state.slots[0].pos()[0], u.pos[0]);
    // Level 2 skips one stage down the dead chain: the stump.
    sim::Unit& v = b.units[2];
    v.type = &dead_type;
    v.cell_x = 2;
    v.cell_z = 10;
    v.pos = {(2 * 16 + 8) << 16, 50 << 16, (10 * 16 + 8) << 16};
    sim::spawn_wreck(fw, v, {2, false, 0}, 2, false);
    CHECK_EQ(b.map.cells[10 * 16 + 2].feature, 1);
    sim::Unit& w = b.units[3];
    w.type = &dead_type;
    w.cell_x = 12;
    w.cell_z = 4;
    w.pos = {(12 * 16 + 16) << 16, 30 << 16, (4 * 16 + 16) << 16};
    sim::spawn_wreck(fw, w, {2, false, 0}, 1, true);
    const int sunk = b.map.cells[4 * 16 + 12].feature_aux;
    CHECK_EQ(b.feature_state.slots[static_cast<std::size_t>(sunk)].vel()[1], -0x2CCC);
    b.advance_tick();
    b.step_features();
    CHECK_EQ(b.feature_state.idle, 0);  // the dry one is at rest
    CHECK_EQ(b.feature_state.slots[static_cast<std::size_t>(sunk)].pos()[1], (30 << 16) - 0x2CCC);

    // Script explode: a falling piece (6 draws, the piece is hidden, the debris cache is used).
    b.settings.gravity = 0x8000;  // strong gravity: everything lands within the test's steps
    sim::PoseModel pm;
    pm.root = 0;
    pm.nodes.emplace_back();
    pm.nodes[0].vertices = {{-(4 << 16), 0, -(4 << 16)}, {4 << 16, 0, -(4 << 16)}, {4 << 16, 0, 4 << 16}, {-(4 << 16), 0, 4 << 16}};
    pm.nodes[0].primitives.push_back({{0, 1, 2, 3}, false});
    u.pose.build(pm, {});
    u.pose.pieces[0].flags = sim::pose_flags::kVisible;
    u.pose.sync(0, 0, 0);
    sim::BattleRng ref;
    ref.set_state(rng.state());
    sim::EffectsWorld& ew = b.effects_world();
    sim::explode_piece(ew, u, 0, 0x04 | 0x02);  // fall, explode on landing
    for (int k = 0; k < 6; ++k) ref.next_mod(3000);
    CHECK_EQ(rng.state(), ref.state());
    CHECK_EQ(u.pose.pieces[0].flags & sim::pose_flags::kVisible, 0);
    CHECK_EQ(b.effects.debris_block[0], 8);
    CHECK_EQ(b.effects.cache.cursor, 8 + 4 * 12 + 0x66);
    CHECK_EQ(b.effects.debris[0].flags, sim::debris_flags::kFall | sim::debris_flags::kExplodeOnLanding);
    // Shattered: one shard (6 more draws per quad plus 2 for its facing).
    ref.set_state(rng.state());
    sim::explode_piece(ew, u, 0, 0x01 | 0x20);  // bitmap only: no debris, no draws
    CHECK_EQ(rng.state(), ref.state());
    sim::explode_piece(ew, u, 0, 0x01);
    for (int k = 0; k < 6 + 6 + 2; ++k) ref.next_mod(3000);
    CHECK_EQ(rng.state(), ref.state());
    CHECK_EQ(b.effects.count, 1);
    CHECK_EQ(b.effects.effects[0].shard, 0);
    CHECK_EQ(b.effects.shards[0].free, 0);
    // The shard falls and lands; the debris piece bounces and lands, leaving an explosion.
    for (int k = 0; k < 200 && (b.effects.debris_block[0] != -1 || b.effects.count > 0); ++k) b.step_effects();
    CHECK_EQ(b.effects.debris_block[0], -1);
    CHECK_EQ(b.effects.shards[0].free, 0xFF);
    CHECK_EQ(b.effects.count, 0);
    CHECK_EQ(b.effects.fault.empty(), true);
}

// Line of sight through the battle: with fog, a player's enemy list holds only what its
// units' sight shapes cover; the explored map grows with them; the viewed player's radar
// marks units outside sight; a cloaked enemy is never seen.
void test_sight() {
    sim::HeightMap hm(48, 16);
    NullOutside outside;
    sim::BattleRng rng;
    rng.set_state(99);
    sim::Battle b(outside, hm, rng);
    b.map.width = 48;
    b.map.height = 16;
    b.map.cells.resize(48 * 16);
    b.init_buckets();
    b.init_unit_table(4);
    b.assign_unit_range(0, 1, 1);
    b.assign_unit_range(1, 2, 3);
    b.types.resize(2);
    sim::BattleUnitType& ty = b.types[1];
    ty.info.max_damage = 100;
    ty.info.model_max_y = 20 << 16;
    ty.tick.type_flags = sim::targeting_type_flags::kShootMe;
    ty.targeting.sight_distance = 300;
    ty.sight.sight = 300;
    ty.sight.radar = 700;
    ty.sight.model_size_y = 20 << 16;
    // One mask: a disc of radius 2 blocks (64 world units) around the unit's block.
    sim::SightMask m;
    m.width = m.height = 5;
    m.x_offset = m.y_offset = 2;
    for (int z = 0; z < 5; ++z)
        for (int x = 0; x < 5; ++x) m.covered.push_back((x - 2) * (x - 2) + (z - 2) * (z - 2) <= 4 ? 1 : 0);
    b.sight_tables.masks.push_back(m);
    for (int p = 0; p < 2; ++p) {
        b.players[static_cast<std::size_t>(p)].active = true;
        b.players[static_cast<std::size_t>(p)].controller = static_cast<std::uint8_t>(1 + p);
        b.players[static_cast<std::size_t>(p)].state = static_cast<std::uint8_t>(p);
        b.alliances[static_cast<std::size_t>(p)][static_cast<std::size_t>(p)] = 1;
    }
    b.settings.fog_of_war = true;
    b.settings.explore = true;
    b.settings.player_count = 2;
    b.settings.local_view = 0;
    b.settings.unit_limit = 0;
    b.init_sight();
    CHECK_EQ(b.explored.size(), 24u * 8u);
    const std::int32_t xs[] = {0, 100, 160, 600};
    for (std::uint16_t i = 1; i <= 3; ++i) {
        Unit& u = b.units[i];
        u.type = &b.types[1].info;
        u.hit_points = 100;
        u.flags = unit_flags::kAlive | 1u;
        u.activate_flags = unit_flags::kActivated;
        u.pos = {xs[i] << 16, 0, 100 << 16};
        b.ticks[i].type_index = 1;
        b.ticks[i].tick_type = &b.types[1].tick;
        sim::claim_footprint(b.footprint_world(), u);
    }
    b.rebuild_sight(true);
    // Player 0's unit at x 100 sees x 160 (two blocks away), not x 600.
    CHECK_EQ(b.unit_seen_by(0, b.units[2]), true);
    CHECK_EQ(b.unit_seen_by(0, b.units[3]), false);
    CHECK_EQ(b.unit_seen_by(0, b.units[1]), true);  // its own
    CHECK_EQ((b.explored[3 * 24 + 3] & 1u) != 0 && (b.explored[3 * 24 + 18] & 1u) == 0, true);
    b.start_players();
    for (int k = 0; k < 30; ++k) b.advance_tick();
    b.players_stage();
    CHECK_EQ(b.targeting().state.tables[0].seen.size(), 1u);
    CHECK_EQ(b.targeting().state.tables[0].radar.size(), 0u);  // marks come after the refresh
    // The viewed player's radar (700 + the height bonus) marked unit 3.
    CHECK_EQ((b.units[3].flags & sim::sight_unit_flags::kRadar) != 0, true);
    // Cloaked: never seen.
    b.units[2].activate_flags |= unit_flags::kCloaked;
    CHECK_EQ(b.unit_seen_by(0, b.units[2]), false);
    // A move takes the sight along: at x 560 player 0 sees unit 3.
    b.units[1].pos[0] = 560 << 16;
    for (int k = 0; k < 30; ++k) b.advance_tick();
    b.players_stage();
    CHECK_EQ(b.unit_seen_by(0, b.units[3]), true);
    CHECK_EQ(b.sight_faults(), 0u);
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
    test_targeting();
    test_unit_create();
    test_motion();
    test_route_search();
    test_features_and_effects();
    test_sight();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
