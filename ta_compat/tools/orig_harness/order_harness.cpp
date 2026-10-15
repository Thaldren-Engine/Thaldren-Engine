// Placed units' first-order strings against the original (0x487BF0): random order strings
// (well-formed and malformed tokens of every letter) on a synthetic list of placed units.
// The original runs with its own C runtime conversions, unit-name and type lookups; its intent
// resolution, order call and transport attach are logging stubs. Compared per string: every
// order (kind, target, position, parameters), every transport attach and the unit's flags.
//
// Usage: order_harness <TotalA.exe> [scenarios] [strings per scenario] [first seed]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include "remote.h"
#include "sim/mission_goals.h"
#include "sim/orders.h"
#include "sim/unit.h"
#include "ta/campaign.h"

namespace {

using harness::u32;
using i32 = std::int32_t;

constexpr u32 kGameStatePtr = 0x00511DE8, kParse = 0x00487BF0, kResolve = 0x0043F0E0, kEnqueue = 0x0043ADC0, kFindName = 0x00438760,
              kAttach = 0x0048AAC0;
constexpr u32 gDefs = 0x1439B, gDefCount = 0x1438F, gMapInfo = 0x391E9;
constexpr u32 kUnitSize = 0x118, kDefSize = 0x249, kRecord = 48;

const char* const kTypes[] = {"AR_1", "ARMAA", "ARMBB", "CORCC", "CORDD", "EEEE"};  // sorted (case-insensitive)
constexpr int kTypeCount = 6, kUnits = 6;

// Mission kinds the original looks up by name: the address of the name it passes.
u32 name_address(std::uint8_t kind) {
    switch (kind) {
    case sim::mission::kAttackUnitType: return 0x508C9C;
    case sim::mission::kBuildingBuild: return 0x502684;
    case sim::mission::kBuildWeapon: return 0x502678;
    case sim::mission::kMobileBuild: return 0x502654;
    case sim::mission::kSelfDestructFg: return 0x508C3C;
    case sim::mission::kMakeSelectable: return 0x508CB4;
    case sim::mission::kWait: return 0x508C6C;
    case sim::mission::kWaitForAttack: return 0x508C4C;
    default: return 0;
    }
}

struct Emitter {
    std::vector<std::uint8_t> b;
    void bytes(std::initializer_list<int> v) {
        for (int x : v) b.push_back(static_cast<std::uint8_t>(x));
    }
    void u32le(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void align() {
        while (b.size() % 16) b.push_back(0xCC);
    }
};

struct Rec {
    u32 tag = 0, kind = 0, queued = 0, unit = 0, target = 0, pos = 0, x = 0, y = 0, z = 0, p0 = 0, p1 = 0;
    bool operator==(const Rec& o) const {
        if (tag == 2 && o.tag == 2) return unit == o.unit && target == o.target && p0 == o.p0 && p1 == o.p1;
        return tag == o.tag && kind == o.kind && queued == o.queued && unit == o.unit && target == o.target && (pos != 0) == (o.pos != 0) &&
               (!pos || (x == o.x && y == o.y && z == o.z)) && p0 == o.p0 && p1 == o.p1;
    }
};

struct LogSink final : sim::MissionOrderSink {
    std::vector<Rec> log;
    bool resolved = false;
    std::function<u32(sim::Unit*)> addr;
    std::uint8_t resolve(std::uint8_t intent, sim::Unit&, sim::Unit*, const std::array<i32, 3>*) override {
        resolved = true;
        return static_cast<std::uint8_t>(intent + 0x40);
    }
    void order(sim::Unit& u, std::uint8_t kind, sim::Unit* target, const std::array<i32, 3>* pos, i32 p0, i32 p1) override {
        Rec r;
        r.tag = 1;
        r.kind = resolved ? kind : name_address(kind);
        resolved = false;
        r.queued = 1;
        r.unit = addr(&u);
        r.target = target ? addr(target) : 0;
        r.pos = pos ? 1 : 0;
        if (pos) r.x = static_cast<u32>((*pos)[0]), r.y = static_cast<u32>((*pos)[1]), r.z = static_cast<u32>((*pos)[2]);
        r.p0 = static_cast<u32>(p0);
        r.p1 = static_cast<u32>(p1);
        log.push_back(r);
    }
    void load_into(sim::Unit& u, sim::Unit& carrier) override {
        Rec r;
        r.tag = 2;
        r.unit = addr(&u);
        r.target = addr(&carrier);
        r.p0 = 0xFFFFFFFFu;
        r.p1 = 0;
        log.push_back(r);
    }
};

std::string fnum(std::mt19937& g) {
    char buf[32];
    switch (g() % 4) {
    case 0: std::snprintf(buf, sizeof buf, "%d", static_cast<int>(g() % 4000)); break;
    case 1: std::snprintf(buf, sizeof buf, "%d.%d", static_cast<int>(g() % 2000), static_cast<int>(g() % 100)); break;
    case 2: std::snprintf(buf, sizeof buf, "-%d.5", static_cast<int>(g() % 50)); break;
    default: std::snprintf(buf, sizeof buf, "%de1", static_cast<int>(g() % 300)); break;
    }
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: order_harness <TotalA.exe> [scenarios] [strings] [first seed]\n");
        return 2;
    }
    const int scenarios = argc > 2 ? std::atoi(argv[2]) : 10;
    const int strings = argc > 3 ? std::atoi(argv[3]) : 50;
    const u32 first = argc > 4 ? static_cast<u32>(std::strtoul(argv[4], nullptr, 0)) : 1u;
    std::size_t total = 0, orders = 0, attaches = 0, mismatches = 0;
    for (int sc = 0; sc < scenarios; ++sc) {
        const u32 seed = first + static_cast<u32>(sc);
        std::mt19937 g(seed);
        harness::Remote r;
        if (!r.start(argv[1])) {
            std::printf("cannot start %s\n", argv[1]);
            return 1;
        }
        const u32 gs = r.alloc(0x40000), stubs = r.alloc(0x1000, PAGE_EXECUTE_READWRITE), log = r.alloc(0x10000), defs = r.alloc(0x1000),
                  mi = r.alloc(0x2000), strs = r.alloc(0x1000), units = r.alloc(kUnits * kUnitSize + 0x40), ls = r.alloc(0x100), text = r.alloc(0x1000);
        Emitter e;
        auto stub = [&](u32 target) {
            const u32 at = stubs + static_cast<u32>(e.b.size());
            r.patch_jump(target, at);
            return at;
        };
        // Resolve: out byte = intent + 0x40.
        stub(kResolve);
        e.bytes({0x8B, 0x44, 0x24, 0x04, 0x8B, 0x4C, 0x24, 0x08, 0x83, 0xC1, 0x40, 0x88, 0x08, 0xC2, 0x14, 0x00});
        e.align();
        // Name lookup: the slot gets the name's address.
        stub(kFindName);
        e.bytes({0x8B, 0x44, 0x24, 0x04, 0x89, 0x01, 0x8B, 0xC1, 0xC2, 0x04, 0x00});
        e.align();
        auto record_head = [&](int tag) {
            e.bytes({0xA1}); e.u32le(log);                    // mov eax,[count]
            e.bytes({0xFF, 0x05}); e.u32le(log);              // inc dword [count]
            e.bytes({0x6B, 0xC0, static_cast<int>(kRecord)});  // imul eax,eax,48
            e.bytes({0x05}); e.u32le(log + 4);                // add eax,log+4
            e.bytes({0xC7, 0x00}); e.u32le(static_cast<u32>(tag));
        };
        auto copy_arg = [&](int arg_off, int rec_off) {
            e.bytes({0x8B, 0x4C, 0x24, arg_off, 0x89, 0x48, rec_off});
        };
        // Order: {1, kind, queued, unit, target, pos, x, y, z, p0, p1}.
        stub(kEnqueue);
        record_head(1);
        copy_arg(0x04, 0x04);
        copy_arg(0x08, 0x08);
        copy_arg(0x0C, 0x0C);
        copy_arg(0x10, 0x10);
        copy_arg(0x14, 0x14);
        e.bytes({0x85, 0xC9, 0x74, 0x11, 0x8B, 0x11, 0x89, 0x50, 0x18, 0x8B, 0x51, 0x04, 0x89, 0x50, 0x1C, 0x8B, 0x51, 0x08, 0x89, 0x50, 0x20});
        copy_arg(0x18, 0x24);
        copy_arg(0x1C, 0x28);
        e.bytes({0xC2, 0x1C, 0x00});
        e.align();
        // Transport attach: {2, -, -, cargo, transport, -, -, -, -, piece, layer}.
        stub(kAttach);
        record_head(2);
        copy_arg(0x04, 0x0C);
        copy_arg(0x08, 0x10);
        copy_arg(0x0C, 0x24);
        copy_arg(0x10, 0x28);
        e.bytes({0xC2, 0x10, 0x00});
        e.align();
        r.write(stubs, e.b.data(), e.b.size());
        // The C runtime's start-up parts the parser's conversions need (heap, thread data, I/O
        // handles, code page), as the program's entry runs them.
        // The runtime's fatal-error paths (they show a message box) fault instead.
        for (u32 fatal : {0x004E7180u, 0x004F18B0u, 0x004F3D00u}) {
            const std::uint8_t ud2[2] = {0x0F, 0x0B};
            if (!r.patch_jump(fatal, fatal) || !r.write(fatal, ud2, 2)) return 1;
        }
        // Floating-point support (_fpmath, run by the start-up's initialiser tables) included.
        for (u32 init : {0x004F1830u, 0x004EB040u, 0x004F15C0u, 0x004F0C50u, 0x004E4320u})
            if (!r.call(init, 0, {})) {
                std::printf("C runtime start-up %08x failed\n", init);
                return 1;
            }
        r.w32(kGameStatePtr, gs);
        r.w32(gs + gDefs, defs);
        r.w32(gs + gDefCount, kTypeCount + 1);
        for (int k = 1; k <= kTypeCount; ++k) {
            r.write(defs + static_cast<u32>(k) * kDefSize + 0x20, kTypes[k - 1], std::strlen(kTypes[k - 1]) + 1);
            const std::uint16_t index = static_cast<std::uint16_t>(k);
            r.write(defs + static_cast<u32>(k) * kDefSize + 0x21E, &index, 2);
        }
        // Placed units: names and identifiers, some not placed.
        std::vector<sim::Unit> eu(kUnits);
        std::vector<std::string> unames(kUnits), idents(kUnits);
        std::vector<bool> placed(kUnits), mobile(kUnits), has_ident(kUnits);
        u32 sp = strs;
        auto put_str = [&](const std::string& s) {
            const u32 at = sp;
            r.write(at, s.c_str(), s.size() + 1);
            sp += static_cast<u32>(s.size()) + 1;
            return at;
        };
        r.w32(gs + gMapInfo, mi);
        const u32 entries = mi + 0x1000;
        r.w32(mi + 0xDAC, entries);
        r.w32(mi + 0xDB0, kUnits);
        r.w32(ls + 4, ls + 0x20);
        for (int i = 0; i < kUnits; ++i) {
            unames[static_cast<std::size_t>(i)] = kTypes[g() % kTypeCount];
            has_ident[static_cast<std::size_t>(i)] = g() % 2 != 0;
            idents[static_cast<std::size_t>(i)] = has_ident[static_cast<std::size_t>(i)] ? (g() % 3 ? "Hero" + std::to_string(g() % 3) : std::string()) : std::string();
            placed[static_cast<std::size_t>(i)] = g() % 5 != 0;
            mobile[static_cast<std::size_t>(i)] = g() % 3 != 0;
            r.w32(entries + static_cast<u32>(i) * 0x24, put_str(unames[static_cast<std::size_t>(i)]));
            r.w32(entries + static_cast<u32>(i) * 0x24 + 4, has_ident[static_cast<std::size_t>(i)] ? put_str(idents[static_cast<std::size_t>(i)]) : 0u);
            r.w32(ls + 0x20 + static_cast<u32>(i) * 4, placed[static_cast<std::size_t>(i)] ? units + static_cast<u32>(i) * kUnitSize : 0u);
            r.w32(units + static_cast<u32>(i) * kUnitSize, mobile[static_cast<std::size_t>(i)] ? 0x1234u : 0u);
            eu[static_cast<std::size_t>(i)].id = static_cast<std::uint16_t>(i + 1);
        }
        auto addr = [&](sim::Unit* u) { return units + static_cast<u32>(u - eu.data()) * kUnitSize; };
        auto same = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
            return true;
        };
        auto find = [&](const std::string& n) -> sim::Unit* {
            for (int i = 0; i < kUnits; ++i) {
                const std::size_t k = static_cast<std::size_t>(i);
                if (!placed[k]) continue;
                if ((has_ident[k] && same(idents[k], n)) || same(unames[k], n)) return &eu[k];
            }
            return nullptr;
        };
        auto type_index = [&](const std::string& n) -> std::uint16_t {
            for (int k = 0; k < kTypeCount; ++k)
                if (same(kTypes[k], n)) return static_cast<std::uint16_t>(k + 1);
            return 0;
        };
        auto any_name = [&]() -> std::string {
            switch (g() % 5) {
            case 0: return kTypes[g() % kTypeCount];
            case 1: return "Hero" + std::to_string(g() % 4);
            case 2: return unames[g() % kUnits];
            case 3: return "zz.q_" + std::to_string(g() % 9);
            default: return std::string();
            }
        };
        for (int sidx = 0; sidx < strings; ++sidx) {
            ++total;
            const int who = static_cast<int>(g() % kUnits);
            sim::Unit& u = eu[static_cast<std::size_t>(who)];
            u.flags = (g() & 0x3C0020u) | 0x10000000u;
            r.w32(units + static_cast<u32>(who) * kUnitSize + 0x110, u.flags);
            // The string: a well-formed move first (the position scratch starts known), then tokens.
            std::string s = "M " + fnum(g) + " " + fnum(g);
            const int n = 1 + static_cast<int>(g() % 6);
            for (int k = 0; k < n; ++k) {
                s += g() % 4 ? "," : ", ";
                std::string t;
                switch (g() % 16) {
                case 0: t = "A " + fnum(g) + " " + fnum(g); break;
                case 1: t = "a " + any_name(); break;
                case 2: t = "B " + any_name() + " " + std::to_string(g() % 4) + " " + fnum(g) + " " + fnum(g); break;
                case 3: t = g() % 2 ? "BW " + std::to_string(g() % 5) : "bw"; break;
                case 4: t = "D"; break;
                case 5: t = "G " + any_name(); break;
                case 6: t = "i" + any_name(); break;
                case 7: t = "m " + fnum(g) + (g() % 3 ? " " + fnum(g) : std::string()); break;
                case 8: t = "O " + std::to_string(g() % 4) + (g() % 2 ? " " + std::to_string(g() % 4) : std::string()); break;
                case 9: t = "P " + fnum(g) + " " + fnum(g) + (g() % 2 ? " " + fnum(g) : std::string()); break;
                case 10: t = "S"; break;
                case 11: t = "U " + fnum(g) + " " + fnum(g); break;
                case 12: t = "WA " + any_name(); break;
                case 13: t = "W " + fnum(g) + (g() % 2 ? " " + std::to_string(g() % 9) : std::string()); break;
                case 14: t = "X junk"; break;
                default: t = ""; break;
                }
                s += t;
            }
            r.write(text, s.c_str(), s.size() + 1);
            r.w32(log, 0);
            if (!r.call(kParse, 0, {units + static_cast<u32>(who) * kUnitSize, text, ls})) {
                std::printf("scenario %u: the original's parse failed on \"%s\"\n", seed, s.c_str());
                ++mismatches;
                break;
            }
            ta::MissionParseState st;
            const std::vector<sim::MissionUnitOrder> parsed = ta::parse_initial_orders(s, type_index, st);
            LogSink sink;
            sink.addr = addr;
            sim::give_mission_orders(sink, u, mobile[static_cast<std::size_t>(who)], parsed, find);
            const u32 count = r.r32(log);
            std::vector<Rec> orig(count);
            for (u32 k = 0; k < count; ++k) {
                u32 w[11];
                r.read(log + 4 + k * kRecord, w, sizeof w);
                Rec x{w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10]};
                if (x.tag == 1 && x.kind >= 0x400000u) {
                } else if (x.tag == 1) {
                    x.kind &= 0xFF;  // a resolved kind (the stub writes one byte)
                }
                orig[k] = x;
            }
            bool ok = orig.size() == sink.log.size();
            for (std::size_t k = 0; ok && k < orig.size(); ++k) ok = orig[k] == sink.log[k];
            ok = ok && r.r32(units + static_cast<u32>(who) * kUnitSize + 0x110) == u.flags;
            for (const Rec& x : orig) (x.tag == 1 ? orders : attaches)++;
            if (!ok) {
                std::printf("scenario %u: \"%s\": original %zu records, engine %zu; flags %08x/%08x\n", seed, s.c_str(), orig.size(), sink.log.size(),
                            r.r32(units + static_cast<u32>(who) * kUnitSize + 0x110), u.flags);
                for (const Rec& x : orig) std::printf("  orig  %u kind %x tgt %x pos %u %d,%d,%d p %d %d\n", x.tag, x.kind, x.target, x.pos, x.x, x.y, x.z, x.p0, x.p1);
                for (const Rec& x : sink.log) std::printf("  engine %u kind %x tgt %x pos %u %d,%d,%d p %d %d\n", x.tag, x.kind, x.target, x.pos, x.x, x.y, x.z, x.p0, x.p1);
                ++mismatches;
                break;
            }
        }
    }
    std::printf("scenarios %d, strings %zu: orders %zu, transport attaches %zu\n", scenarios, total, orders, attaches);
    std::printf("%zu mismatches\n", mismatches);
    return mismatches ? 1 : 0;
}
