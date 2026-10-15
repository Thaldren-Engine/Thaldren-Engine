// Pixel comparison of unit drawing: inside a suspended TotalA.exe the original's own unit
// draw (0x45AC20: pose sync, sprite build, depth-plane composite, shadows, units on board)
// runs into an 8-bit 640x480 back buffer; the engine draws the same unit through
// render::build_draw_list and the CPU backend. Every scenario uses a real unit type's model,
// script pieces and textures with a random pose, attitude, flags (structure, z-buffer,
// under construction, cloaked, digger / hover / floater / no shadow), sea level, visual
// options (shadows, unit shadows, anti-aliasing, shading), viewer, scroll and sometimes units
// on board. The frames are compared pixel by pixel; differing scenarios are written as BMPs
// (original, engine, difference) to the output directory.
//
// Usage: render_harness <TotalA.exe> <game_dir> [scenarios] [seed] [out_dir] [only_unit]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "render/battle_view.h"
#include "render/draw_list.h"
#include "render/presenter.h"
#include "render/unit_look.h"
#include "sim/pose.h"
#include "sim/unit.h"
#include "ta/cob.h"
#include "ta/hpi.h"
#include "ta/model3do.h"
#include "ta/render_assets.h"
#include "ta/unitdefs.h"

namespace {

using harness::u32;
using i32 = std::int32_t;
using u16 = std::uint16_t;
using u8 = std::uint8_t;

constexpr u32 kGameStatePtr = 0x00511DE8, kDisplayPtr = 0x0051FBD0;
constexpr u32 kRelocate = 0x004CB4C0, kNegateXZ = 0x004CB590, kBuildState = 0x0045A950;
constexpr u32 kSetMove = 0x00480C50, kSetTurn = 0x00480CE0, kSetVisible = 0x00480D50, kSetCached = 0x00480DB0,
              kSetShaded = 0x00480DF0;
constexpr u32 kMalloc = 0x004D83B0, kGameAlloc = 0x004B4F10, kCacheAlloc = 0x00437A30;
constexpr u32 kDrawUnit = 0x0045AC20, kMakeContext = 0x004B8A80, kHeightAt = 0x00485070, kWaterTable = 0x004BAF30;
constexpr int W = 640, H = 480;

struct Layout {
    u32 heap = 0, bump_ptr = 0, stubs = 0, gs = 0, display = 0, cache = 0, scratch = 0, frame_hdr = 0, frame = 0, ctx = 0,
        script_ctx = 0, players = 0, cells = 0, defs = 0, units = 0, motions = 0, textures = 0, tex_ptr = 0, palette = 0;
    u32 alp = 0, shd = 0, lht = 0, water = 0;
};
constexpr int kMapCells = 64;

void put32(std::vector<u8>& s, u32 v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<u8>(v >> (8 * i)));
}

