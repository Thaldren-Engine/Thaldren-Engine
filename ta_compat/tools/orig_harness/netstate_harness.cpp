// Unit state packets against the original: the whole-state writer (0x48B200) and its apply
// (0x48B3F0), and the motion goal states inside the per-unit records: point goal write/read
// (0x44DDC0 / 0x44E080), manoeuvre goal write/read (0x44E930 / 0x44E9C0), route goal
// write (0x44F4A0) and the remote route read (0x44F5C0). Everything runs in the suspended
// exe; the routines the apply reaches (packet 9, activation, transport, footprint release
// and claim, sight) are logging stubs, and the engine's sim::apply_whole_state drives a
// mirror of the unit whose calls are logged the same way.
//
// Usage: netstate_harness <TotalA.exe> [scenarios] [operations] [first seed]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "remote.h"
#include "sim/bitstream.h"
#include "sim/motion.h"
#include "sim/net_units.h"

namespace {

using harness::u32;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using i16 = std::int16_t;
using i32 = std::int32_t;

constexpr u32 kGameStatePtr = 0x00511DE8;
constexpr u32 kWholeWrite = 0x0048B200, kWholeApply = 0x0048B3F0;
constexpr u32 kPointWrite = 0x0044DDC0, kPointRead = 0x0044E080, kManWrite = 0x0044E930, kManRead = 0x0044E9C0;
constexpr u32 kRouteWrite = 0x0044F4A0, kRouteRead = 0x0044F5C0, kBitInit = 0x00415B60;
constexpr u32 kMake = 0x004861D0, kActivate = 0x0048B090, kTransport = 0x0048AB70, kRelease = 0x0047D0E0, kClaim = 0x0047CC30,
              kSight = 0x004827B0, kLinkInit = 0x004895C0, kLinkSet = 0x00489690;
constexpr u32 kUnitSize = 0x118;

// One logged call: tag, ecx, two stack words, 24 bytes behind a pointer argument.
struct Call {
    u32 tag = 0, ecx = 0, a1 = 0, a2 = 0;
    u8 bytes[32]{};
};

struct Emitter {
    std::vector<u8> b;
    void raw(std::initializer_list<int> v) {
        for (int x : v) b.push_back(static_cast<u8>(x));
    }
    void u32le(u32 v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void align() {
        while (b.size() % 16) b.push_back(0xCC);
    }
};

// A logging stub: record {tag, ecx, [esp+4], [esp+8], 32 bytes at stack argument `copy`
// (1-based, 0 none)}, then `ret pop`.
u32 emit_logger(Emitter& e, u32 base, u32 log_ptr, u32 tag, int copy, int pop) {
    const u32 at = base + static_cast<u32>(e.b.size());
    e.raw({0xA1}); e.u32le(log_ptr);                       // mov eax,[log_ptr]
    e.raw({0xC7, 0x00}); e.u32le(tag);                     // mov [eax],tag
    e.raw({0x89, 0x48, 0x04});                             // mov [eax+4],ecx
    e.raw({0x8B, 0x54, 0x24, 0x04, 0x89, 0x50, 0x08});     // mov edx,[esp+4]; mov [eax+8],edx
    e.raw({0x8B, 0x54, 0x24, 0x08, 0x89, 0x50, 0x0C});     // mov edx,[esp+8]; mov [eax+12],edx
    if (copy > 0) {
        e.raw({0x56, 0x57});                                               // push esi; push edi
        e.raw({0x8B, 0x74, 0x24, static_cast<int>(8 + 4 * copy)});         // mov esi,[esp+8+4k]
        e.raw({0x8D, 0x78, 0x10, 0xB9, 0x08, 0x00, 0x00, 0x00, 0xF3, 0xA5});  // lea edi,[eax+16]; mov ecx,8; rep movsd
        e.raw({0x5F, 0x5E});                                               // pop edi; pop esi
    }
    e.raw({0x83, 0x05}); e.u32le(log_ptr); e.raw({0x30});  // add dword [log_ptr],48
    if (pop) e.raw({0xC2, pop, 0x00});
    else e.raw({0xC3});
    e.align();
    return at;
}

// The unit fields the whole-state routines touch (offsets of the original's unit).
struct UnitImage {
    u8 b[kUnitSize]{};
    template <class T> T get(u32 off) const {
        T v;
        std::memcpy(&v, b + off, sizeof v);
        return v;
    }
    template <class T> void set(u32 off, T v) { std::memcpy(b + off, &v, sizeof v); }
};

// The engine side: a mirror of the unit driven through sim::WholeStateTarget.
struct Mirror final : sim::WholeStateTarget {
    UnitImage u;
    u32 unit_addr = 0, motion_speed = 0;
    bool motion = false, carried_ = false;
    std::vector<Call> log;
    void add(u32 tag, u32 ecx, u32 a1, u32 a2, const u8* bytes = nullptr, std::size_t n = 0) {
        Call c{tag, ecx, a1, a2, {}};
        if (bytes) std::memcpy(c.bytes, bytes, n);
        log.push_back(c);
    }
    u16 type() override { return u.get<u16>(0xA6); }
    void make(u16 type) override {
        u8 pk[32]{9};
        std::memcpy(pk + 1, &type, 2);
        std::memcpy(pk + 3, u.b + 0xA8, 2);
        std::memcpy(pk + 5, u.b + 0x6A, 12);
        add(1, 0, u.b[0xFF], 0, pk, 17);
    }
    void add_flags(u32 bits) override { u.set<u32>(0x110, u.get<u32>(0x110) | bits); }
    void set_hit_points(i16 hp) override { u.set<i16>(0x108, hp); }
    float fraction_left() override { return u.get<float>(0x104); }
    void set_fraction_left(float f) override { u.set<float>(0x104, f); }
    void activate(u8 mask, bool on) override {
        add(2, unit_addr, mask, on ? 1u : 0u);
        // The original's routine (stubbed on both sides) would change +0x10E.
    }
    bool carried() override { return carried_; }
    void transport(u16 carrier, u8 piece, u8 layer) override {
        u8 pk[8]{};
        std::memcpy(pk + 1, u.b + 0xA8, 2);
        std::memcpy(pk + 3, &carrier, 2);
        pk[5] = piece;
        pk[6] = layer;
        add(3, 0, 0, 0, pk, 7);
    }
    bool same_cells(i32 x, i32 z, u32 layer) override {
        return sim::footprint_cell(x, u.get<i16>(0x7E)) == u.get<i16>(0x76) && sim::footprint_cell(z, u.get<i16>(0x80)) == u.get<i16>(0x78) &&
               layer == (u.get<u32>(0x110) & 3u);
    }
    void set_position(const std::array<i32, 3>& pos) override { std::memcpy(u.b + 0x6A, pos.data(), 12); }
    void move_cells(const std::array<i32, 3>& pos, u32 layer) override {
        add(4, 0, unit_addr, 0);
        std::memcpy(u.b + 0x6A, pos.data(), 12);
        u.set<i16>(0x76, sim::footprint_cell(pos[0], u.get<i16>(0x7E)));
        u.set<i16>(0x78, sim::footprint_cell(pos[2], u.get<i16>(0x80)));
        u.set<u32>(0x110, (u.get<u32>(0x110) & ~3u) | (layer & 3u));
        add(5, 0, unit_addr, 0);
        add(6, 0, unit_addr, 0);
    }
    void set_angles(u16 pitch, u16 heading, u16 bank) override {
        u.set<u16>(0x64, pitch);
        u.set<u16>(0x66, heading);
        u.set<u16>(0x68, bank);
    }
    bool has_motion() override { return motion; }
    void set_speed(i32 speed) override { motion_speed = static_cast<u32>(speed); }
};

float random_fraction(std::mt19937& g) {
    switch (g() % 6) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return static_cast<float>(g() % 255) / 255.0f;
    case 3: return static_cast<float>(g() % 254) / 254.0f;
    case 4: return static_cast<float>(g() % 1000000) / 1000000.0f;
    default: {
        float f;
        const u32 bits = 0x3F000000u + g() % 0x01000000u;  // [0.5, 2)
        std::memcpy(&f, &bits, 4);
        return f > 1.0f ? f - 1.0f : f;
    }
    }
}

}  // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc < 2) {
        std::fprintf(stderr, "usage: netstate_harness <TotalA.exe> [scenarios] [operations] [first seed]\n");
        return 2;
    }
    const int scenarios = argc > 2 ? std::atoi(argv[2]) : 3;
    const int ops = argc > 3 ? std::atoi(argv[3]) : 60;
    const u32 first = argc > 4 ? static_cast<u32>(std::strtoul(argv[4], nullptr, 0)) : 1u;
    std::size_t n_write = 0, n_apply = 0, n_moves = 0, n_made = 0, n_carried = 0, n_point = 0, n_man = 0, n_route = 0, mismatches = 0;
    for (int sc = 0; sc < scenarios; ++sc) {
        const u32 seed = first + static_cast<u32>(sc);
        std::mt19937 g(seed);
        harness::Remote r;
        if (!r.start(argv[1])) {
            std::printf("cannot start %s\n", argv[1]);
            return 1;
        }
        const u32 gs = r.alloc(0x40000), stubs = r.alloc(0x1000, PAGE_EXECUTE_READWRITE), units = r.alloc(kUnitSize * 64),
                  motion = r.alloc(0x100), objstate = r.alloc(0x100), writer = r.alloc(0x1000), reader = r.alloc(0x100),
                  data = r.alloc(0x1000), goal = r.alloc(0x200), owner = r.alloc(0x100), log = r.alloc(0x2000), log_ptr = r.alloc(0x10);
        r.w32(kGameStatePtr, gs);
        r.w32(gs + 0x14357, units);
        Emitter e;
        std::vector<std::pair<u32, u32>> jumps;
        jumps.push_back({kMake, emit_logger(e, stubs, log_ptr, 1, 2, 8)});
        jumps.push_back({kActivate, emit_logger(e, stubs, log_ptr, 2, 0, 8)});
        jumps.push_back({kTransport, emit_logger(e, stubs, log_ptr, 3, 1, 4)});
        jumps.push_back({kRelease, emit_logger(e, stubs, log_ptr, 4, 0, 4)});
        jumps.push_back({kClaim, emit_logger(e, stubs, log_ptr, 5, 0, 4)});
        jumps.push_back({kSight, emit_logger(e, stubs, log_ptr, 6, 0, 4)});
        jumps.push_back({kLinkInit, stubs + static_cast<u32>(e.b.size())});
        e.raw({0xC7, 0x41, 0x04, 0, 0, 0, 0, 0xC2, 0x08, 0x00});  // mov dword [ecx+4],0; ret 8
        e.align();
        jumps.push_back({kLinkSet, stubs + static_cast<u32>(e.b.size())});
        e.raw({0x8B, 0x44, 0x24, 0x04, 0x89, 0x41, 0x04, 0xC2, 0x04, 0x00});  // [ecx+4] = unit; ret 4
        e.align();
        r.write(stubs, e.b.data(), e.b.size());
        bool ok = true;
        for (auto& [target, at] : jumps) ok = ok && r.patch_jump(target, at);
        if (!ok) {
            std::printf("scenario %u: set-up failed\n", seed);
            return 1;
        }
        auto fail = [&](int op, const char* what) {
            std::printf("scenario %u op %d: %s\n", seed, op, what);
            ++mismatches;
        };
        auto original_bits = [&]() {
            const u32 index = r.r32(writer), bit = r.r32(writer + 4);
            std::vector<u8> ob(index * 4 + ((bit + 7) >> 3));
            r.read(r.r32(writer + 12), ob.data(), ob.size());
            return ob;
        };
        auto set_reader = [&](const std::vector<u8>& bytes) {
            std::vector<u8> padded(bytes);
            padded.resize(0x400, 0);
            r.write(data, padded.data(), padded.size());
            r.w32(reader, data);
            r.w32(reader + 4, 0);
            r.w32(reader + 8, 0);
        };
        for (int op = 0; op < ops; ++op) {
            const int what = static_cast<int>(g() % 5);
            if (what <= 1) {
                // Whole state: write with both, then apply a stream to a unit with both.
                const int type_bits = 1 + static_cast<int>(g() % 10);
                r.w32(gs + 0x14393, static_cast<u32>(type_bits));
                const u32 id = 1 + g() % 60, carrier_id = 1 + g() % 60;
                const u32 unit = units + id * kUnitSize, carrier = units + carrier_id * kUnitSize;
                UnitImage img;
                for (auto& c : img.b) c = static_cast<u8>(g());
                const bool has_motion = g() % 2 == 0, is_carried = g() % 4 == 0;
                img.set<u32>(0x00, has_motion ? motion : 0u);
                img.set<u32>(0x86, is_carried ? carrier : 0u);
                img.set<u32>(0x9E, objstate);
                img.set<u16>(0xA8, static_cast<u16>(id));
                img.set<u16>(0xA6, static_cast<u16>(g() % 3 == 0 ? 0 : g() & ((1u << type_bits) - 1)));
                img.set<float>(0x104, random_fraction(g));
                img.set<i16>(0x7E, static_cast<i16>(g() % 8));
                img.set<i16>(0x80, static_cast<i16>(g() % 8));
                r.write(unit, img.b, kUnitSize);
                r.w32(carrier + 0xA8, carrier_id);
                const u32 speed = g();
                r.w32(motion + 0x20, speed);
                if (!r.call(kBitInit, writer, {}) || !r.call(kWholeWrite, 0, {writer, unit})) {
                    fail(op, "the original's whole-state writer failed");
                    break;
                }
                const auto ob = original_bits();
                sim::NetWholeState s;
                s.type = img.get<u16>(0xA6);
                s.hit_points = img.get<i16>(0x108);
                s.fraction_left = img.get<float>(0x104);
                s.activate = img.b[0x10E];
                s.layer = img.get<u32>(0x110) & 3u;
                s.carried = is_carried;
                s.carrier_id = static_cast<u16>(carrier_id);
                s.piece = static_cast<std::int8_t>(img.b[0xF9]);
                std::memcpy(s.pos.data(), img.b + 0x6A, 12);
                s.heading = img.get<u16>(0x66);
                s.bank = img.get<u16>(0x68);
                s.pitch = img.get<u16>(0x64);
                s.has_motion = has_motion;
                s.speed = static_cast<i32>(speed);
                sim::BitWriter w;
                sim::write_whole_state(w, type_bits, s);
                ++n_write;
                if (w.bytes() != ob) {
                    fail(op, "whole-state streams differ");
                    break;
                }
                // Apply: a stream from a fresh random state (sometimes the unit's own cells).
                UnitImage now;
                for (auto& c : now.b) c = static_cast<u8>(g());
                const bool now_motion = g() % 2 == 0, now_carried = g() % 3 == 0;
                now.set<u32>(0x00, now_motion ? motion : 0u);
                now.set<u32>(0x86, now_carried ? carrier : 0u);
                now.set<u32>(0x9E, objstate);
                now.set<u16>(0xA8, static_cast<u16>(id));
                now.set<u16>(0xA6, static_cast<u16>(g() % 4 == 0 ? s.type : g() & ((1u << type_bits) - 1)));
                now.set<float>(0x104, g() % 2 ? random_fraction(g) : static_cast<float>(g() % 256) * 0.003921569f);
                now.set<i16>(0x7E, static_cast<i16>(g() % 8));
                now.set<i16>(0x80, static_cast<i16>(g() % 8));
                if (g() % 2) {
                    s.pos[0] = static_cast<i32>(g() % 0x10000000u);
                    s.pos[2] = static_cast<i32>(g() % 0x10000000u);
                    if (g() % 2) {
                        s.pos[0] = now.get<i16>(0x76) * 0x100000 + static_cast<i32>(now.get<i16>(0x7E)) * 0x80000 -
                                   0x80000 + static_cast<i32>(g() % 0x100000u);
                        s.pos[2] = now.get<i16>(0x78) * 0x100000 + static_cast<i32>(now.get<i16>(0x80)) * 0x80000 - 0x80000 +
                                   static_cast<i32>(g() % 0x100000u);
                        if (g() % 2) s.layer = now.get<u32>(0x110) & 3u;
                    }
                }
                s.has_motion = now_motion;
                s.carried = g() % 4 == 0;
                if (g() % 5 == 0) s.type = 0;
                sim::BitWriter w2;
                sim::write_whole_state(w2, type_bits, s);
                const auto stream = w2.bytes();
                r.write(unit, now.b, kUnitSize);
                r.w32(objstate + 0x10, 0xABCDEF01u);
                r.w32(motion + 0x20, 0x5A5A5A5Au);
                set_reader(stream);
                r.w32(log_ptr, log);
                if (!r.call(kWholeApply, 0, {reader, unit})) {
                    fail(op, "the original's whole-state apply failed");
                    break;
                }
                const u32 n_calls = (r.r32(log_ptr) - log) / 48;
                std::vector<Call> oc(n_calls);
                if (n_calls) r.read(log, oc.data(), n_calls * sizeof(Call));
                UnitImage after;
                r.read(unit, after.b, kUnitSize);
                Mirror m;
                m.u = now;
                m.unit_addr = unit;
                m.motion = now_motion;
                m.carried_ = now_carried;
                m.motion_speed = 0x5A5A5A5Au;
                sim::BitReader br(stream.data(), stream.size());
                sim::apply_whole_state(br, type_bits, m);
                ++n_apply;
                // Compare the calls (the stubs leave the unit alone).
                bool same = oc.size() == m.log.size();
                for (std::size_t k = 0; same && k < oc.size(); ++k) {
                    const Call &a = oc[k], &b = m.log[k];
                    same = a.tag == b.tag;
                    if (!same) break;
                    if (a.tag == 1) {
                        ++n_made;
                        same = (a.a1 & 0xFF) == b.a1 && std::memcmp(a.bytes, b.bytes, 17) == 0;
                    } else if (a.tag == 2) {
                        same = a.ecx == b.ecx && (a.a1 & 0xFF) == b.a1 && a.a2 == b.a2;
                    } else if (a.tag == 3) {
                        ++n_carried;
                        same = std::memcmp(a.bytes + 1, b.bytes + 1, 6) == 0;  // byte 0 is a stale stack byte, never read
                    } else {
                        n_moves += a.tag == 4;
                        same = a.a1 == b.a1;
                    }
                }
                if (!same) {
                    std::printf("  calls: original");
                    for (auto& c : oc) std::printf(" %u(%x,%x)", c.tag, c.a1, c.a2);
                    std::printf(" / engine");
                    for (auto& c : m.log) std::printf(" %u(%x,%x)", c.tag, c.a1, c.a2);
                    std::printf("\n");
                    fail(op, "whole-state apply calls differ");
                    break;
                }
                // Compare the unit fields and the motion speed.
                bool fields = true;
                for (u32 off = 0; off < kUnitSize && fields; ++off) {
                    if (after.b[off] != m.u.b[off]) {
                        std::printf("  unit +0x%X: original %02x engine %02x (stream type %u)\n", off, after.b[off], m.u.b[off], s.type);
                        fields = false;
                    }
                }
                if (!fields || r.r32(motion + 0x20) != m.motion_speed) {
                    fail(op, "unit after whole-state apply differs");
                    break;
                }
                // The original drops the object state's draw frame (+0x10) for a live unit;
                // that pointer is render-only (the engine has no draw-frame cache in sim).
                const bool dropped = r.r32(objstate + 0x10) == 0;
                if (dropped != (s.type != 0)) {
                    fail(op, "object state draw-frame drop differs from expectation");
                    break;
                }
            } else if (what == 2) {
                // Point goal state.
                sim::NetPointGoal pg;
                pg.flags = static_cast<u16>(g() & 0xFFFF);
                pg.piece = static_cast<i16>(g());
                pg.radius = static_cast<i16>(g());
                pg.altitude = static_cast<i16>(g());
                pg.heading = static_cast<i16>(g());
                pg.link_id = static_cast<u16>(g() % 3 == 0 ? 0 : 1 + g() % 60);
                for (auto& v : pg.pos) v = static_cast<i32>(g());
                std::vector<u8> img(0x40);
                for (auto& c : img) c = static_cast<u8>(g());
                std::memcpy(img.data() + 8, &pg.flags, 2);
                std::memcpy(img.data() + 0x10, &pg.piece, 2);
                std::memcpy(img.data() + 0x0A, &pg.radius, 2);
                std::memcpy(img.data() + 0x0C, &pg.altitude, 2);
                std::memcpy(img.data() + 0x0E, &pg.heading, 2);
                const u32 link = pg.link_id ? units + pg.link_id * kUnitSize : 0u;
                std::memcpy(img.data() + 0x1A, &link, 4);
                std::memcpy(img.data() + 0x26, pg.pos.data(), 12);
                if (link) r.w32(link + 0xA8, pg.link_id);
                r.write(goal, img.data(), img.size());
                if (!r.call(kBitInit, writer, {}) || !r.call(kPointWrite, goal, {writer})) {
                    fail(op, "the original's point goal writer failed");
                    break;
                }
                const auto ob = original_bits();
                sim::BitWriter w;
                sim::write_point_goal_state(w, pg);
                // Read back with the original constructor and with the engine.
                set_reader(ob);
                std::vector<u8> fresh(0x40);
                for (auto& c : fresh) c = static_cast<u8>(g());
                r.write(goal + 0x100, fresh.data(), fresh.size());
                ++n_point;
                if (w.bytes() != ob || !r.call(kPointRead, goal + 0x100, {owner, reader})) {
                    fail(op, "point goal streams differ");
                    break;
                }
                std::vector<u8> og(0x40);
                r.read(goal + 0x100, og.data(), og.size());
                sim::NetPointGoal back;
                std::memcpy(back.pos.data(), fresh.data() + 0x26, 12);
                sim::BitReader br(ob.data(), ob.size());
                sim::read_point_goal_state(br, back);
                u32 olink = 0;
                std::memcpy(&olink, og.data() + 0x1A, 4);
                const u16 olink_id = olink ? static_cast<u16>((olink - units) / kUnitSize) : 0;
                i16 f16[4];
                std::memcpy(f16, og.data() + 0x0A, 8);  // radius, altitude, heading, piece
                std::array<i32, 3> opos;
                std::memcpy(opos.data(), og.data() + 0x26, 12);
                u16 oflags;
                std::memcpy(&oflags, og.data() + 8, 2);
                // The piece is kept only with the follow flag (the engine reports 0 otherwise, the
                // original leaves the fresh field).
                const bool follow = (oflags & 1) != 0;
                if (oflags != back.flags || f16[0] != back.radius || f16[1] != back.altitude || f16[2] != back.heading ||
                    (follow && f16[3] != back.piece) || olink_id != back.link_id || opos != back.pos) {
                    fail(op, "point goal read differs");
                    break;
                }
            } else if (what == 3) {
                // Manoeuvre goal state.
                sim::NetManoeuvreGoal mg;
                mg.flags = static_cast<u16>(g() & 0xFFFF);
                for (auto& v : mg.pos) v = static_cast<i32>(g());
                for (auto& v : mg.vel) v = static_cast<i32>(g());
                mg.target_heading = static_cast<u16>(g());
                std::vector<u8> img(0x40);
                for (auto& c : img) c = static_cast<u8>(g());
                std::memcpy(img.data() + 8, &mg.flags, 2);
                std::memcpy(img.data() + 0x0A, mg.pos.data(), 12);
                std::memcpy(img.data() + 0x16, mg.vel.data(), 12);
                std::memcpy(img.data() + 0x24, &mg.target_heading, 2);
                r.write(goal, img.data(), img.size());
                if (!r.call(kBitInit, writer, {}) || !r.call(kManWrite, goal, {writer})) {
                    fail(op, "the original's manoeuvre goal writer failed");
                    break;
                }
                const auto ob = original_bits();
                sim::BitWriter w;
                sim::write_manoeuvre_goal_state(w, mg);
                set_reader(ob);
                ++n_man;
                if (w.bytes() != ob || !r.call(kManRead, goal + 0x100, {owner, reader})) {
                    fail(op, "manoeuvre goal streams differ");
                    break;
                }
                std::vector<u8> og(0x40);
                r.read(goal + 0x100, og.data(), og.size());
                sim::NetManoeuvreGoal back;
                back.target_heading = 0;
                sim::BitReader br(ob.data(), ob.size());
                sim::read_manoeuvre_goal_state(br, back);
                u16 oflags, oth;
                std::array<i32, 3> op_, ov;
                std::memcpy(&oflags, og.data() + 8, 2);
                std::memcpy(op_.data(), og.data() + 0x0A, 12);
                std::memcpy(ov.data(), og.data() + 0x16, 12);
                std::memcpy(&oth, og.data() + 0x24, 2);
                if (oflags != back.flags || op_ != back.pos || ov != back.vel || ((oflags & 1) && oth != back.target_heading)) {
                    fail(op, "manoeuvre goal read differs");
                    break;
                }
            } else {
                // Route goal: write (with the flag update) and the remote read.
                std::vector<u8> img(0x80);
                for (auto& c : img) c = static_cast<u8>(g());
                const u32 motion_ptr_cell = goal + 0x180;
                const i32 count = static_cast<i32>(g() % 6) - 1;
                std::memcpy(img.data() + 0x5C, &count, 4);
                std::memcpy(img.data() + 8, &motion_ptr_cell, 4);
                r.write(goal, img.data(), img.size());
                r.w32(motion_ptr_cell, motion);
                const u8 mflags = static_cast<u8>(g());
                r.write(motion + 0x2E, &mflags, 1);
                if (!r.call(kBitInit, writer, {}) || !r.call(kRouteWrite, goal, {writer})) {
                    fail(op, "the original's route writer failed");
                    break;
                }
                const auto ob = original_bits();
                std::array<u32, 3> pts{};
                std::memcpy(pts.data(), img.data() + 0x0C, 12);
                sim::BitWriter w;
                sim::write_route_state(w, (mflags & 4) != 0, img[100], count, pts.data());
                u8 oflags = 0;
                r.read(goal + 100, &oflags, 1);
                ++n_route;
                if (w.bytes() != ob || oflags != sim::route_flags_after_send(img[100], mflags)) {
                    fail(op, "route goal streams or flags differ");
                    break;
                }
                // Remote read into a fresh goal.
                std::vector<u8> fresh(0x40);
                for (auto& c : fresh) c = static_cast<u8>(g());
                std::memcpy(fresh.data() + 8, &motion_ptr_cell, 4);
                r.write(goal + 0x100, fresh.data(), fresh.size());
                const u8 mflags2 = static_cast<u8>(g());
                r.write(motion + 0x2E, &mflags2, 1);
                set_reader(ob);
                if (!r.call(kRouteRead, goal + 0x100, {reader})) {
                    fail(op, "the original's route read failed");
                    break;
                }
                std::vector<u8> og(0x40);
                r.read(goal + 0x100, og.data(), og.size());
                u8 mflags_after = 0;
                r.read(motion + 0x2E, &mflags_after, 1);
                bool blocked = false;
                i32 n = 0;
                std::array<u32, 3> back{};
                std::memcpy(back.data(), fresh.data() + 0x0C, 12);
                sim::BitReader br(ob.data(), ob.size());
                sim::read_route_state(br, blocked, n, back);
                i32 on = 0;
                std::memcpy(&on, og.data() + 0x18, 4);
                std::array<u32, 3> opts;
                std::memcpy(opts.data(), og.data() + 0x0C, 12);
                const u8 expect_flags = static_cast<u8>((mflags2 & 0xFB) | (blocked ? 4 : 0));
                if (on != n || mflags_after != expect_flags || !std::equal(opts.begin(), opts.begin() + n, back.begin())) {
                    fail(op, "route goal read differs");
                    break;
                }
            }
        }
    }
    std::printf("scenarios %d: whole states written %zu, applied %zu (%zu cell moves, %zu remakes, %zu transport calls), point goals %zu, "
                "manoeuvre goals %zu, routes %zu\n",
                scenarios, n_write, n_apply, n_moves, n_made, n_carried, n_point, n_man, n_route);
    std::printf("%zu mismatches\n", mismatches);
    return mismatches ? 1 : 0;
}
