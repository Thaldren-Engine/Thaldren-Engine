// Differential tester for the script-facing unit values (sim::unit_get_value /
// unit_set_value and the transport queries). Inside a suspended TotalA.exe it
// builds a fake game state (unit table, unit definitions, the Moon Quartet
// height grid) plus, per unit type, the original piece state from the unit's
// real model and script, then calls the original GET / SET / transport
// callbacks with random arguments and unit states and compares every result
// and the touched unit fields with the engine.
//
// Usage: unitvalue_harness <TotalA.exe> <game_dir> [ops_per_unit] [only_unit]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/pose.h"
#include "sim/unit.h"
#include "ta/cob.h"
#include "ta/hpi.h"
#include "ta/model3do.h"
#include "ta/tnt.h"
#include "ta/unitdefs.h"

namespace {

using harness::u32;
using i32 = std::int32_t;

constexpr u32 kGameStatePtr = 0x00511DE8;
constexpr u32 kRelocate = 0x004CB4C0, kNegateXZ = 0x004CB590, kBuildState = 0x0045A950, kMalloc = 0x004D83B0;
constexpr u32 kSetMove = 0x00480C50, kSetTurn = 0x00480CE0;
constexpr u32 kGetValue = 0x00480770, kSetValue = 0x00480B20, kTransporting = 0x00481430, kCarrier = 0x00481470;
constexpr u32 kUnitSize = 0x118, kUnits = 65536;  // the whole id space, so stray ids read zeroed units
constexpr int kLiveUnits = 8;

struct Layout {
    u32 heap = 0, bump_ptr = 0, bump_stub = 0, model = 0, cob = 0, ctx = 0, gs = 0, units = 0, defs = 0, cells = 0;
};

bool setup(harness::Remote& r, Layout& L, const sim::HeightMap& hm) {
    L.heap = r.alloc(64u << 20);
    L.bump_ptr = r.alloc(0x1000);
    L.bump_stub = r.alloc(0x1000, PAGE_EXECUTE_READWRITE);
    L.model = r.alloc(4u << 20);
    L.cob = r.alloc(1u << 20);
    L.ctx = r.alloc(0x1000);
    L.gs = r.alloc(0x40000);
    L.units = r.alloc(static_cast<SIZE_T>(kUnits) * kUnitSize);
    L.defs = r.alloc(0x1000);
    L.cells = r.alloc(static_cast<SIZE_T>(hm.width()) * hm.height() * 13);
    if (!L.heap || !L.gs || !L.units || !L.cells) return false;
    std::vector<std::uint8_t> s = {0xA1};
    auto put = [&](u32 v) { for (int i = 0; i < 4; ++i) s.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
    put(L.bump_ptr);
    s.insert(s.end(), {0x8B, 0x4C, 0x24, 0x08, 0x83, 0xC1, 0x03, 0x83, 0xE1, 0xFC, 0x01, 0x0D});
    put(L.bump_ptr);
    s.push_back(0xC3);
    if (!r.write(L.bump_stub, s.data(), s.size()) || !r.patch_jump(kMalloc, L.bump_stub)) return false;
    // Game state: unit table, height map cells (13 bytes, height at +4) and dimensions.
    r.w32(kGameStatePtr, L.gs);
    r.w32(L.gs + 0x14357, L.units);
    r.w32(L.gs + 0x14287, L.cells);
    r.w32(L.gs + 0x14233, static_cast<u32>(hm.width()));
    r.w32(L.gs + 0x14237, static_cast<u32>(hm.height()));
    std::vector<std::uint8_t> cells(static_cast<std::size_t>(hm.width()) * hm.height() * 13, 0);
    for (i32 z = 0; z < hm.height(); ++z)
        for (i32 x = 0; x < hm.width(); ++x) cells[(static_cast<std::size_t>(z) * hm.width() + x) * 13 + 4] = hm.at(x, z);
    return r.write(L.cells, cells.data(), cells.size());
}

// Remote unit field offsets.
enum : u32 {
    kPitch = 0x64, kHeading = 0x66, kBank = 0x68, kPosX = 0x6A, kPosY = 0x6E, kPosZ = 0x72, kCarrierPtr = 0x86,
    kCargoHead = 0x8A, kCargoNext = 0x8E, kDef = 0x92, kState = 0x9E, kId = 0xA8, kNetDirty = 0xBA, kBuildLeft = 0x104,
    kHp = 0x108, kActivate = 0x10E, kScriptFlags = 0x10F, kFlags = 0x110,
};

struct World : sim::UnitWorld {
    std::vector<sim::Unit>* units = nullptr;
    const sim::HeightMap* hm = nullptr;
    sim::Unit* unit_by_id(std::uint16_t id) override { return id < units->size() ? &(*units)[id] : nullptr; }
    const sim::HeightMap& heightmap() override { return *hm; }
    bool yard_cells_free(sim::Unit&, i32) override { return false; }
    void claim_yard_cells(sim::Unit&) override {}
    void on_activate_flags_changed(sim::Unit&, std::uint8_t, std::uint8_t) override {}
    std::uint32_t random_mod(std::uint32_t) override { return 0; }
    void emit_sfx(sim::Unit&, i32, i32) override {}
    void explode_piece(sim::Unit&, i32, i32) override {}
    void attach_unit(sim::Unit&, i32, i32, i32) override {}
    void drop_unit(sim::Unit&, i32) override {}
};

struct Stats {
    std::size_t units = 0, gets = 0, sets = 0, transport = 0, mismatches = 0, skipped = 0, failures = 0;
    std::size_t per_id[22] = {};
};

i32 random_xz(std::mt19937& g) {
    // Packed X/Z mostly inside the map (610 x 616 cells of 16 units), sometimes anywhere.
    if (g() % 5 == 0) return static_cast<i32>(g());
    const i32 x = static_cast<i32>(g() % 10400) - 200, z = static_cast<i32>(g() % 10500) - 200;
    return sim::pack_xz(x << 16 | static_cast<i32>(g() & 0xFFFF), z << 16 | static_cast<i32>(g() & 0xFFFF));
}

bool run_unit(harness::Remote& r, const Layout& L, ta::Vfs& vfs, const ta::UnitDefRecord& def, const sim::HeightMap& hm,
              int ops, unsigned seed, Stats& st) {
    std::vector<std::uint8_t> model_file, cob_file;
    if (!vfs.read("objects3d\\" + def.object_name + ".3DO", model_file) || !vfs.read("scripts\\" + def.unit_name + ".COB", cob_file)) {
        ++st.skipped;
        return true;
    }
    ta::CobFile cob;
    std::string err;
    const auto m = ta::load_3do(model_file, true);
    if (!m.root || !ta::load_cob(cob_file, cob, err)) {
        ++st.skipped;
        return true;
    }
    const std::string name = def.unit_name;
    std::mt19937 g(seed);

    // Original piece state, owned by the self unit.
    const int self = 1 + static_cast<int>(g() % (kLiveUnits - 1));
    const u32 self_at = L.units + static_cast<u32>(self) * kUnitSize;
    r.w32(L.bump_ptr, L.heap);
    r.write(L.model, model_file.data(), model_file.size());
    if (!r.call(kRelocate, 0, {L.model, L.model}) || !r.call(kNegateXZ, L.model, {L.model})) return false;
    std::vector<std::uint8_t> img = cob_file;
    auto rel = [&](std::size_t off) {
        u32 v;
        std::memcpy(&v, &img[off], 4);
        v += L.cob;
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
    r.write(L.cob, img.data(), img.size());
    u32 state = 0;
    if (!r.call(kBuildState, 0, {L.model, L.cob, self_at}, &state) || !state) return false;
    r.w32(L.ctx + 0x540, state);

    // Engine side.
    const sim::PoseModel pm = ta::to_pose_model(m.root.get());
    std::vector<sim::Unit> units(kLiveUnits);
    std::vector<sim::UnitTypeInfo> types(kLiveUnits);
    World world;
    world.units = &units;
    world.hm = &hm;
    units[static_cast<std::size_t>(self)].pose.build(pm, cob.program.piece_names);
    ++st.units;

    std::vector<std::uint8_t> zero(kLiveUnits * kUnitSize + 0x1000, 0);
    r.write(L.units, zero.data(), kLiveUnits * kUnitSize);
    auto randomize_units = [&]() {
        for (int i = 0; i < kLiveUnits; ++i) {
            sim::Unit& u = units[static_cast<std::size_t>(i)];
            const u32 at = L.units + static_cast<u32>(i) * kUnitSize;
            sim::UnitTypeInfo& t = types[static_cast<std::size_t>(i)];
            t.max_damage = g() % 8 == 0 ? static_cast<u32>(g()) : 1 + g() % 6000;
            t.model_max_y = static_cast<i32>(g());
            u.type = &t;
            u.id = static_cast<std::uint16_t>(i);
            for (auto& c : u.pos) c = g() % 4 == 0 ? static_cast<i32>(g()) : static_cast<i32>((g() % 10000) << 16 | (g() & 0xFFFF));
            u.pitch = static_cast<std::int16_t>(g());
            u.heading = static_cast<std::uint16_t>(g());
            u.bank = static_cast<std::int16_t>(g());
            u.hit_points = static_cast<std::int16_t>(g() % 4 == 0 ? g() : g() % 6000);
            u.activate_flags = static_cast<std::uint8_t>(g());
            u.script_flags = static_cast<std::uint8_t>(g());
            u.flags = static_cast<u32>(g());
            if (g() % 3) u.flags |= sim::unit_flags::kAlive;
            static const float kFractions[] = {0.0f, 1.0f, 0.5f, 0.25f, 0.01f, 0.99f, -0.5f, 2.0f};
            u.build_fraction_left = g() % 2 ? kFractions[g() % 8] : static_cast<float>(g() % 100001) / 100000.0f;
            u.net_dirty = static_cast<std::uint16_t>(g());
            u.carrier = nullptr;
            u.cargo_head = u.cargo_next = nullptr;
            // Remote mirror.
            const u32 def_at = L.defs + static_cast<u32>(i) * 0x200;
            r.write(at + kPitch, &u.pitch, 2);
            r.write(at + kHeading, &u.heading, 2);
            r.write(at + kBank, &u.bank, 2);
            r.write(at + kPosX, &u.pos[0], 4);
            r.write(at + kPosY, &u.pos[1], 4);
            r.write(at + kPosZ, &u.pos[2], 4);
            r.write(at + kId, &u.id, 2);
            r.write(at + kNetDirty, &u.net_dirty, 2);
            r.write(at + kBuildLeft, &u.build_fraction_left, 4);
            r.write(at + kHp, &u.hit_points, 2);
            r.write(at + kActivate, &u.activate_flags, 1);
            r.write(at + kScriptFlags, &u.script_flags, 1);
            r.w32(at + kFlags, u.flags);
            // The definition pointer is biased so that max damage (+0x1FA) and model height (+0x16E) land in our block.
            const u32 def_base = def_at - 0x16E;
            r.w32(at + kDef, def_base);
            r.w32(def_base + 0x16E, static_cast<u32>(t.model_max_y));
            r.w32(def_base + 0x1FA, t.max_damage);
            r.w32(at + kState, i == self ? state : 0);
            r.w32(at + kCarrierPtr, 0);
            r.w32(at + kCargoHead, 0);
            r.w32(at + kCargoNext, 0);
        }
        // A random cargo arrangement for the self unit.
        sim::Unit& me = units[static_cast<std::size_t>(self)];
        sim::Unit* prev = nullptr;
        for (int i = 0; i < kLiveUnits; ++i) {
            if (i == self || g() % 3 != 0) continue;
            sim::Unit& c = units[static_cast<std::size_t>(i)];
            c.carrier = &me;
            const u32 at = L.units + static_cast<u32>(i) * kUnitSize;
            r.w32(at + kCarrierPtr, self_at);
            if (prev) {
                prev->cargo_next = &c;
                r.w32(L.units + static_cast<u32>(prev->id) * kUnitSize + kCargoNext, at);
            } else {
                me.cargo_head = &c;
                r.w32(self_at + kCargoHead, at);
            }
            prev = &c;
        }
        if (g() % 4 == 0) {
            const int c = (self + 1) % kLiveUnits;
            me.carrier = &units[static_cast<std::size_t>(c)];
            r.w32(self_at + kCarrierPtr, L.units + static_cast<u32>(c) * kUnitSize);
        }
    };

    sim::Unit& me = units[static_cast<std::size_t>(self)];
    sim::UnitScriptHost host(me, world);
    const int pieces = static_cast<int>(std::min<std::size_t>(cob.program.piece_names.size(), me.pose.pieces.size()));
    for (int k = 0; k < ops; ++k) {
        if (k % 20 == 0) randomize_units();
        // Move / turn some pieces so piece positions vary.
        if (pieces > 0 && g() % 2) {
            const i32 piece = static_cast<i32>(g() % static_cast<u32>(pieces)), axis = static_cast<i32>(g() % 3);
            if (g() % 2) {
                const i32 v = static_cast<i32>(g() % 0x400000) - 0x200000;
                if (!r.call(kSetMove, L.ctx, {static_cast<u32>(piece), static_cast<u32>(axis), static_cast<u32>(v)})) return false;
                me.pose.set_move(piece, axis, v);
            } else {
                const std::uint16_t v = static_cast<std::uint16_t>(g());
                if (!r.call(kSetTurn, L.ctx, {static_cast<u32>(piece), static_cast<u32>(axis), v})) return false;
                me.pose.set_turn(piece, axis, static_cast<std::int16_t>(v));
            }
        }
        const u32 kind = g() % 10;
        if (kind < 7) {
            // GET with ids 0..21 (0 and 21 are out of range).
            const i32 id = static_cast<i32>(g() % 22);
            i32 a, b;
            switch (id) {
            case 7: case 8: a = static_cast<i32>(g() % static_cast<u32>(pieces + 3)) - 1; b = 0; break;
            case 9: case 10: case 11: a = g() % 6 == 0 ? static_cast<i32>(g()) : static_cast<i32>(g() % (kLiveUnits + 2)); b = 0; break;
            case 12: case 13: case 16: a = random_xz(g); b = 0; break;
            case 14: case 15: {
                auto v = [&]() { return g() % 3 == 0 ? static_cast<i32>(g()) : static_cast<i32>(g() % 0x2000000) - 0x1000000; };
                a = v();
                b = v();
                break;
            }
            default: a = static_cast<i32>(g()); b = static_cast<i32>(g()); break;
            }
            u32 orig = 0;
            if (!r.call(kGetValue, L.ctx, {static_cast<u32>(id), static_cast<u32>(a), static_cast<u32>(b), 0, 0}, &orig)) return false;
            const i32 ours = sim::unit_get_value(me, world, id, a, b);
            ++st.gets;
            ++st.per_id[id];
            if (static_cast<i32>(orig) != ours) {
                std::printf("%s: GET %d (%d, %d): orig %d ours %d\n", name.c_str(), id, a, b, static_cast<i32>(orig), ours);
                ++st.mismatches;
                return true;
            }
        } else if (kind < 9) {
            // SET for ids without game-wide side effects (activation and yard open need the full game).
            static const i32 kIds[] = {0, 2, 3, 4, 5, 6, 7, 9, 12, 16, 17, 19, 21, 99, -1};
            const i32 id = kIds[g() % (sizeof kIds / sizeof kIds[0])];
            const i32 value = g() % 3 == 0 ? static_cast<i32>(g()) : static_cast<i32>(g() % 3);
            if (!r.call(kSetValue, L.ctx, {static_cast<u32>(id), static_cast<u32>(value)})) return false;
            sim::unit_set_value(me, world, id, value);
            ++st.sets;
            const std::uint8_t sf = r.rd<std::uint8_t>(self_at + kScriptFlags);
            const std::uint16_t nd = r.rd<std::uint16_t>(self_at + kNetDirty);
            const u32 fl = r.r32(self_at + kFlags);
            if (sf != me.script_flags || nd != me.net_dirty || fl != me.flags) {
                std::printf("%s: SET %d %d: script flags orig %02x ours %02x, dirty %04x/%04x, flags %08x/%08x\n", name.c_str(), id,
                            value, sf, me.script_flags, nd, me.net_dirty, fl, me.flags);
                ++st.mismatches;
                return true;
            }
        } else {
            u32 orig = 0;
            i32 ours;
            if (g() % 2) {
                const i32 q = g() % 4 == 0 ? static_cast<i32>(g()) : static_cast<i32>(g() % kLiveUnits);
                if (!r.call(kTransporting, L.ctx, {static_cast<u32>(q)}, &orig)) return false;
                ours = host.get_transporting(q);
            } else {
                if (!r.call(kCarrier, L.ctx, {}, &orig)) return false;
                ours = host.get_carrier();
            }
            ++st.transport;
            if (static_cast<i32>(orig) != ours) {
                std::printf("%s: transport query: orig %d ours %d\n", name.c_str(), static_cast<i32>(orig), ours);
                ++st.mismatches;
                return true;
            }
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: unitvalue_harness <TotalA.exe> <game_dir> [ops_per_unit] [only_unit]\n");
        return 2;
    }
    const int ops = argc > 3 ? std::atoi(argv[3]) : 400;
    const std::string only = argc > 4 ? argv[4] : "";
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
    harness::Remote r;
    Layout L;
    if (!r.start(argv[1]) || !setup(r, L, hm)) {
        std::fprintf(stderr, "cannot start the original\n");
        return 1;
    }
    Stats st;
    for (std::size_t i = 1; i < defs.units.size(); ++i) {
        if (!only.empty() && _stricmp(defs.units[i].unit_name.c_str(), only.c_str()) != 0) continue;
        if (!run_unit(r, L, vfs, defs.units[i], hm, ops, static_cast<unsigned>(i * 131 + 3), st)) {
            ++st.failures;
            std::printf("%s: original call failed - restarting\n", defs.units[i].unit_name.c_str());
            r.stop();
            if (!r.start(argv[1]) || !setup(r, L, hm)) return 1;
        }
    }
    std::printf("GET calls per id:");
    for (int i = 0; i < 22; ++i) std::printf(" %d:%zu", i, st.per_id[i]);
    std::printf("\nunits %zu, GET %zu, SET %zu, transport %zu, mismatching units %zu, skipped %zu, original failures %zu\n",
                st.units, st.gets, st.sets, st.transport, st.mismatches, st.skipped, st.failures);
    return st.mismatches || st.failures ? 1 : 0;
}