bool setup(harness::Remote& r, Layout& L, const std::vector<u8>& alp, const std::vector<u8>& shd, const std::vector<u8>& lht,
           const std::vector<u8>& pal) {
    L.heap = r.alloc(96u << 20);
    L.bump_ptr = r.alloc(0x1000);
    L.stubs = r.alloc(0x1000, PAGE_EXECUTE_READWRITE);
    L.gs = r.alloc(0x40000);
    L.display = r.alloc(0x1000);
    L.cache = r.alloc(0x100);
    L.scratch = r.alloc(0x100);
    L.frame_hdr = r.alloc(0x100);
    L.frame = r.alloc(W * H);
    L.ctx = r.alloc(0x100);
    L.script_ctx = r.alloc(0x1000);
    L.players = r.alloc(0x1000);
    L.cells = r.alloc(kMapCells * kMapCells * 13);
    L.defs = r.alloc(0x2000);
    L.units = r.alloc(4 * 0x118);
    L.motions = r.alloc(0x400);
    L.textures = r.alloc(48u << 20);
    L.palette = r.alloc(0x400);
    L.alp = r.alloc(0x10000);
    L.shd = r.alloc(0x2000);
    L.lht = r.alloc(0x2000);
    L.water = r.alloc(0x100);
    const u32 scratch_px = r.alloc(4u << 20), scratch_depth = r.alloc(4u << 20);
    if (!L.heap || !L.stubs || !L.gs || !L.textures || !scratch_px || !scratch_depth) return false;
    L.tex_ptr = L.textures;
    // Bump allocators: malloc-with-name (size at +8), the game allocator (size at +4), and the
    // sprite cache's block allocator (thiscall (slot, size): *slot = block, returns 1).
    std::vector<u8> s;
    auto align16 = [&]() {
        while (s.size() % 16) s.push_back(0xCC);
    };
    const int size_at[2] = {8, 4};
    const u32 targets[2] = {kMalloc, kGameAlloc};
    for (int k = 0; k < 2; ++k) {
        const u32 at = L.stubs + static_cast<u32>(s.size());
        s.push_back(0xA1); put32(s, L.bump_ptr);
        s.insert(s.end(), {0x8B, 0x4C, 0x24, static_cast<u8>(size_at[k]), 0x83, 0xC1, 0x0F, 0x83, 0xE1, 0xF0, 0x01, 0x0D});
        put32(s, L.bump_ptr);
        s.push_back(0xC3);
        align16();
        if (!r.patch_jump(targets[k], at)) return false;
    }
    {
        const u32 at = L.stubs + static_cast<u32>(s.size());
        s.push_back(0xA1); put32(s, L.bump_ptr);                                   // mov eax,[bump]
        s.insert(s.end(), {0x8B, 0x54, 0x24, 0x08, 0x83, 0xC2, 0x1F, 0x83, 0xE2, 0xF0});  // edx = size + 31 & ~15
        s.insert(s.end(), {0x01, 0x15}); put32(s, L.bump_ptr);                     // add [bump],edx
        s.insert(s.end(), {0x83, 0xC0, 0x08});                                     // add eax,8 (block header)
        s.insert(s.end(), {0x8B, 0x54, 0x24, 0x04, 0x89, 0x02});                   // mov edx,[esp+4]; mov [edx],eax
        s.insert(s.end(), {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x08, 0x00});       // mov eax,1; ret 8
        align16();
        if (!r.patch_jump(kCacheAlloc, at)) return false;
    }
    if (!r.write(L.stubs, s.data(), s.size())) return false;
    r.w32(kGameStatePtr, L.gs);
    r.w32(kDisplayPtr, L.display);
    // Display: mixing, shade, light and water tables; all table features on.
    r.write(L.alp, alp.data(), 0x10000);
    r.write(L.shd, shd.data(), 0x2000);
    r.write(L.lht, lht.data(), 0x2000);
    r.write(L.palette, pal.data(), 0x400);
    r.w32(L.display + 0xC0, L.alp);
    r.w32(L.display + 0xC4, L.shd);
    r.w32(L.display + 0xC8, L.lht);
    r.w32(L.display + 0xD0, L.water);
    r.w32(L.display + 0xF0, 0x20 | 0x40 | 0x80 | 0x200);
    // The water table is built by the original's own routine (checked against ours).
    if (!r.call(kWaterTable, 0, {L.palette})) return false;
    // Sprite cache with its scratch frame (+0x10).
    r.w32(L.cache + 0x10, L.scratch);
    std::uint8_t sh[0x18] = {};
    sh[8] = 1;
    std::memcpy(sh + 0x10, &scratch_px, 4);
    std::memcpy(sh + 0x14, &scratch_depth, 4);
    r.write(L.scratch, sh, sizeof sh);
    r.w32(L.gs + 0x1437B, L.cache);
    // Back buffer and its drawing context (clip = the battle view).
    std::uint8_t fh[0x18] = {};
    const u16 w = W, h = H;
    std::memcpy(fh, &w, 2);
    std::memcpy(fh + 2, &h, 2);
    std::memcpy(fh + 0x10, &L.frame, 4);
    r.write(L.frame_hdr, fh, sizeof fh);
    if (!r.call(kMakeContext, 0, {L.ctx, L.frame_hdr})) return false;
    r.w32(L.ctx + 0x1C, 128);
    r.w32(L.ctx + 0x20, 32);
    r.w32(L.ctx + 0x24, W - 1);
    r.w32(L.ctx + 0x28, H - 33);
    // Players: record pointer at GS + slot * 0x14B + 0x1B8A, team colour at +0x96.
    for (int p = 0; p < 10; ++p) r.w32(L.gs + static_cast<u32>(p) * 0x14B + 0x1B8A, L.players + static_cast<u32>(p) * 0x100);
    // Height map.
    r.w32(L.gs + 0x14287, L.cells);
    r.w32(L.gs + 0x14233, kMapCells);
    r.w32(L.gs + 0x14237, kMapCells);
    return true;
}

