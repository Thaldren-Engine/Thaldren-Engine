// Simulation core tests. Expected values marked "golden" were produced by
// running the original routines in Ghidra's emulator (see docs/RE_MEMORY.md).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include <string>

#include "sim/fixed.h"
#include "sim/rng.h"
#include "sim/script_vm.h"
#include "sim/step_budget.h"
#include "sim/trig.h"
#include "sim/world.h"
#include "sim/x87.h"
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
    CHECK_EQ(sim::x87::fpu_profile() == sim::x87::FpuProfile::IntelP6, true);
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
    sim::x87::set_fpu_profile(sim::x87::FpuProfile::IntelP6);
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
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
