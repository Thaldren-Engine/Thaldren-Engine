// The user interface's order routine (0x48CF30, sim::command::issue_to_selected) against the
// original: a local player's part of a unit table with random selections, positions and
// abilities; the intent resolution (0x43F0E0), the mission table lookups (0x438830,
// 0x438760) and the per-unit order call (0x43AFC0) replaced by stubs that answer from shared
// tables and log. Compared: every resolution call (unit, intent, target) and every order
// (mission, queued, unit, target, position, parameters), in order.
//
// Usage: cmd_harness <TotalA.exe> [scenarios] [calls] [first seed]

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "remote.h"
#include "sim/commands.h"
#include "sim/orders.h"
#include "sim/unit.h"

namespace {

using harness::u32;
using i32 = std::int32_t;

constexpr u32 kGameStatePtr = 0x00511DE8, kRoutine = 0x0048CF30, kResolve = 0x0043F0E0, kIssue = 0x0043AFC0, kEntry = 0x00438830,
              kFindName = 0x00438760, kFireName = 0x0050120C;
constexpr u32 kUnitSize = 0x118;
constexpr u32 kOffLocal = 0x2A42, kOffPlayers = 0x1B63, kPlayerSize = 0x14B, kOffHover = 0x2CBA, kOffCursor = 0x2CAA, kOffUnits = 0x14357;
constexpr int kUnits = 40;

struct Emitter {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32le(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void bytes(std::initializer_list<std::uint8_t> l) {
        for (auto v : l) u8(v);
    }
};

struct Call {
    u32 mission, queued, unit, target, pos_ptr;
    i32 pos[3];
    i32 p0, p1;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: cmd_harness <TotalA.exe> [scenarios] [calls] [first seed]\n");
        return 2;
    }
    const int scenarios = argc > 2 ? std::atoi(argv[2]) : 10;
    const int calls = argc > 3 ? std::atoi(argv[3]) : 100;
    const u32 first_seed = argc > 4 ? static_cast<u32>(std::strtoul(argv[4], nullptr, 0)) : 1u;
    std::size_t total = 0, orders = 0, spread = 0, resolutions = 0, gated = 0, mismatches = 0;
    for (int sc = 0; sc < scenarios; ++sc) {
        const u32 seed = first_seed + static_cast<u32>(sc);
        std::mt19937 g(seed);
        harness::Remote r;
        if (!r.start(argv[1])) {
            std::printf("cannot start %s\n", argv[1]);
            return 1;
        }
        const u32 gs = r.alloc(0x40000), stubs = r.alloc(0x1000, PAGE_EXECUTE_READWRITE), units = r.alloc((kUnits + 2) * kUnitSize),
                  defs = r.alloc(0x2000), table = r.alloc(0x1000), mtable = r.alloc(0x1000), rlog = r.alloc(0x10000), ilog = r.alloc(0x10000),
                  click = r.alloc(0x100), posbuf = r.alloc(0x100);
        Emitter e;
        auto here = [&] { return stubs + static_cast<u32>(e.b.size()); };
        auto align = [&] {
            while (e.b.size() % 16) e.u8(0xCC);
        };
        {  // resolve(out, intent, unit, hover, cursor): mission from mtable[unit id], logged.
            const u32 at = here();
            e.bytes({0x8B, 0x44, 0x24, 0x0C}); e.u8(0x2D); e.u32le(units);
            e.bytes({0x31, 0xD2}); e.u8(0xB9); e.u32le(kUnitSize); e.bytes({0xF7, 0xF1});
            e.bytes({0x0F, 0xB6, 0x80}); e.u32le(mtable);
            e.bytes({0x8B, 0x4C, 0x24, 0x04, 0x88, 0x01});
            e.bytes({0x8B, 0x15}); e.u32le(rlog);
            e.bytes({0x8B, 0x44, 0x24, 0x08, 0x89, 0x02});        // intent
            e.bytes({0x8B, 0x44, 0x24, 0x0C, 0x89, 0x42, 0x04});  // unit
            e.bytes({0x8B, 0x44, 0x24, 0x10, 0x89, 0x42, 0x08});  // hover
            e.bytes({0x8B, 0x44, 0x24, 0x14, 0x89, 0x42, 0x0C});  // cursor pointer
            e.bytes({0x83, 0x05}); e.u32le(rlog); e.u8(0x10);
            e.bytes({0x89, 0xC8, 0xC2, 0x14, 0x00});
            align();
            if (!r.patch_jump(kResolve, at)) return 1;
        }
        {  // issue(mission, queued, unit, target, pos, p0, p1): logged.
            const u32 at = here();
            e.bytes({0x8B, 0x15}); e.u32le(ilog);
            e.bytes({0x8B, 0x44, 0x24, 0x04, 0x89, 0x02});
            e.bytes({0x8B, 0x44, 0x24, 0x08, 0x89, 0x42, 0x04});
            e.bytes({0x8B, 0x44, 0x24, 0x0C, 0x89, 0x42, 0x08});
            e.bytes({0x8B, 0x44, 0x24, 0x10, 0x89, 0x42, 0x0C});
            e.bytes({0x8B, 0x4C, 0x24, 0x14, 0x89, 0x4A, 0x10});
            e.bytes({0x85, 0xC9, 0x74, 0x12});
            e.bytes({0x8B, 0x01, 0x89, 0x42, 0x14, 0x8B, 0x41, 0x04, 0x89, 0x42, 0x18, 0x8B, 0x41, 0x08, 0x89, 0x42, 0x1C});
            e.u8(0x90);
            e.bytes({0x8B, 0x44, 0x24, 0x18, 0x89, 0x42, 0x20});
            e.bytes({0x8B, 0x44, 0x24, 0x1C, 0x89, 0x42, 0x24});
            e.bytes({0x83, 0x05}); e.u32le(ilog); e.u8(0x28);
            e.bytes({0xC2, 0x1C, 0x00});
            align();
            if (!r.patch_jump(kIssue, at)) return 1;
        }
        {  // table entry by index (ECX = &mission byte): table + 0x20 * mission.
            const u32 at = here();
            e.bytes({0x0F, 0xB6, 0x01, 0xC1, 0xE0, 0x05}); e.u8(0x05); e.u32le(table);
            e.u8(0xC3);
            align();
            if (!r.patch_jump(kEntry, at)) return 1;
        }
        {  // mission by name (ECX = out): the two standing orders.
            const u32 at = here();
            e.bytes({0x8B, 0x44, 0x24, 0x04}); e.u8(0x3D); e.u32le(kFireName);
            e.bytes({0xC6, 0x01, static_cast<std::uint8_t>(sim::mission::kStandingMoveOrder)});
            e.bytes({0x75, 0x03, 0xC6, 0x01, static_cast<std::uint8_t>(sim::mission::kStandingFireOrder)});
            e.bytes({0x89, 0xC8, 0xC2, 0x04, 0x00});
            align();
            if (!r.patch_jump(kFindName, at)) return 1;
        }
        r.write(stubs, e.b.data(), e.b.size());
        // The mission table's flags (as the orders module has them).
        for (int k = 0; k < sim::mission::kCount; ++k) r.w32(table + static_cast<u32>(k) * 0x20 + 0x11, sim::mission::default_flags()[static_cast<std::size_t>(k)]);
        r.w32(kGameStatePtr, gs);
        const std::uint8_t local = static_cast<std::uint8_t>(g() % 10);
        r.write(gs + kOffLocal, &local, 1);
        r.w32(gs + kOffUnits, units);
        // The player's range: ids lo..hi.
        const int lo = 1 + static_cast<int>(g() % 5), hi = lo + static_cast<int>(g() % (kUnits - 6));
        const u32 rec = gs + kOffPlayers + static_cast<u32>(local) * kPlayerSize;
        r.w32(rec + 0x67, units + static_cast<u32>(lo) * kUnitSize);
        r.w32(rec + 0x6B, units + static_cast<u32>(hi) * kUnitSize);
        std::vector<sim::Unit> mine(kUnits + 2);
        std::vector<std::uint8_t> abil(kUnits + 2), mis(kUnits + 2);
        for (int id = 0; id < kUnits + 2; ++id) {
            const u32 def = defs + static_cast<u32>(id) * 0x40 - 0x245 + 0x1000;  // def + 0x245 lands in our block
            r.w32(units + static_cast<u32>(id) * kUnitSize + 0x92, def);
            mine[static_cast<std::size_t>(id)].id = static_cast<std::uint16_t>(id);
        }
        int failures = 0;
        for (int call = 0; call < calls && !failures; ++call) {
            ++total;
            // Fresh selection, positions, abilities and resolution answers.
            const int spread_mode = static_cast<int>(g() % 3);
            for (int id = lo; id <= hi; ++id) {
                sim::Unit& u = mine[static_cast<std::size_t>(id)];
                u.flags = g() % 3 ? 0x10u : 0u;
                if (g() % 7 == 0) u.flags |= 0x20u;
                const i32 base = spread_mode == 0 ? 0x1000 : (spread_mode == 1 ? 40 : 4000);
                u.pos = {static_cast<i32>((1000 + g() % static_cast<u32>(base)) << 16) | static_cast<i32>(g() & 0xFFFF), static_cast<i32>(g()),
                         static_cast<i32>((1000 + g() % static_cast<u32>(base)) << 16) | static_cast<i32>(g() & 0xFFFF)};
                if (g() % 10 == 0) u.pos[0] = static_cast<i32>(g());  // far away / negative
                abil[static_cast<std::size_t>(id)] = static_cast<std::uint8_t>(g() % 4);
                const u32 pick = g() % 10;
                mis[static_cast<std::size_t>(id)] = static_cast<std::uint8_t>(
                    pick == 0 ? 0 : pick == 1 ? sim::mission::kStandingFireOrder : pick == 2 ? sim::mission::kStandingMoveOrder : g() % sim::mission::kCount);
                const u32 ua = units + static_cast<u32>(id) * kUnitSize;
                r.w32(ua + 0x110, u.flags);
                r.w32(ua + 0x6A, static_cast<u32>(u.pos[0]));
                r.w32(ua + 0x6E, static_cast<u32>(u.pos[1]));
                r.w32(ua + 0x72, static_cast<u32>(u.pos[2]));
                const std::uint8_t a = abil[static_cast<std::size_t>(id)];
                r.write(defs + static_cast<u32>(id) * 0x40 + 0x1000, &a, 1);
            }
            r.write(mtable, mis.data(), mis.size());
            const std::uint16_t hover = g() % 3 ? static_cast<std::uint16_t>(lo + static_cast<int>(g() % static_cast<u32>(hi - lo + 1))) : 0;
            r.write(gs + kOffHover, &hover, 2);
            const std::array<i32, 3> cursor = {static_cast<i32>(g()), static_cast<i32>(g()), static_cast<i32>(g())};
            r.write(gs + kOffCursor, cursor.data(), 12);
            const bool queued = g() % 2;
            r.w32(click + 8, queued ? 4u | (g() & ~4u) : g() & ~4u);
            const std::uint8_t intent = g() % 3 ? static_cast<std::uint8_t>(1 + g() % 14) : 0;
            const std::uint8_t mission = static_cast<std::uint8_t>(g() % sim::mission::kCount);
            const bool with_pos = g() % 4 != 0;
            const std::array<i32, 3> pos = {static_cast<i32>((1000 + g() % 3000) << 16), static_cast<i32>(g() % 0x1000000), static_cast<i32>((1000 + g() % 3000) << 16)};
            r.write(posbuf, pos.data(), 12);
            const i32 p0 = static_cast<i32>(g()), p1 = static_cast<i32>(g());
            r.w32(rlog, rlog + 4);
            r.w32(ilog, ilog + 4);
            if (!r.call(kRoutine, 0, {click, intent, static_cast<u32>(mission) | (g() & 0xFFFFFF00u), with_pos ? posbuf : 0u, static_cast<u32>(p0), static_cast<u32>(p1)})) {
                std::printf("scenario %u: the original's routine failed\n", seed);
                ++mismatches;
                ++failures;
                break;
            }
            // Ours.
            struct Sys final : sim::command::SelectedOrderSystems {
                std::vector<std::uint8_t>& mis;
                std::vector<std::uint8_t>& abil;
                std::vector<Call> issued;
                std::vector<std::array<u32, 3>> resolved;  // unit id, intent, target id
                Sys(std::vector<std::uint8_t>& m, std::vector<std::uint8_t>& a) : mis(m), abil(a) {}
                std::uint8_t resolve(std::uint8_t in, sim::Unit& u, sim::Unit* t, const std::array<i32, 3>&) override {
                    resolved.push_back({u.id, in, t ? t->id : 0u});
                    return mis[u.id];
                }
                std::uint32_t abilities(const sim::Unit& u) override { return abil[u.id]; }
                void issue(std::uint8_t m, bool q, sim::Unit& u, sim::Unit* t, const std::array<i32, 3>* p, i32 a, i32 b) override {
                    Call c{m, q ? 1u : 0u, u.id, t ? t->id : 0u, p ? 1u : 0u, {}, a, b};
                    if (p) c.pos[0] = (*p)[0], c.pos[1] = (*p)[1], c.pos[2] = (*p)[2];
                    issued.push_back(c);
                }
            } sys(mis, abil);
            sim::Unit* hp = hover ? &mine[hover] : nullptr;
            sim::command::issue_to_selected(sys, &mine[static_cast<std::size_t>(lo)], &mine[static_cast<std::size_t>(hi)], intent, mission, queued, hp,
                                            with_pos ? &pos : nullptr, cursor, p0, p1);
            // Compare.
            auto id_of = [&](u32 addr) -> u32 { return addr ? (addr - units) / kUnitSize : 0u; };
            const u32 nres = (r.r32(rlog) - (rlog + 4)) / 16, niss = (r.r32(ilog) - (ilog + 4)) / 0x28;
            bool ok = nres == sys.resolved.size() && niss == sys.issued.size();
            for (u32 k = 0; ok && k < nres; ++k) {
                u32 w[4];
                r.read(rlog + 4 + k * 16, w, 16);
                ok = (w[0] & 0xFF) == sys.resolved[k][1] && id_of(w[1]) == sys.resolved[k][0] && id_of(w[2]) == sys.resolved[k][2] &&
                     w[3] == gs + kOffCursor;
            }
            for (u32 k = 0; ok && k < niss; ++k) {
                u32 w[10];
                r.read(ilog + 4 + k * 0x28, w, 0x28);
                const Call& c = sys.issued[k];
                ok = (w[0] & 0xFF) == c.mission && w[1] == c.queued && id_of(w[2]) == c.unit && id_of(w[3]) == c.target && (w[4] != 0) == (c.pos_ptr != 0) &&
                     static_cast<i32>(w[8]) == c.p0 && static_cast<i32>(w[9]) == c.p1;
                if (ok && c.pos_ptr) {
                    ok = static_cast<i32>(w[5]) == c.pos[0] && static_cast<i32>(w[6]) == c.pos[1] && static_cast<i32>(w[7]) == c.pos[2];
                    if (w[4] != posbuf) ++spread;
                }
            }
            resolutions += nres;
            orders += niss;
            for (int id = lo; id <= hi; ++id) gated += (mine[static_cast<std::size_t>(id)].flags & 0x10u) != 0;
            if (!ok) {
                std::printf("scenario %u call %d: differs (resolutions orig %u ours %zu, orders orig %u ours %zu)\n", seed, call, nres, sys.resolved.size(), niss,
                            sys.issued.size());
                ++mismatches;
                ++failures;
            }
        }
    }
    std::printf("calls %zu, intent resolutions %zu, orders %zu (%zu with a spread position), selected units seen %zu\n", total, resolutions, orders, spread,
                gated);
    std::printf("%zu mismatches\n", mismatches);
    return mismatches ? 1 : 0;
}