// Remote model tree nodes in the engine's piece order (node, child subtree, sibling).
void walk_model(harness::Remote& r, u32 node, std::vector<u32>& order) {
    if (!node) return;
    order.push_back(node);
    walk_model(r, r.r32(node + 0x30), order);
    walk_model(r, r.r32(node + 0x2C), order);
}

struct TypeAssets {
    std::vector<u8> model_file, cob_file;
    std::unique_ptr<ta::Model3doPiece> root;
    sim::PoseModel pose_model;
    render::Model model;
    ta::CobFile cob;
    bool ok = false;
};

struct Side {
    const ta::UnitDefRecord* def = nullptr;
    TypeAssets* assets = nullptr;
    u32 unit = 0, state = 0;
    sim::Unit eng;
    u32 type_flags = 0;
    u8 owner_slot = 0, team = 0;
};

u32 bump(harness::Remote& r, const Layout& L, u32 n) {
    const u32 at = r.r32(L.bump_ptr);
    r.w32(L.bump_ptr, at + ((n + 15) & ~15u));
    return at;
}

// Texture frames in the original's layout: sequence {u16 count, ..., frame pointers at +0x28 + 8i}.
std::map<const render::ModelTexture*, u32> g_seq;
u32 remote_sequence(harness::Remote& r, Layout& L, const render::ModelTexture* t) {
    auto it = g_seq.find(t);
    if (it != g_seq.end()) return it->second;
    const u32 seq = L.tex_ptr;
    L.tex_ptr += 0x28 + static_cast<u32>(t->frames.size()) * 8 + 16;
    std::vector<u8> sq(0x28 + t->frames.size() * 8, 0);
    const u16 n = static_cast<u16>(t->frames.size());
    std::memcpy(sq.data(), &n, 2);
    for (std::size_t i = 0; i < t->frames.size(); ++i) {
        const render::Sprite8& f = t->frames[i];
        const u32 hdr = L.tex_ptr;
        L.tex_ptr += (0x18 + static_cast<u32>(f.pixels.size()) + 15) & ~15u;
        std::vector<u8> fh(0x18 + f.pixels.size(), 0);
        const u16 fw = static_cast<u16>(f.width), fhh = static_cast<u16>(f.height);
        const std::int16_t xo = static_cast<std::int16_t>(f.x_offset), yo = static_cast<std::int16_t>(f.y_offset);
        std::memcpy(&fh[0], &fw, 2);
        std::memcpy(&fh[2], &fhh, 2);
        std::memcpy(&fh[4], &xo, 2);
        std::memcpy(&fh[6], &yo, 2);
        fh[8] = f.key;
        const u32 px = hdr + 0x18;
        std::memcpy(&fh[0x10], &px, 4);
        std::memcpy(&fh[0x18], f.pixels.data(), f.pixels.size());
        r.write(hdr, fh.data(), fh.size());
        std::memcpy(&sq[0x28 + i * 8], &hdr, 4);
    }
    r.write(seq, sq.data(), sq.size());
    g_seq[t] = seq;
    return seq;
}

