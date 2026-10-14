// Battle step parts that draw the C runtime rand() outside the effect lists, against the
// original: the meteor shower (0x437DE0, its meteor launch 0x49DF10 replaced by a logging
// stub) and the camera shake (0x41C640 add, 0x41C6F0 decay). After every call: rand()
// state, the shower's or shake's whole state, and the launched meteors / view offsets.
//
// Usage: flow_harness <TotalA.exe> [scenarios] [operations] [first seed]

#include <cstdio>
#include <cstdlib>
#include <random>

#include "remote.h"
#include "sim/sky.h"

namespace {

using harness::u32;
using i32 = std::int32_t;

constexpr u32 kGameStatePtr = 0x00511DE8, kRand = 0x004E4870, kMeteorTick = 0x00437DE0, kSpawnProjectile = 0x0049DF10;
constexpr u32 kShakeAdd = 0x0041C640, kShakeDecay = 0x0041C6F0;
constexpr u32 kOffTick = 0x38A47, kOffW = 0x14233, kOffH = 0x14237, kOffScrollX = 0x1431F, kOffScrollY = 0x14323,
              kOffShakeMag = 0x1432F, kOffShakeLeft = 0x14333, kOffShakeX = 0x14337, kOffShakeY = 0x1433B, kOffCamFlags = 0x1434E,
              kOffUiFlags = 0x37F2F;
// Meteor globals.
constexpr u32 kMagnitude = 0x00512310, kInterval = 0x00512314, kActive = 0x00512318, kEnd = 0x0051231C, kAim = 0x00512320,
              kDuration = 0x00512324, kWeapon = 0x00512328, kEnabled = 0x0051232C, kNextSpawn = 0x00512330, kOrigin = 0x00512334,
              kGap = 0x00512338, kNextSchedule = 0x005122E8;

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: flow_harness <TotalA.exe> [scenarios] [operations] [first seed]\n");
        return 2;
    }
    const int scenarios = argc > 2 ? std::atoi(argv[2]) : 10;
    const int ops = argc > 3 ? std::atoi(argv[3]) : 200;
    const u32 first = argc > 4 ? static_cast<u32>(std::strtoul(argv[4], nullptr, 0)) : 1u;
    std::size_t total_ops = 0, schedules = 0, meteors = 0, shakes = 0, decays = 0, mismatches = 0;
    for (int sc = 0; sc < scenarios; ++sc) {
        const u32 seed = first + static_cast<u32>(sc);
        std::mt19937 g(seed);
        harness::Remote r;
        if (!r.start(argv[1])) {
            std::printf("cannot start %s\n", argv[1]);
            return 1;
        }
        const u32 gs = r.alloc(0x40000), stubs = r.alloc(0x1000, PAGE_EXECUTE_READWRITE), rs = r.alloc(0x1000), log = r.alloc(0x1000);
        Emitter e;
        {
            const u32 at = stubs + static_cast<u32>(e.b.size());
            e.u8(0xA1); e.u32le(rs);
            e.u8(0x69); e.u8(0xC0); e.u32le(214013u);
            e.u8(0x05); e.u32le(2531011u);
            e.u8(0xA3); e.u32le(rs);
            e.u8(0xC1); e.u8(0xE8); e.u8(0x10);
            e.u8(0x25); e.u32le(0x7FFF);
            e.u8(0xC3);
            while (e.b.size() % 16) e.u8(0xCC);
            if (!r.patch_jump(kRand, at)) return 1;
        }
        {
            // Meteor launch: log {count, weapon, pos[3], vel[3], flag}, ret 0x10.
            const u32 at = stubs + static_cast<u32>(e.b.size());
            e.u8(0xFF); e.u8(0x05); e.u32le(log);                                   // inc dword [log]
            e.u8(0x8B); e.u8(0x44); e.u8(0x24); e.u8(0x04); e.u8(0xA3); e.u32le(log + 4);   // weapon
            e.u8(0x8B); e.u8(0x4C); e.u8(0x24); e.u8(0x08);                         // mov ecx,[esp+8] pos
            for (int k = 0; k < 3; ++k) {
                e.u8(0x8B); e.u8(0x41); e.u8(static_cast<std::uint8_t>(4 * k));        // mov eax,[ecx+4k]
                e.u8(0xA3); e.u32le(log + 8 + 4 * static_cast<u32>(k));
            }
            e.u8(0x8B); e.u8(0x4C); e.u8(0x24); e.u8(0x0C);                         // vel
            for (int k = 0; k < 3; ++k) {
                e.u8(0x8B); e.u8(0x41); e.u8(static_cast<std::uint8_t>(4 * k));
                e.u8(0xA3); e.u32le(log + 0x14 + 4 * static_cast<u32>(k));
            }
            e.u8(0x8B); e.u8(0x44); e.u8(0x24); e.u8(0x10); e.u8(0xA3); e.u32le(log + 0x20);
            e.u8(0xC2); e.u8(0x10); e.u8(0x00);
            while (e.b.size() % 16) e.u8(0xCC);
            if (!r.patch_jump(kSpawnProjectile, at)) return 1;
        }
        r.write(stubs, e.b.data(), e.b.size());
        r.w32(kGameStatePtr, gs);
        const i32 w = 64 + static_cast<i32>(g() % 400), h = 64 + static_cast<i32>(g() % 400);
        r.w32(gs + kOffW, static_cast<u32>(w));
        r.w32(gs + kOffH, static_cast<u32>(h));
        sim::Lcg15 rand;
        rand.set_state(g());
        r.w32(rs, rand.state());
        // Shower configuration.
        sim::MeteorShower m;
        m.enabled = g() % 3 != 0;
        m.radius = static_cast<i32>(g() % 400);
        m.spawn_interval = 1 + static_cast<i32>(g() % 40);
        m.duration = static_cast<i32>(g() % 300);
        m.schedule_gap = static_cast<i32>(g() % 600);
        m.reset(nullptr);
        r.w32(kMagnitude, static_cast<u32>(m.radius));
        r.w32(kInterval, static_cast<u32>(m.spawn_interval));
        r.w32(kDuration, static_cast<u32>(m.duration));
        r.w32(kGap, static_cast<u32>(m.schedule_gap));
        r.w32(kEnabled, m.enabled ? 1 : 0);
        r.w32(kActive, 0);
        r.w32(kNextSchedule, static_cast<u32>(m.spawn_interval));
        r.w32(kWeapon, 0x1234);
        sim::CameraShake shake;
        const bool shake_off = g() % 8 == 0;
        r.w32(gs + kOffUiFlags, shake_off ? 0x10 : 0);
        u32 tick = 0;
        bool bad = false;
        for (int op = 0; op < ops && !bad; ++op) {
            ++total_ops;
            const u32 what = g() % 4;
            if (what < 2) {
                tick += 1 + (g() % 4 == 0 ? g() % 200 : 0);
                r.w32(gs + kOffTick, tick);
                const u32 before = r.r32(log);
                const u32 sched_before = m.next_schedule;
                std::int32_t pos[3], vel[3];
                const bool drop = sim::step_meteors(m, tick, w, h, rand, pos, vel);
                if (!r.call(kMeteorTick, 0, {})) {
                    std::printf("scenario %u: the original's meteor step failed\n", seed);
                    ++mismatches;
                    bad = true;
                    break;
                }
                schedules += m.next_schedule != sched_before;
                const bool orig_drop = r.r32(log) != before;
                bool ok = drop == orig_drop && r.r32(rs) == rand.state() && (r.r32(kActive) != 0) == m.active && r.r32(kEnd) == m.end_tick &&
                          r.r32(kNextSchedule) == m.next_schedule && r.r32(kNextSpawn) == m.next_spawn;
                const u32 origin = r.r32(kOrigin), aim = r.r32(kAim);
                ok = ok && static_cast<std::int16_t>(origin) == m.origin_x && static_cast<std::int16_t>(origin >> 16) == m.origin_z &&
                     static_cast<std::int16_t>(aim) == m.aim_x && static_cast<std::int16_t>(aim >> 16) == m.aim_z;
                if (ok && drop) {
                    ++meteors;
                    for (int k = 0; k < 3; ++k)
                        ok = ok && static_cast<i32>(r.r32(log + 8 + 4 * static_cast<u32>(k))) == pos[k] &&
                             static_cast<i32>(r.r32(log + 0x14 + 4 * static_cast<u32>(k))) == vel[k];
                    ok = ok && r.r32(log + 4) == 0x1234 && r.r32(log + 0x20) == 1;
                }
                if (!ok) {
                    std::printf("scenario %u: meteors differ at step %u\n", seed, tick);
                    ++mismatches;
                    bad = true;
                }
            } else if (what == 2) {
                const i32 dx = static_cast<i32>(g() % 20), dy = static_cast<i32>(g() % 20), mag = static_cast<i32>(g() % 40) - 5;
                sim::add_camera_shake(shake, dx, dy, mag, shake_off);
                if (!r.call(kShakeAdd, 0, {static_cast<u32>(dx), static_cast<u32>(dy), static_cast<u32>(mag)})) {
                    ++mismatches;
                    bad = true;
                    break;
                }
                ++shakes;
            } else {
                const i32 sx = static_cast<i32>(r.r32(gs + kOffScrollX)), sy = static_cast<i32>(r.r32(gs + kOffScrollY));
                sim::step_camera_shake(shake, rand);
                if (!r.call(kShakeDecay, 0, {})) {
                    ++mismatches;
                    bad = true;
                    break;
                }
                ++decays;
                const bool ok = r.r32(rs) == rand.state() && static_cast<i32>(r.r32(gs + kOffScrollX)) - sx == shake.offset_x &&
                                static_cast<i32>(r.r32(gs + kOffScrollY)) - sy == shake.offset_y;
                if (!ok) {
                    std::printf("scenario %u: camera shake differs\n", seed);
                    ++mismatches;
                    bad = true;
                    break;
                }
            }
            const bool same = ((r.rd<std::uint8_t>(gs + kOffCamFlags) & 1) != 0) == shake.shaking &&
                              static_cast<i32>(r.r32(gs + kOffShakeLeft)) == shake.ticks_left && static_cast<i32>(r.r32(gs + kOffShakeMag)) == shake.magnitude &&
                              static_cast<i32>(r.r32(gs + kOffShakeX)) == shake.accum_x && static_cast<i32>(r.r32(gs + kOffShakeY)) == shake.accum_y;
            if (!same && !bad) {
                std::printf("scenario %u: shake state differs\n", seed);
                ++mismatches;
                bad = true;
            }
        }
    }
    std::printf("scenarios %d, operations %zu, meteor steps with a new shower %zu, meteors %zu, shakes added %zu, shake steps %zu\n", scenarios,
                total_ops, schedules, meteors, shakes, decays);
    std::printf("%zu mismatches\n", mismatches);
    return mismatches ? 1 : 0;
}
