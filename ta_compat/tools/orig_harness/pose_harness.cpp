// Differential tester for sim::PoseState. For every unit of the game it loads
// the unit's 3DO model and COB script inside a suspended TotalA.exe, builds the
// original's piece state (allocator patched to a bump allocator), then drives
// the original piece setters/getters and transform sync with random
// operations. After every operation the whole piece state - including the
// transformed vertices - is compared with sim::PoseState driven identically.
//
// Usage: pose_harness <TotalA.exe> <game_dir> [ops_per_unit] [only_unit]

#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "sim/pose.h"
#include "ta/cob.h"
#include "ta/hpi.h"
#include "ta/model3do.h"
#include "ta/unitdefs.h"

namespace {

using harness::u32;
using i32 = std::int32_t;

constexpr u32 kRelocate = 0x004CB4C0;      // cdecl (base, node)
constexpr u32 kNegateXZ = 0x004CB590;      // (node)
constexpr u32 kBuildState = 0x0045A950;  // cdecl (model, cob, owner) -> state
constexpr u32 kSync = 0x0045AB10;          // cdecl (unit)
constexpr u32 kSetMove = 0x00480C50, kSetTurn = 0x00480CE0, kSetVisible = 0x00480D50, kSetCached = 0x00480DB0,
              kSetShaded = 0x00480DF0, kGetMove = 0x00480C30, kGetTurn = 0x00480CB0;  // thiscall on script context
constexpr u32 kMalloc = 0x004D83B0;        // cdecl (tag, size)
constexpr u32 kPieceSize = 54, kStateHeader = 0x22;

struct Layout {
    u32 heap = 0, bump_ptr = 0, bump_stub = 0, model = 0, cob = 0, unit = 0, ctx = 0;
};

bool setup(harness::Remote& r, Layout& L) {
    L.heap = r.alloc(64u << 20);
    L.bump_ptr = r.alloc(0x1000);
    L.bump_stub = r.alloc(0x1000, PAGE_EXECUTE_READWRITE);
    L.model = r.alloc(4u << 20);
    L.cob = r.alloc(1u << 20);
    L.unit = r.alloc(0x1000);
    L.ctx = r.alloc(0x1000);
    if (!L.heap || !L.bump_ptr || !L.bump_stub || !L.model || !L.cob || !L.unit || !L.ctx) return false;
    std::vector<std::uint8_t> s = {0xA1};                       // mov eax,[bump_ptr]
    auto put = [&](u32 v) { for (int i = 0; i < 4; ++i) s.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
    put(L.bump_ptr);
    s.insert(s.end(), {0x8B, 0x4C, 0x24, 0x08, 0x83, 0xC1, 0x03, 0x83, 0xE1, 0xFC, 0x01, 0x0D});  // size, align, add
    put(L.bump_ptr);
    s.push_back(0xC3);
    return r.write(L.bump_stub, s.data(), s.size()) && r.patch_jump(kMalloc, L.bump_stub);
}

// Remote model node pointers in the same depth-first order as ta::to_pose_model.
void walk_model(harness::Remote& r, u32 node, std::map<u32, int>& index, std::vector<u32>& order) {
    if (!node) return;
    index[node] = static_cast<int>(order.size());
    order.push_back(node);
    walk_model(r, r.r32(node + 0x30), index, order);  // child
    walk_model(r, r.r32(node + 0x2C), index, order);  // sibling
}

struct Stats {
    std::size_t units = 0, ops = 0, mismatches = 0, skipped = 0, failures = 0;
};

bool run_unit(harness::Remote& r, const Layout& L, ta::Vfs& vfs, const ta::UnitDefRecord& u, int ops, unsigned seed,
              Stats& st) {
    std::vector<std::uint8_t> model_file, cob_file;
    if (!vfs.read("objects3d\\" + u.object_name + ".3DO", model_file) ||
        !vfs.read("scripts\\" + u.unit_name + ".COB", cob_file)) {
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
    const std::string name = u.unit_name;

    // Original: model (relocate + negate), COB image, state.
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
    const u32 nfun = hdr(1), npieces = hdr(2), nsnd = hdr(5), names_off = hdr(7), pieces_off = hdr(8), snd_off = hdr(10);
    for (u32 i = 0; i < nfun; ++i) rel(names_off + i * 4);
    for (u32 i = 0; i < npieces; ++i) rel(pieces_off + i * 4);
    for (u32 i = 0; i < nsnd; ++i) rel(snd_off + i * 8 + 4);
    for (int i : {6, 7, 8, 9, 10}) rel(static_cast<std::size_t>(i) * 4);
    r.write(L.cob, img.data(), img.size());
    u32 state = 0;
    if (!r.call(kBuildState, 0, {L.model, L.cob, 0}, &state) || !state) return false;
    std::vector<std::uint8_t> zero(0x1000, 0);
    r.write(L.unit, zero.data(), zero.size());
    r.write(L.ctx, zero.data(), zero.size());
    r.w32(L.unit + 0x9E, state);
    r.w32(L.ctx + 0x540, state);

    std::map<u32, int> node_index;
    std::vector<u32> node_order;
    walk_model(r, L.model, node_index, node_order);

    // Ours.
    const sim::PoseModel pm = ta::to_pose_model(m.root.get());
    sim::PoseState pose;
    pose.build(pm, cob.program.piece_names);
    ++st.units;

    const u32 pieces_at = state + kStateHeader;
    auto piece_index = [&](u32 p) { return p ? static_cast<int>((p - pieces_at) / kPieceSize) : -1; };
    auto compare = [&](const std::string& what) -> bool {
        bool ok = true;
        auto bad = [&](const std::string& m) {
            if (ok) std::printf("%s: %s: %s\n", name.c_str(), what.c_str(), m.c_str());
            ok = false;
        };
        std::uint8_t h[kStateHeader];
        r.read(state, h, sizeof h);
        i32 count, cache_count, dirty;
        std::memcpy(&count, h, 4);
        std::memcpy(&cache_count, h + 4, 4);
        std::memcpy(&dirty, h + 8, 4);
        std::int16_t base[3];
        std::memcpy(base, h + 0x18, 6);
        u32 root;
        std::memcpy(&root, h + 0x1E, 4);
        if (count != static_cast<i32>(pose.pieces.size())) bad("piece count");
        if (cache_count != pose.cache_draw_count) bad("cache draw count");
        if (dirty != pose.anim_dirty) bad("dirty flag");
        for (int k = 0; k < 3; ++k)
            if (base[k] != pose.base_angle[static_cast<std::size_t>(k)]) bad("base angle");
        if (piece_index(root) != pose.root) bad("root");
        std::vector<std::uint8_t> ps(static_cast<std::size_t>(count) * kPieceSize);
        if (!ps.empty()) r.read(pieces_at, ps.data(), ps.size());
        for (std::size_t i = 0; i < pose.pieces.size() && i < static_cast<std::size_t>(count); ++i) {
            const std::uint8_t* p = &ps[i * kPieceSize];
            const sim::PosePiece& o = pose.pieces[i];
            u32 model_ptr, points_ptr, sib, chi, par;
            i32 mv[3], xf[3];
            std::int16_t tu[3];
            std::uint16_t skip, flags;
            std::memcpy(&model_ptr, p, 4);
            std::memcpy(mv, p + 4, 12);
            std::memcpy(tu, p + 16, 6);
            std::memcpy(xf, p + 22, 12);
            std::memcpy(&points_ptr, p + 34, 4);
            std::memcpy(&skip, p + 38, 2);
            std::memcpy(&flags, p + 40, 2);
            std::memcpy(&sib, p + 42, 4);
            std::memcpy(&chi, p + 46, 4);
            std::memcpy(&par, p + 50, 4);
            const std::string at = "piece " + std::to_string(i) + " ";
            const auto ni = node_index.find(model_ptr);
            if (ni == node_index.end() || ni->second != o.node) bad(at + "model node");
            for (int k = 0; k < 3; ++k) {
                if (mv[k] != o.move[static_cast<std::size_t>(k)]) bad(at + "move");
                if (tu[k] != o.turn[static_cast<std::size_t>(k)]) bad(at + "turn");
                if (xf[k] != o.xform[static_cast<std::size_t>(k)])
                    bad(at + "xform axis " + std::to_string(k) + ": orig " + std::to_string(xf[k]) + " ours " +
                        std::to_string(o.xform[static_cast<std::size_t>(k)]));
            }
            if (skip != static_cast<std::uint16_t>(o.skip_transform)) bad(at + "skip flag");
            if (flags != o.flags) bad(at + "flags");
            if (piece_index(sib) != o.sibling || piece_index(chi) != o.child || piece_index(par) != o.parent) bad(at + "links");
            std::vector<i32> pts(o.points.size());
            if (!pts.empty()) r.read(points_ptr, pts.data(), pts.size() * 4);
            if (pts != o.points) bad(at + "transformed vertices");
        }
        if (!pose.fault().empty()) bad("fault: " + pose.fault());
        if (!ok) ++st.mismatches;
        return ok;
    };
    if (!compare("build")) return true;

    std::mt19937 g(seed);
    const int usable = static_cast<int>(std::min<std::size_t>(cob.program.piece_names.size(), pose.pieces.size()));
    std::int16_t pitch = 0, bank = 0;
    std::uint16_t heading = 0;
    for (int k = 0; k < ops && usable > 0; ++k) {
        const u32 kind = g() % 100;
        const i32 piece = static_cast<i32>(g() % static_cast<u32>(usable));
        const i32 axis = static_cast<i32>(g() % 3);
        std::string what;
        if (kind < 35) {
            const i32 v = g() % 4 == 0 ? static_cast<i32>(g()) : static_cast<i32>(g() % 0x400000) - 0x200000;
            what = "set_move";
            if (!r.call(kSetMove, L.ctx, {static_cast<u32>(piece), static_cast<u32>(axis), static_cast<u32>(v)})) return false;
            pose.set_move(piece, axis, v);
        } else if (kind < 65) {
            const std::uint16_t v = static_cast<std::uint16_t>(g());
            what = "set_turn";
            if (!r.call(kSetTurn, L.ctx, {static_cast<u32>(piece), static_cast<u32>(axis), v})) return false;
            pose.set_turn(piece, axis, static_cast<std::int16_t>(v));
        } else if (kind < 72) {
            const u32 on = g() % 2;
            const u32 which = g() % 3;
            const u32 fn = which == 0 ? kSetVisible : which == 1 ? kSetCached : kSetShaded;
            what = "set flag";
            if (!r.call(fn, L.ctx, {static_cast<u32>(piece), on})) return false;
            if (which == 0) pose.set_visible(piece, on);
            else if (which == 1) pose.set_cached(piece, on);
            else pose.set_shaded(piece, on);
        } else if (kind < 76) {
            u32 a = 0, b = 0;
            what = "get";
            if (!r.call(kGetMove, L.ctx, {static_cast<u32>(piece), static_cast<u32>(axis)}, &a) ||
                !r.call(kGetTurn, L.ctx, {static_cast<u32>(piece), static_cast<u32>(axis)}, &b))
                return false;
            if (static_cast<i32>(a) != pose.get_move(piece, axis) || b != pose.get_turn(piece, axis)) {
                std::printf("%s: getter results differ (turn orig %u ours %u)\n", name.c_str(), b, pose.get_turn(piece, axis));
                ++st.mismatches;
                return true;
            }
        } else {
            if (g() % 5 == 0) {
                pitch = static_cast<std::int16_t>(g());
                heading = static_cast<std::uint16_t>(g());
                bank = static_cast<std::int16_t>(g());
            } else {
                pitch = static_cast<std::int16_t>(pitch + static_cast<i32>(g() % 21) - 10);
                heading = static_cast<std::uint16_t>(heading + static_cast<i32>(g() % 41) - 20);
                bank = static_cast<std::int16_t>(bank + static_cast<i32>(g() % 21) - 10);
            }
            r.write(L.unit + 0x64, &pitch, 2);
            r.write(L.unit + 0x66, &heading, 2);
            r.write(L.unit + 0x68, &bank, 2);
            what = "sync";
            if (!r.call(kSync, 0, {L.unit})) return false;
            pose.sync(pitch, heading, bank);
        }
        ++st.ops;
        if (!compare(what + " #" + std::to_string(k))) return true;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: pose_harness <TotalA.exe> <game_dir> [ops_per_unit] [only_unit]\n");
        return 2;
    }
    const int ops = argc > 3 ? std::atoi(argv[3]) : 400;
    const std::string only = argc > 4 ? argv[4] : "";
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[2]);
    ta::UnitDefContext ctx;
    ta::load_unit_context(vfs, ctx);
    const ta::UnitDefTable units = ta::load_unit_defs(vfs, ctx);

    harness::Remote r;
    Layout L;
    if (!r.start(argv[1]) || !setup(r, L)) {
        std::fprintf(stderr, "cannot start the original\n");
        return 1;
    }
    Stats st;
    for (std::size_t i = 1; i < units.units.size(); ++i) {
        const auto& u = units.units[i];
        if (!only.empty() && _stricmp(u.unit_name.c_str(), only.c_str()) != 0) continue;
        if (!run_unit(r, L, vfs, u, ops, static_cast<unsigned>(i * 31 + 7), st)) {
            ++st.failures;
            std::printf("%s: original call failed - restarting\n", u.unit_name.c_str());
            r.stop();
            if (!r.start(argv[1]) || !setup(r, L)) return 1;
        }
    }
    std::printf("units %zu, operations %zu, mismatching units %zu, skipped %zu, original failures %zu\n", st.units, st.ops,
                st.mismatches, st.skipped, st.failures);
    return st.mismatches || st.failures ? 1 : 0;
}