// Loads the model and script into the original, builds its piece state for `unit` and binds
// the primitives' looks exactly as the engine's model has them.
bool load_side(harness::Remote& r, Layout& L, Side& sd, u32 cursor_frame) {
    TypeAssets& a = *sd.assets;
    const u32 model = bump(r, L, static_cast<u32>(a.model_file.size()) + 64);
    r.write(model, a.model_file.data(), a.model_file.size());
    if (!r.call(kRelocate, 0, {model, model}) || !r.call(kNegateXZ, model, {model})) return false;
    std::vector<u8> img = a.cob_file;
    const u32 cob = bump(r, L, static_cast<u32>(img.size()) + 64);
    auto rel = [&](std::size_t off) {
        u32 v;
        std::memcpy(&v, &img[off], 4);
        v += cob;
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
    r.write(cob, img.data(), img.size());
    u32 state = 0;
    if (!r.call(kBuildState, 0, {model, cob, sd.unit}, &state) || !state) return false;
    sd.state = state;
    r.w32(state + 0xC, sd.unit);
    // Primitive looks.
    std::vector<u32> nodes;
    walk_model(r, model, nodes);
    for (std::size_t i = 0; i < nodes.size() && i < a.model.pieces.size(); ++i) {
        const u32 prims = r.r32(nodes[i] + 0x28);
        const auto& mp = a.model.pieces[i].primitives;
        for (std::size_t k = 0; k < mp.size(); ++k) {
            const render::ModelPrimitive& p = mp[k];
            const u32 at = prims + static_cast<u32>(k) * 0x20;
            u32 flags = 0;
            if (p.flat) {
                flags = 1;
                r.w32(at, p.colour);
            } else if (p.texture && !p.texture->frames.empty()) {
                const u32 seq = remote_sequence(r, L, p.texture);
                flags = (p.texture->animated ? 2u : 0u) | (p.texture->team_coloured ? 4u : 0u);
                r.w32(at + 0x18, seq);
                if (p.texture->animated) r.w32(at + 0x10, cursor_frame % static_cast<u32>(p.texture->frames.size()));
                else r.w32(at + 0x10, r.r32(seq + 0x28));
            }
            r.w32(at + 0x1C, flags);
        }
    }
    sd.eng.pose.build(a.pose_model, a.cob.program.piece_names);
    return true;
}

struct Stats {
    int scenarios = 0, same = 0, differ = 0, failures = 0, skipped = 0;
    long long pixels = 0;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: render_harness <TotalA.exe> <game_dir> [scenarios] [seed] [out_dir] [only_unit]\n");
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const int scenarios = argc > 3 ? std::atoi(argv[3]) : 20;
    const unsigned seed = argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 1u;
    const std::string out_dir = argc > 5 ? argv[5] : "";
    const std::string only = argc > 6 ? argv[6] : "";
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[2]);
    ta::UnitDefContext uctx;
    ta::load_unit_context(vfs, uctx);
    const ta::UnitDefTable units = ta::load_unit_defs(vfs, uctx);
    ta::TextureLibrary textures;
    textures.load(vfs);
    render::Palette palette;
    render::BlendTable alp;
    std::vector<u8> shd, lht, pal;
    if (!ta::load_palette(vfs, palette) || !ta::load_blend_table(vfs, alp) || !vfs.read("palettes\\PALETTE.SHD", shd) ||
        !vfs.read("palettes\\PALETTE.LHT", lht) || !vfs.read("palettes\\PALETTE.PAL", pal) || shd.size() < 0x2000 ||
        lht.size() < 0x2000) {
        std::fprintf(stderr, "cannot read the palette tables\n");
        return 1;
    }
    const std::vector<u8> water = render::water_tint_table(palette);

    harness::Remote r;
    Layout L;
    if (!r.start(argv[1]) || !setup(r, L, alp, shd, lht, pal)) {
        std::fprintf(stderr, "cannot start the original\n");
        return 1;
    }
    {
        std::vector<u8> theirs(256);
        r.read(L.water, theirs.data(), 256);
        int bad = 0;
        for (int i = 0; i < 256; ++i) bad += theirs[static_cast<std::size_t>(i)] != water[static_cast<std::size_t>(i)];
        std::printf("water table: %d of 256 entries differ\n", bad);
    }
    std::map<std::string, std::unique_ptr<TypeAssets>> cache;
    auto assets_of = [&](const ta::UnitDefRecord& d) -> TypeAssets* {
        auto& slot = cache[d.unit_name];
        if (!slot) {
            slot = std::make_unique<TypeAssets>();
            TypeAssets& a = *slot;
            std::string err;
            if (vfs.read("objects3d\\" + d.object_name + ".3DO", a.model_file) && vfs.read("scripts\\" + d.unit_name + ".COB", a.cob_file)) {
                auto m = ta::load_3do(a.model_file, true);
                if (m.root && ta::load_cob(a.cob_file, a.cob, err)) {
                    a.root = std::move(m.root);
                    a.pose_model = ta::to_pose_model(a.root.get());
                    a.model = ta::build_render_model(a.root.get(), textures);
                    a.ok = true;
                }
            }
        }
        return slot->ok ? slot.get() : nullptr;
    };
    std::vector<const ta::UnitDefRecord*> pool;
    for (std::size_t i = 1; i < units.units.size(); ++i)
        if (only.empty() || _stricmp(units.units[i].unit_name.c_str(), only.c_str()) == 0) pool.push_back(&units.units[i]);
    if (pool.empty()) return 1;

    std::mt19937 g(seed);
    auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(g); };
    Stats st;
    int written = 0;
    for (int sc = 0; sc < scenarios; ++sc) {
        r.w32(L.bump_ptr, L.heap);
        const u32 heap_top = L.heap + (64u << 20);
        (void)heap_top;
        // Scenario parameters.
        const int ncargo = rnd(0, 3) == 0 ? rnd(1, 2) : 0;
        std::vector<Side> sides(static_cast<std::size_t>(1 + ncargo));
        bool ok = true;
        for (std::size_t k = 0; k < sides.size(); ++k) {
            Side& sd = sides[k];
            for (int tries = 0; tries < 20 && !sd.assets; ++tries) {
                sd.def = pool[static_cast<std::size_t>(rnd(0, static_cast<int>(pool.size()) - 1))];
                sd.assets = assets_of(*sd.def);
            }
            if (!sd.assets) ok = false;
            sd.unit = L.units + static_cast<u32>(k) * 0x118;
        }
        if (!ok) {
            ++st.skipped;
            continue;
        }
        u16 visual = 0;
        if (rnd(0, 4)) visual |= 4;   // shadows
        if (rnd(0, 3)) visual |= 8;   // unit shadows
        if (rnd(0, 1)) visual |= 2;   // anti-aliasing
        if (rnd(0, 1)) visual |= 0x20;  // shading
        const u8 sea = static_cast<u8>(rnd(0, 3) == 0 ? 0 : rnd(0, 120));
        const u8 viewer = static_cast<u8>(rnd(0, 3));
        const u32 tick = static_cast<u32>(rnd(0, 100000));
        const u32 cursor = static_cast<u32>(rnd(0, 50));
        // Height map.
        std::vector<u8> cells(kMapCells * kMapCells * 13, 0);
        const int base_h = rnd(0, 100);
        for (int z = 0; z < kMapCells; ++z)
            for (int x = 0; x < kMapCells; ++x) cells[static_cast<std::size_t>((z * kMapCells + x) * 13 + 4)] = static_cast<u8>(base_h + rnd(0, 20));
        r.write(L.cells, cells.data(), cells.size());
        std::vector<u8> zero(0x40000, 0);
        // (only the fields the draw reads are set; the rest of the game state stays as it was)
        r.write(L.gs + 0x2A43, &viewer, 1);
        r.write(L.gs + 0x1427F, &sea, 1);
        r.write(L.gs + 0x37F06, &visual, 2);
        u8 zero8 = 0;
        r.write(L.gs + 0x14280, &zero8, 1);
        r.w32(L.gs + 0x38A47, tick);
        // The carrier's position and the view.
        const i32 cx = (rnd(200, 800) << 16) | rnd(0, 0xFFFF), cz = (rnd(200, 800) << 16) | rnd(0, 0xFFFF);
        const i32 cy = ((base_h + rnd(-30, 60)) << 16) | rnd(0, 0xFFFF);
        const int scroll_x = (cx >> 16) - 256 + (rnd(0, 5) == 0 ? rnd(-300, 300) : rnd(-40, 40));
        const int scroll_y = (cz >> 16) - 208 + (rnd(0, 5) == 0 ? rnd(-260, 260) : rnd(-40, 40));
        r.w32(L.gs + 0x1431F, static_cast<u32>(scroll_x));
        r.w32(L.gs + 0x14323, static_cast<u32>(scroll_y));
        std::vector<u8> unit_zero(4 * 0x118, 0);
        r.write(L.units, unit_zero.data(), unit_zero.size());
        for (std::size_t k = 0; k < sides.size(); ++k) {
            Side& sd = sides[k];
            sd.owner_slot = static_cast<u8>(rnd(0, 3));
            sd.team = static_cast<u8>((sd.owner_slot * 7 + sc) % 10);  // one colour per player slot
            r.write(L.players + static_cast<u32>(sd.owner_slot) * 0x100 + 0x96, &sd.team, 1);
            sd.type_flags = sd.def->type_flags;
            if (rnd(0, 7) == 0) sd.type_flags ^= render::look_type_flags::kDigger;
            if (rnd(0, 7) == 0) sd.type_flags ^= render::look_type_flags::kNoShadow;
            if (rnd(0, 7) == 0) sd.type_flags ^= render::look_type_flags::kHover;
            if (rnd(0, 9) == 0) sd.type_flags ^= render::look_type_flags::kFloater;
            const u32 def = L.defs + static_cast<u32>(k) * 0x400;
            r.w32(def + 0x241, sd.type_flags);
            sd.eng = sim::Unit();
            sim::Unit& e = sd.eng;
            e.id = static_cast<u16>(rnd(1, 500));
            e.pos = {cx, cy, cz};
            if (k > 0) {
                e.pos[0] = cx + (rnd(-30, 30) << 16) + rnd(0, 0xFFFF);
                e.pos[1] = cy + (rnd(-10, 30) << 16) + rnd(0, 0xFFFF);
                e.pos[2] = cz + (rnd(-30, 30) << 16) + rnd(0, 0xFFFF);
            }
            const bool structure = rnd(0, 2) == 0;
            e.flags = (structure ? 0x20000000u : 0u) | 0x10000000u | (rnd(0, 7) == 0 ? 0x200u : 0u);
            if (k > 0 && rnd(0, 5) == 0) e.flags |= 0x20000u;  // hidden on board
            e.draw_flags = rnd(0, 4) ? 1u : 0u;
            e.build_fraction_left = rnd(0, 3) == 0 ? static_cast<float>(rnd(1, 1000)) / 1000.0f : 0.0f;
            e.activate_flags = static_cast<u8>((rnd(0, 1) ? 1 : 0) | (rnd(0, 9) == 0 ? 4 : 0));
            e.pitch = static_cast<std::int16_t>(rnd(0, 3) == 0 ? rnd(-32768, 32767) : rnd(-2000, 2000));
            e.heading = static_cast<u16>(rnd(0, 65535));
            e.bank = static_cast<std::int16_t>(rnd(0, 3) == 0 ? rnd(-32768, 32767) : rnd(-2000, 2000));
            const u32 u = sd.unit;
            const u32 motion = L.motions + static_cast<u32>(k) * 0x100;
            r.w32(motion + 0x20, rnd(0, 1) ? 1u : 0u);
            r.w32(u + 0x00, motion);
            r.write(u + 0x64, &e.pitch, 2);
            r.write(u + 0x66, &e.heading, 2);
            r.write(u + 0x68, &e.bank, 2);
            r.write(u + 0x6A, e.pos.data(), 12);
            r.w32(u + 0x92, def);
            const u16 type_index = 1;
            r.write(u + 0xA6, &type_index, 2);
            r.write(u + 0xA8, &e.id, 2);
            r.write(u + 0xFF, &sd.owner_slot, 1);
            r.write(u + 0x104, &e.build_fraction_left, 4);
            r.write(u + 0x10E, &e.activate_flags, 1);
            r.w32(u + 0x110, e.flags);
            r.w32(u + 0x114, e.draw_flags);
            if (!load_side(r, L, sd, cursor)) {
                ok = false;
                break;
            }
            r.w32(u + 0x9E, sd.state);
            // Random pose operations, the same on both sides.
            r.w32(L.script_ctx + 0x540, sd.state);
            const int usable = static_cast<int>(std::min<std::size_t>(sd.assets->cob.program.piece_names.size(), e.pose.pieces.size()));
            const int nops = rnd(0, 30);
            for (int op = 0; op < nops && usable > 0 && ok; ++op) {
                const i32 piece = rnd(0, usable - 1), axis = rnd(0, 2);
                const int kind = rnd(0, 99);
                if (kind < 30) {
                    const i32 v = rnd(-0x200000, 0x200000);
                    ok = r.call(kSetMove, L.script_ctx, {static_cast<u32>(piece), static_cast<u32>(axis), static_cast<u32>(v)});
                    e.pose.set_move(piece, axis, v);
                } else if (kind < 75) {
                    const u16 v = static_cast<u16>(rnd(0, 65535));
                    ok = r.call(kSetTurn, L.script_ctx, {static_cast<u32>(piece), static_cast<u32>(axis), v});
                    e.pose.set_turn(piece, axis, static_cast<std::int16_t>(v));
                } else {
                    const u32 on = static_cast<u32>(rnd(0, 3) != 0);
                    const int which = rnd(0, 2);
                    const u32 fn = which == 0 ? kSetVisible : which == 1 ? kSetCached : kSetShaded;
                    ok = r.call(fn, L.script_ctx, {static_cast<u32>(piece), which == 0 ? static_cast<u32>(rnd(0, 5) != 0) : on});
                    if (which == 0) e.pose.set_visible(piece, which == 0 ? 1 : on);
                }
            }
            if (!ok) break;
            // Read back the flags the setters left (visibility draws differ), to mirror exactly.
            const u32 pieces_at = sd.state + 0x22;
            for (std::size_t p = 0; p < e.pose.pieces.size(); ++p) {
                u16 fl = 0;
                r.read(pieces_at + static_cast<u32>(p) * 54 + 40, &fl, 2);
                e.pose.pieces[p].flags = fl;
            }
        }
        if (!ok) {
            ++st.failures;
            std::printf("scenario %d: original call failed - restarting\n", sc);
            r.stop();
            g_seq.clear();
            L = Layout();
            if (!r.start(argv[1]) || !setup(r, L, alp, shd, lht, pal)) return 1;
            continue;
        }
        // Cargo links.
        for (std::size_t k = 1; k < sides.size(); ++k) {
            r.w32(sides[k].unit + 0x86, sides[0].unit);
            r.w32((k == 1 ? sides[0].unit + 0x8A : sides[k - 1].unit + 0x8E), sides[k].unit);
        }
        for (std::size_t k = 1; k < sides.size(); ++k) {
            sides[k].eng.carrier = &sides[0].eng;
            if (k == 1) sides[0].eng.cargo_head = &sides[k].eng;
            else sides[k - 1].eng.cargo_next = &sides[k].eng;
        }
        // Background: a fixed pattern, the same on both sides.
        std::vector<u8> bg(W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) bg[static_cast<std::size_t>(y * W + x)] = static_cast<u8>(((x / 8 + y / 8) & 1) ? 0x50 + (x + y) % 32 : 0xC0 + (x * 3 + y) % 24);
        r.write(L.frame, bg.data(), bg.size());
        u32 ground = 0;
        r.call(kHeightAt, 0, {sides[0].unit + 0x6A}, &ground);
        // Original draw.
        if (!r.call(kDrawUnit, 0, {L.ctx, sides[0].unit}, nullptr, 10000)) {
            ++st.failures;
            std::printf("scenario %d: original draw failed - restarting\n", sc);
            r.stop();
            g_seq.clear();
            L = Layout();
            if (!r.start(argv[1]) || !setup(r, L, alp, shd, lht, pal)) return 1;
            continue;
        }
        std::vector<u8> theirs(W * H);
        r.read(L.frame, theirs.data(), theirs.size());

        // Engine draw.
        render::BattleScene scene;
        scene.camera.scroll_x = scroll_x;
        scene.camera.scroll_y = scroll_y;
        scene.options.shadows = (visual & 4) != 0;
        scene.options.unit_shadows = (visual & 8) != 0;
        scene.options.antialias = (visual & 2) != 0;
        scene.options.shading = (visual & 0x20) != 0;
        scene.tables.mix = &alp;
        scene.tables.shade = &shd;
        scene.tables.water = &water;
        scene.sea_level = sea;
        scene.game_tick = tick;
        for (std::size_t k = 0; k < sides.size(); ++k) {
            Side& sd = sides[k];
            render::UnitInstance ui;
            ui.unit = &sd.eng;
            ui.model = &sd.assets->model;
            ui.params.team_colour = sd.team;
            ui.params.animation_frame = cursor;
            ui.type_flags = sd.type_flags;
            ui.shadow = true;
            ui.ground_height = static_cast<i32>(ground);
            ui.translucent = (sd.eng.activate_flags & 4) != 0;
            ui.own_view = (sd.eng.flags & 0x200) || sd.owner_slot == viewer;
            const u32 motion_moving = r.r32(L.motions + static_cast<u32>(k) * 0x100 + 0x20);
            ui.animate_direct = (sd.eng.flags & 0x20000000u) ? (sd.eng.activate_flags & 1) != 0 : motion_moving != 0;
            scene.units.push_back(ui);
        }
        // The carrier's own decision applies to its cargo's direct pieces.
        for (std::size_t k = 1; k < scene.units.size(); ++k) scene.units[k].animate_direct = scene.units[0].animate_direct;
        render::DrawList list = render::build_draw_list(scene);
        render::Sprite8 bgs;
        bgs.width = W;
        bgs.height = H;
        bgs.has_key = false;
        bgs.pixels = bg;
        list.cmds.insert(list.cmds.begin(), render::DrawCmd{render::DrawOp::Blit, &bgs, false, 0, 0, 0, 0, 0, {0, 0, W - 1, H - 1}});
        const render::Image8 mine = render::execute(list, alp);
        int diff = 0, drawn = 0;
        for (int i = 0; i < W * H; ++i) {
            diff += mine.pixels[static_cast<std::size_t>(i)] != theirs[static_cast<std::size_t>(i)];
            drawn += bg[static_cast<std::size_t>(i)] != theirs[static_cast<std::size_t>(i)];
        }
        ++st.scenarios;
        st.pixels += diff;
        const Side& c0 = sides[0];
        std::printf("scenario %d: %s%s structure=%d zbuf=%d build=%.3f cargo=%d visual=%02X sea=%d y=%d: %d drawn, %d differ\n", sc,
                    c0.def->unit_name.c_str(), "", (c0.eng.flags & 0x20000000u) ? 1 : 0, c0.eng.draw_flags & 1, c0.eng.build_fraction_left,
                    ncargo, visual, sea, c0.eng.pos[1] >> 16, drawn, diff);
        if (diff == 0) {
            ++st.same;
        } else {
            ++st.differ;
            if (!out_dir.empty() && written < 12) {
                ++written;
                render::Image8 a(W, H), b(W, H), d(W, H);
                a.pixels = theirs;
                b.pixels = mine.pixels;
                for (int i = 0; i < W * H; ++i) d.pixels[static_cast<std::size_t>(i)] = theirs[static_cast<std::size_t>(i)] == mine.pixels[static_cast<std::size_t>(i)] ? 0 : 0xFF;
                const std::string base = out_dir + "/unit_" + std::to_string(sc);
                render::write_bmp(base + "_orig.bmp", a, palette);
                render::write_bmp(base + "_engine.bmp", b, palette);
                render::write_bmp(base + "_diff.bmp", d, palette);
            }
        }
    }
    std::printf("scenarios %d: identical %d, different %d (%lld pixels), skipped %d, original failures %d\n", st.scenarios, st.same,
                st.differ, st.pixels, st.skipped, st.failures);
    return st.differ || st.failures ? 1 : 0;
}
