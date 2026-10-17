// Whole-frame comparison of the battle view (render_harness's approach extended to the frame):
// the engine plays a skirmish (computer players) through the client (ta::ClientWorld,
// client::GameClient); at chosen steps the battle's state is written into a suspended
// TotalA.exe as the original holds it (map cells, tiles, feature types and animation slots, unit
// table with object states, players' sight and explored maps, fog art, projectiles, effects,
// options and the view), the original's own frame draw (Render_DrawBattleFrame 0x468CF0, with
// the interface-only parts stubbed: bottom bar, GUI gadgets, console, cursor) runs into an 8-bit
// back buffer allocated here, and the battle view rectangle is compared pixel by pixel with the
// client's frame. Differing scenes are written as BMPs (original, engine, difference).
//
// The game itself is never started: the process stays suspended and only the routines called
// here run (remote threads). Error boxes are suppressed; faults are reported with their address.
//
// Usage: frame_harness <TotalA.exe> <game_dir> [scenes] [seed] [out_dir] [--map NAME]
//                      [--res WxH] [--steps N] [--no-fog] [--players N] [--verbose]

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "client/game_client.h"
#include "remote.h"
#include "render/draw_list.h"
#include "render/presenter.h"
#include "sim/battle.h"
#include "ta/client_world.h"
#include "ta/model3do.h"
#include "ta/render_assets.h"
#include "ta/tdf.h"

namespace {

using harness::u32;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using i16 = std::int16_t;
using i32 = std::int32_t;

// Original routines and statics.
constexpr u32 kGameStatePtr = 0x00511DE8, kDisplayPtr = 0x0051FBD0;
constexpr u32 kDrawFrame = 0x00468CF0, kVisibleList = 0x0048BAE0, kMakeContext = 0x004B8A80;
constexpr u32 kRelocate = 0x004CB4C0, kNegateXZ = 0x004CB590, kBuildState = 0x0045A950, kFeatureState = 0x0045A8D0;
constexpr u32 kMalloc = 0x004D83B0, kGameAlloc = 0x004B4F10, kCacheAlloc = 0x00437A30;
constexpr u32 kWaterTable = 0x004BAF30, kGrayTable = 0x004BAD30;
constexpr u32 kBottomBar = 0x0046A860, kGui = 0x004AB170, kConsole = 0x00464060, kKeyHeld = 0x004C1B80, kUnitVisible = 0x00465AC0;
constexpr u32 kUnitSize = 0x118, kPlayerSize = 0x14B, kDefSize = 0x249, kCellSize = 13, kFeatureDefSize = 0x100, kSlotSize = 0x30;
constexpr u32 kPieceSize = 54, kStateHeader = 0x22;

// Game state offsets.
enum : u32 {
    gLocal = 0x2A42, gView = 0x2A43, gPlayers = 0x1B63, gHud = 0xDCB, gRowsBase = 0x141FB, gRowPtrs = 0x141FF, gRowCounts = 0x14203,
    gSlots = 0x1420B, gProxy = 0x1420F, gActive = 0x14213, gIdle = 0x14217, gFree = 0x1421B, gFogGrid = 0x1421F, gMapW = 0x14233,
    gMapH = 0x14237, gRowCap = 0x1424B, gRowCount = 0x1424F, gFeatureCount = 0x14253, gFeatureDefs = 0x1426F, gExplored = 0x14273,
    gSea = 0x1427F, gDebugGrid = 0x14280, gMapFlags = 0x14281, gTileInfo = 0x14283, gCells = 0x14287, gTileMap = 0x1428B,
    gScrollX = 0x1431F, gScrollY = 0x14323, gUnits = 0x14357, gUnitsEnd = 0x1435B, gVisible = 0x1435F, gVisibleCount = 0x14367,
    gModels = 0x14377, gUnitDefs = 0x1439B, gFogBlack = 0x1485F, gFogGray = 0x1486F, gBackCtx = 0x37E1B, gScreenW = 0x37E1F,
    gScreenH = 0x37E23, gViewRect = 0x37E27, gViewW = 0x37E37, gViewH = 0x37E3B, gTopBar = 0x37E3F, gVisual = 0x37F06,
    gGameOptions = 0x37F2F, gTick = 0x38A47, gSchema = 0x391E9,
    pInfo = 0x27, pCounts = 0x7C, pCountW = 0x80, pCountH = 0x84, pIndex = 0x146, pNextTurn = 0xF8,
};

void put32(std::vector<u8>& s, u32 v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<u8>(v >> (8 * i)));
}
template <class T> void put_at(std::vector<u8>& b, std::size_t off, const T& v) { std::memcpy(&b[off], &v, sizeof v); }
template <class T> T get_at(const std::vector<u8>& b, std::size_t off) {
    T v{};
    std::memcpy(&v, &b[off], sizeof v);
    return v;
}

// ---- The suspended original ----
struct Original {
    harness::Remote r;
    u32 heap = 0, heap_end = 0, bump_ptr = 0, stubs = 0, gs = 0, display = 0, ctx = 0, frame_hdr = 0, frame = 0;
    u32 key_state = 0, vis_table = 0, cache = 0, scratch = 0, rand_state = 0, probe = 0;
    int w = 0, h = 0;
    bool keep_bottom_bar = false;  // --hud: the original's bottom bar runs inside its frame
    u32 host_next = 0;  // our own allocations (above the original's bump allocator)
    std::map<std::string, u32> gafs;                 // GAF file -> remote copy
    std::map<const render::ModelTexture*, u32> texture_seqs;

    u32 alloc(u32 n) {
        const u32 at = host_next;
        host_next += (n + 15) & ~15u;
        return at;
    }
    bool start(const std::string& exe, int width, int height) {
        w = width;
        h = height;
        if (!r.start(exe)) return false;
        if (!r.install_crash_catcher()) std::printf("note: no crash catcher\n");
        heap = r.alloc(192u << 20);
        if (!heap) return false;
        heap_end = heap + (192u << 20);
        host_next = heap + (96u << 20);
        bump_ptr = r.alloc(0x1000);
        stubs = r.alloc(0x1000, PAGE_EXECUTE_READWRITE);
        gs = r.alloc(0x40000);
        display = r.alloc(0x1000);
        key_state = r.alloc(0x1000);
        vis_table = r.alloc(0x10000);
        if (!bump_ptr || !stubs || !gs || !display || !key_state || !vis_table) return false;
        // Bump allocators for the original's own allocations (malloc-with-name, the game allocator,
        // the sprite cache's block allocator), as render_harness.
        std::vector<u8> s;
        auto align16 = [&]() {
            while (s.size() % 16) s.push_back(0xCC);
        };
        const int size_at[2] = {8, 4};
        const u32 targets[2] = {kMalloc, kGameAlloc};
        for (int k = 0; k < 2; ++k) {
            const u32 at = stubs + static_cast<u32>(s.size());
            s.push_back(0xA1);
            put32(s, bump_ptr);
            s.insert(s.end(), {0x8B, 0x4C, 0x24, static_cast<u8>(size_at[k]), 0x83, 0xC1, 0x0F, 0x83, 0xE1, 0xF0, 0x01, 0x0D});
            put32(s, bump_ptr);
            s.push_back(0xC3);
            align16();
            if (!r.patch_jump(targets[k], at)) return false;
        }
        {
            const u32 at = stubs + static_cast<u32>(s.size());
            s.push_back(0xA1);
            put32(s, bump_ptr);
            s.insert(s.end(), {0x8B, 0x54, 0x24, 0x08, 0x83, 0xC2, 0x1F, 0x83, 0xE2, 0xF0});
            s.insert(s.end(), {0x01, 0x15});
            put32(s, bump_ptr);
            s.insert(s.end(), {0x83, 0xC0, 0x08});
            s.insert(s.end(), {0x8B, 0x54, 0x24, 0x04, 0x89, 0x02});
            s.insert(s.end(), {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x08, 0x00});
            align16();
            if (!r.patch_jump(kCacheAlloc, at)) return false;
        }
        // Interface-only parts of the frame: returns (stdcall: 1, 3 and 1 arguments).
        const std::pair<u32, u8> skipped[] = {{kBottomBar, 4}, {kGui, 12}, {kConsole, 4}};
        for (const auto& [fn, bytes] : skipped) {
            if (fn == kBottomBar && keep_bottom_bar) continue;  // --hud: the bottom bar is compared too
            (void)bytes;
            const u32 at = stubs + static_cast<u32>(s.size());
            s.insert(s.end(), {0xC2, bytes, 0x00});
            align16();
            if (!r.patch_jump(fn, at)) return false;
        }
        // Held keys (the original asks the keyboard): our table, indexed by the key code.
        {
            const u32 at = stubs + static_cast<u32>(s.size());
            s.insert(s.end(), {0x8B, 0x44, 0x24, 0x04, 0x25, 0xFF, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0x80});  // mov eax,[esp+4]; and eax,FF; movzx eax,byte [eax+tbl]
            put32(s, key_state);
            s.insert(s.end(), {0xC2, 0x04, 0x00});
            align16();
            if (!r.patch_jump(kKeyHeld, at)) return false;
        }
        // Unit visibility to the viewed player (verified by los_harness): our table by unit id.
        {
            const u32 at = stubs + static_cast<u32>(s.size());
            s.insert(s.end(), {0x8B, 0x44, 0x24, 0x08, 0x0F, 0xB7, 0x80, 0xA8, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0x80});  // mov eax,[esp+8]; movzx eax,word [eax+A8]; movzx eax,byte [eax+tbl]
            put32(s, vis_table);
            s.insert(s.end(), {0xC2, 0x08, 0x00});
            align16();
            if (!r.patch_jump(kUnitVisible, at)) return false;
        }
        // Debugging: FH_STUB=addr:bytes,... makes stdcall routines return at once.
        if (const char* e = std::getenv("FH_STUB")) {
            for (const char* q = e; *q;) {
                char* end = nullptr;
                const u32 fn = static_cast<u32>(std::strtoul(q, &end, 16));
                u8 pop = 0;
                if (end && *end == ':') pop = static_cast<u8>(std::strtoul(end + 1, &end, 10));
                const u32 at = stubs + static_cast<u32>(s.size());
                s.insert(s.end(), {0xC2, pop, 0x00});
                align16();
                r.patch_jump(fn, at);
                q = end && *end == ',' ? end + 1 : (end ? end : q + std::strlen(q));
                if (end && *end == 0) break;
            }
        }
        // Text translation (0x4C5740, stdcall(text)): English, every string stays as it is.
        {
            const u32 at = stubs + static_cast<u32>(s.size());
            s.insert(s.end(), {0x8B, 0x44, 0x24, 0x04, 0xC2, 0x04, 0x00});  // mov eax,[esp+4]; ret 4
            align16();
            if (!r.patch_jump(0x004C5740, at)) return false;
        }
        // rand() on a state we set (the C runtime is not initialised in the suspended process).
        rand_state = r.alloc(0x1000);
        {
            const u32 at = stubs + static_cast<u32>(s.size());
            s.push_back(0xA1);
            put32(s, rand_state);
            s.insert(s.end(), {0x69, 0xC0});
            put32(s, 214013u);
            s.push_back(0x05);
            put32(s, 2531011u);
            s.push_back(0xA3);
            put32(s, rand_state);
            s.insert(s.end(), {0xC1, 0xE8, 0x10, 0x25});
            put32(s, 0x7FFF);
            s.push_back(0xC3);
            align16();
            if (!r.patch_jump(0x004E4870, at)) return false;
        }
        // Observation: the stack words the shot drawing reads without setting (type 3's angles at
        // entry ESP - 0x40) are copied to `probe` on entry to 0x49BE60.
        probe = r.alloc(0x1000);
        {
            const u32 at = stubs + static_cast<u32>(s.size());
            s.insert(s.end(), {0x8B, 0x44, 0x24, 0xC0});  // mov eax,[esp-0x40]
            s.push_back(0xA3);
            put32(s, probe);
            s.insert(s.end(), {0x8B, 0x44, 0x24, 0xC4});  // mov eax,[esp-0x3C]
            s.push_back(0xA3);
            put32(s, probe + 4);
            s.insert(s.end(), {0x89, 0x25});  // mov [probe+8],esp
            put32(s, probe + 8);
            s.push_back(0xA1);  // mov eax,[watch count]
            put32(s, r.watch_count_address());
            s.push_back(0xA3);  // mov [probe+12],eax
            put32(s, probe + 12);
            s.insert(s.end(), {0x83, 0xEC, 0x68});  // sub esp,0x68
            s.push_back(0xA1);
            put32(s, kGameStatePtr);  // mov eax,[GameState]
            s.push_back(0xE9);
            const u32 next = stubs + static_cast<u32>(s.size()) + 4;
            put32(s, 0x0049BE68u - next);
            align16();
            if (!r.patch_jump(0x0049BE60, at)) return false;
        }
        if (!r.write(stubs, s.data(), s.size())) return false;
        r.w32(bump_ptr, heap);
        r.w32(kGameStatePtr, gs);
        r.w32(kDisplayPtr, display);
        return true;
    }
    std::string crash_text() {
        const harness::Remote::Crash c = r.crash_info();
        if (!c.code) return "no exception recorded";
        char buf[2048];
        int n = std::snprintf(buf, sizeof buf, "exception %08X at %08X (eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X esp %08X ebp %08X) stack:",
                              c.code, c.address, c.eax, c.ebx, c.ecx, c.edx, c.esi, c.edi, c.esp, c.ebp);
        for (int k = 0; k < 96 && n < 2000; ++k)
            n += std::snprintf(buf + n, sizeof buf - static_cast<std::size_t>(n), "%s%08X", k % 8 ? " " : "\n    ", c.stack[k]);
        return buf;
    }
    bool call(u32 fn, u32 self, const std::vector<u32>& args, u32* result = nullptr, DWORD timeout = 20000) {
        r.clear_crash_info();
        const bool ok = r.call(fn, self, args, result, timeout);
        if (!ok) std::printf("  original call %08X failed: %s\n", fn, crash_text().c_str());
        return ok;
    }

    // A GAF file copied in and its offsets turned into addresses (sequence frame tables, frame
    // data, layered frames' tables). Returns the copy's base.
    u32 gaf(const std::string& name, const std::vector<u8>& file) {
        auto it = gafs.find(name);
        if (it != gafs.end()) return it->second;
        const u32 base = alloc(static_cast<u32>(file.size()) + 16);
        std::vector<u8> d = file;
        std::set<u32> done;
        auto u32_at = [&](u32 off) { return off + 4 <= d.size() ? get_at<u32>(d, off) : 0u; };
        std::vector<u32> pending;
        auto frame = [&](u32 off) {
            pending.push_back(off);
            while (!pending.empty()) {
                const u32 f = pending.back();
                pending.pop_back();
                if (f + 0x18 > d.size() || !done.insert(f).second) continue;
                const u16 layers = get_at<u16>(d, f + 0xA);
                const u32 data = u32_at(f + 0x10);
                put_at<u32>(d, f + 0x10, base + data);
                if (layers)
                    for (u32 k = 0; k < layers; ++k) {
                        const u32 at = data + 4 * k;
                        if (at + 4 > d.size()) break;
                        const u32 sub = u32_at(at);
                        put_at<u32>(d, at, base + sub);
                        pending.push_back(sub);
                    }
            }
        };
        const u32 entries = u32_at(4);
        for (u32 e = 0; e < entries && 12 + 4 * e + 4 <= d.size(); ++e) {
            const u32 seq = u32_at(12 + 4 * e);
            if (seq + 0x28 > d.size()) continue;
            const u16 frames = get_at<u16>(d, seq);
            for (u32 k = 0; k < frames; ++k) {
                const u32 fe = seq + 0x28 + 8 * k;
                if (fe + 8 > d.size()) break;
                const u32 f = u32_at(fe);
                put_at<u32>(d, fe, base + f);
                frame(f);
            }
        }
        r.write(base, d.data(), d.size());
        gafs[name] = base;
        return base;
    }
    static u32 gaf_entry(const std::vector<u8>& file, int index) {
        if (file.size() < 12 + 4 * static_cast<std::size_t>(index) + 4) return 0;
        return get_at<u32>(file, 12 + 4 * static_cast<std::size_t>(index));
    }

    // Texture frames (sprites) as single-layer uncompressed GAF frames in a sequence.
    u32 texture_sequence(const render::ModelTexture* t) {
        auto it = texture_seqs.find(t);
        if (it != texture_seqs.end()) return it->second;
        const u32 seq = alloc(0x28 + static_cast<u32>(t->frames.size()) * 8 + 16);
        std::vector<u8> sq(0x28 + t->frames.size() * 8, 0);
        put_at<u16>(sq, 0, static_cast<u16>(t->frames.size()));
        for (std::size_t i = 0; i < t->frames.size(); ++i) {
            const render::Sprite8& f = t->frames[i];
            const u32 hdr = alloc(0x18 + static_cast<u32>(f.pixels.size()));
            std::vector<u8> fh(0x18 + f.pixels.size(), 0);
            put_at<u16>(fh, 0, static_cast<u16>(f.width));
            put_at<u16>(fh, 2, static_cast<u16>(f.height));
            put_at<i16>(fh, 4, static_cast<i16>(f.x_offset));
            put_at<i16>(fh, 6, static_cast<i16>(f.y_offset));
            fh[8] = f.key;
            put_at<u32>(fh, 0x10, hdr + 0x18);
            std::memcpy(&fh[0x18], f.pixels.data(), f.pixels.size());
            r.write(hdr, fh.data(), fh.size());
            put_at<u32>(sq, 0x28 + i * 8, hdr);
        }
        r.write(seq, sq.data(), sq.size());
        texture_seqs[t] = seq;
        return seq;
    }

    // A model file copied in, relocated, with the primitives' looks bound as the engine's model
    // has them (render_harness). Remote model tree nodes in the engine's piece order.
    static void walk(harness::Remote& r, u32 node, std::vector<u32>& order) {
        if (!node) return;
        order.push_back(node);
        walk(r, r.r32(node + 0x30), order);
        walk(r, r.r32(node + 0x2C), order);
    }
    u32 model(const std::vector<u8>& file, const render::Model& m, u32 cursor) {
        const u32 at = alloc(static_cast<u32>(file.size()) + 64);
        r.write(at, file.data(), file.size());
        if (!call(kRelocate, 0, {at, at}) || !call(kNegateXZ, at, {at})) return 0;
        std::vector<u32> nodes;
        walk(r, at, nodes);
        for (std::size_t i = 0; i < nodes.size() && i < m.pieces.size(); ++i) {
            const u32 prims = r.r32(nodes[i] + 0x28);
            const auto& mp = m.pieces[i].primitives;
            for (std::size_t k = 0; k < mp.size(); ++k) {
                const render::ModelPrimitive& p = mp[k];
                const u32 pa = prims + static_cast<u32>(k) * 0x20;
                u32 flags = 0;
                if (p.flat) {
                    flags = 1;
                    r.w32(pa, p.colour);
                } else if (p.texture && !p.texture->frames.empty()) {
                    const u32 seq = texture_sequence(p.texture);
                    flags = (p.texture->animated ? 2u : 0u) | (p.texture->team_coloured ? 4u : 0u);
                    r.w32(pa + 0x18, seq);
                    if (p.texture->animated) r.w32(pa + 0x10, cursor % static_cast<u32>(p.texture->frames.size()));
                    else r.w32(pa + 0x10, r.r32(seq + 0x28));
                }
                r.w32(pa + 0x1C, flags);
            }
        }
        return at;
    }
    // A script image copied in with its offsets made addresses (an empty one for no script).
    u32 script(const std::vector<u8>& cob_file) {
        std::vector<u8> img = cob_file;
        if (img.size() < 44) {
            img.assign(64, 0);
            for (int i = 6; i <= 10; ++i) put_at<u32>(img, static_cast<std::size_t>(i) * 4, 44);
        }
        const u32 cob = alloc(static_cast<u32>(img.size()) + 64);
        auto rel = [&](std::size_t off) { put_at<u32>(img, off, get_at<u32>(img, off) + cob); };
        auto hdr = [&](int i) { return get_at<u32>(img, static_cast<std::size_t>(i) * 4); };
        const u32 nfun = hdr(1), npieces = hdr(2), nsnd = hdr(5), names_off = hdr(7), pieces_off = hdr(8), snd_off = hdr(10);
        for (u32 i = 0; i < nfun; ++i) rel(names_off + i * 4);
        for (u32 i = 0; i < npieces; ++i) rel(pieces_off + i * 4);
        for (u32 i = 0; i < nsnd; ++i) rel(snd_off + i * 8 + 4);
        for (int i : {6, 7, 8, 9, 10}) rel(static_cast<std::size_t>(i) * 4);
        r.write(cob, img.data(), img.size());
        return cob;
    }
};

struct Options {
    int scenes = 6;
    unsigned seed = 1;
    std::string out_dir, map;
    int width = 640, height = 480;
    int steps = 300;      // steps between scenes
    int players = 4;
    int shots = 12;       // extra shots per scene (up to)
    bool fog = true;
    bool verbose = false;
    // --hud: the interface drawn over the view too: the selection boxes of selected units
    // (compared within the view) and the bottom bar of the unit under the pointer (compared
    // over the bottom bar's rows).
    bool hud = false;
};

struct Stats {
    int scenes = 0, identical = 0, failures = 0;
    long long pixels = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: frame_harness <TotalA.exe> <game_dir> [scenes] [seed] [out_dir] [--map NAME] [--res WxH] [--steps N] [--no-fog]\n");
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options opt;
    int positional = 0;
    for (int k = 3; k < argc; ++k) {
        const std::string a = argv[k];
        if (a == "--map" && k + 1 < argc) opt.map = argv[++k];
        else if (a == "--res" && k + 1 < argc) std::sscanf(argv[++k], "%dx%d", &opt.width, &opt.height);
        else if (a == "--steps" && k + 1 < argc) opt.steps = std::atoi(argv[++k]);
        else if (a == "--players" && k + 1 < argc) opt.players = std::atoi(argv[++k]);
        else if (a == "--no-fog") opt.fog = false;
        else if (a == "--shots" && k + 1 < argc) opt.shots = std::atoi(argv[++k]);
        else if (a == "--verbose") opt.verbose = true;
        else if (a == "--hud") opt.hud = true;
        else if (positional == 0) opt.scenes = std::atoi(argv[k]), ++positional;
        else if (positional == 1) opt.seed = static_cast<unsigned>(std::atoi(argv[k])), ++positional;
        else if (positional == 2) opt.out_dir = argv[k], ++positional;
    }
    std::mt19937 g(opt.seed);
    auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(g); };

    // ---- Engine: a skirmish between computer players, seen by player 0 ----
    static const char* kMaps[] = {"Moon Quartet", "Lava Highlands", "Coast To Coast", "Great Divide", "Painted Desert", "Ice Scream"};
    ta::SkirmishSetup setup;
    setup.map = opt.map.empty() ? kMaps[rnd(0, 5)] : opt.map;
    setup.players.clear();
    for (int p = 0; p < opt.players; ++p) setup.players.push_back({rnd(0, 1), true, -1});
    setup.human = 0;
    setup.line_of_sight = opt.fog;
    setup.mapping = opt.fog;
    setup.seed = static_cast<std::uint32_t>(g());
    ta::ClientWorld world;
    std::string err;
    if (!world.load(argv[2], setup, err)) {
        std::printf("engine load failed: %s\n", err.c_str());
        return 1;
    }
    sim::Battle& b = world.battle();
    client::ClientOptions co;
    co.width = opt.width;
    co.height = opt.height;
    co.player = 0;
    co.side = setup.players[0].side;
    co.fog = opt.fog;
    client::GameClient gc(b, world.content(), co);
    world.attach(&gc);
    std::printf("map %s, %d players, %dx%d, seed %u\n", setup.map.c_str(), opt.players, opt.width, opt.height, opt.seed);

    // ---- Original ----
    Original o;
    o.keep_bottom_bar = opt.hud;
    if (!o.start(argv[1], opt.width, opt.height)) {
        std::printf("cannot start the original\n");
        return 1;
    }
    harness::Remote& r = o.r;
    const u32 gs = o.gs;
    const int W = opt.width, H = opt.height;
    std::vector<u8> pal_bytes, shd, lht;
    ta::Vfs& vfs = world.vfs_;
    vfs.read("palettes\\PALETTE.PAL", pal_bytes);
    vfs.read("palettes\\PALETTE.SHD", shd);
    vfs.read("palettes\\PALETTE.LHT", lht);
    {
        // Display: mixing, shade, light, grey and water tables; table features on.
        const u32 alp = o.alloc(0x10000), shd_at = o.alloc(0x2000), lht_at = o.alloc(0x2000), gray = o.alloc(0x100), water = o.alloc(0x100),
                  pal = o.alloc(0x400);
        r.write(alp, world.blend_.data(), 0x10000);
        r.write(shd_at, shd.data(), std::min<std::size_t>(shd.size(), 0x2000));
        r.write(lht_at, lht.data(), std::min<std::size_t>(lht.size(), 0x2000));
        r.write(pal, pal_bytes.data(), std::min<std::size_t>(pal_bytes.size(), 0x400));
        r.w32(o.display + 0xC0, alp);
        r.w32(o.display + 0xC4, shd_at);
        r.w32(o.display + 0xC8, lht_at);
        r.w32(o.display + 0xCC, gray);
        r.w32(o.display + 0xD0, water);
        r.w32(o.display + 0xF0, 0x20 | 0x40 | 0x80 | 0x100 | 0x200);
        r.w32(o.display + 0x1CE, 1);  // no software pointer
        o.call(kWaterTable, 0, {pal});
        o.call(kGrayTable, 0, {pal});
        // Back buffer and its drawing context.
        o.frame = o.alloc(static_cast<u32>(W * H));
        o.frame_hdr = o.alloc(0x20);
        o.ctx = o.alloc(0x100);
        std::uint8_t fh[0x18] = {};
        const u16 w16 = static_cast<u16>(W), h16 = static_cast<u16>(H);
        std::memcpy(fh, &w16, 2);
        std::memcpy(fh + 2, &h16, 2);
        std::memcpy(fh + 0x10, &o.frame, 4);
        r.write(o.frame_hdr, fh, sizeof fh);
        o.call(kMakeContext, 0, {o.ctx, o.frame_hdr});
        // Sprite cache with its scratch frame (+0x10), as render_harness.
        o.cache = o.alloc(0x100);
        o.scratch = o.alloc(0x100);
        const u32 scratch_px = o.alloc(4u << 20), scratch_depth = o.alloc(4u << 20);
        r.w32(o.cache + 0x10, o.scratch);
        std::uint8_t sh[0x18] = {};
        sh[8] = 1;
        std::memcpy(sh + 0x10, &scratch_px, 4);
        std::memcpy(sh + 0x14, &scratch_depth, 4);
        r.write(o.scratch, sh, sizeof sh);
        r.w32(gs + 0x1437B, o.cache);
        // Screen and view.
        r.w32(gs + gBackCtx, o.ctx);
        r.w32(gs + gScreenW, static_cast<u32>(W));
        r.w32(gs + gScreenH, static_cast<u32>(H));
        r.w32(gs + gViewRect, 128);
        r.w32(gs + gViewRect + 4, 32);
        r.w32(gs + gViewRect + 8, static_cast<u32>(W - 1));
        r.w32(gs + gViewRect + 12, static_cast<u32>(H - 33));
        r.w32(gs + gViewW, static_cast<u32>(W - 128));
        r.w32(gs + gViewH, static_cast<u32>(H - 64));
        r.w32(gs + gSchema, o.alloc(16));
        r.write(gs + gHud, world.content().hud.data(), 256);
    }

    // Static map data: size, tiles, feature types, unit types (defs, models).
    const int mw = b.map.width, mh = b.map.height;
    r.w32(gs + gMapW, static_cast<u32>(mw));
    r.w32(gs + gMapH, static_cast<u32>(mh));
    const u32 cells = o.alloc(static_cast<u32>(mw * mh) * kCellSize);
    r.w32(gs + gCells, cells);
    {
        const ta::TntTiles& t = world.tiles_;
        const u32 map_at = o.alloc(static_cast<u32>(t.map.size()) * 2);
        r.write(map_at, t.map.data(), t.map.size() * 2);
        const u32 tiles_at = o.alloc(static_cast<u32>(t.tiles.size()) * 1024);
        for (std::size_t k = 0; k < t.tiles.size(); ++k) r.write(tiles_at + static_cast<u32>(k) * 1024, t.tiles[k].pixels.data(), 1024);
        const u32 info = o.alloc(16);
        r.w32(info + 4, tiles_at);
        r.w32(gs + gTileMap, map_at);
        r.w32(gs + gTileInfo, info);
    }
    // Sequences by the engine's sequence identity.
    ta::AnimImporter& anims = *world.lb_->anims;
    auto remote_seq = [&](std::uint32_t token) -> u32 {
        if (!token) return 0;
        const auto it = anims.origins().find(token);
        if (it == anims.origins().end()) return 0;
        const std::vector<u8>* file = anims.file(it->second.first);
        if (!file) return 0;
        return o.gaf(it->second.first, *file) + Original::gaf_entry(*file, it->second.second);
    };
    auto remote_cursor = [&](const std::uint8_t* c12) {
        std::vector<u8> c(c12, c12 + 12);
        put_at<u32>(c, 8, remote_seq(get_at<u32>(c, 8)));
        return c;
    };
    const auto& frecs = world.lb_->feature_defs.records();
    const u32 fdefs = o.alloc(static_cast<u32>(b.features.size()) * kFeatureDefSize + 16);
    r.w32(gs + gFeatureDefs, fdefs);
    r.w32(gs + gFeatureCount, static_cast<u32>(b.features.size()));
    // Feature models (3D): one remote model per model id.
    std::map<std::uint32_t, std::pair<u32, std::unique_ptr<ta::Model3doPiece>>> feature_models;
    std::map<std::uint32_t, render::Model> feature_render_models;
    auto feature_model = [&](std::uint32_t id) -> u32 {
        if (!id || id > world.feature_models_.size()) return 0;
        auto it = feature_models.find(id);
        if (it != feature_models.end()) return it->second.first;
        std::vector<u8> file;
        u32 at = 0;
        std::unique_ptr<ta::Model3doPiece> root;
        if (vfs.read("objects3d\\" + world.feature_models_[id - 1] + ".3DO", file)) {
            auto m = ta::load_3do(file, true);
            if (m.root) {
                root = std::move(m.root);
                feature_render_models[id] = ta::build_render_model(root.get(), world.textures_);
                at = o.model(file, feature_render_models[id], 0);
            }
        }
        feature_models[id] = {at, std::move(root)};
        return at;
    };
    // Unit types: defs (fields the drawing reads), models and scripts.
    const u32 udefs = o.alloc(static_cast<u32>(world.types_.size() + 1) * kDefSize);
    r.w32(gs + gUnitDefs, udefs);
    const u32 models_tbl = o.alloc(static_cast<u32>(world.types_.size() + 1) * 4);
    r.w32(gs + gModels, models_tbl);
    std::vector<u32> type_model(world.types_.size(), 0), type_cob(world.types_.size(), 0);
    for (std::size_t k = 0; k < world.types_.size() && k < world.defs_.units.size(); ++k) {
        const ta::UnitDefRecord& d = world.defs_.units[k];
        std::vector<u8> db(kDefSize, 0);
        put_at<i32>(db, 0x15E, d.model_min_x);
        put_at<i32>(db, 0x162, d.model_min_y);
        put_at<i32>(db, 0x166, d.model_min_z);
        put_at<i32>(db, 0x16A, d.model_max_x);
        put_at<i32>(db, 0x16E, d.model_max_y);
        put_at<i32>(db, 0x172, d.model_max_z);
        put_at<u32>(db, 0x1FA, d.max_damage);
        put_at<u32>(db, 0x241, d.type_flags);
        r.write(udefs + static_cast<u32>(k) * kDefSize, db.data(), db.size());
    }
    auto unit_type = [&](std::size_t k) -> bool {
        if (type_model[k]) return true;
        const ta::ClientUnitType* t = world.types_[k].get();
        if (!t || !t->ok || !t->assets) return false;
        type_model[k] = o.model(t->assets->model_file, t->model, 0);
        type_cob[k] = o.script(t->assets->has_script ? t->assets->cob_file : std::vector<u8>());
        r.w32(models_tbl + static_cast<u32>(k) * 4, type_model[k]);
        return type_model[k] != 0;
    };
    // Weapons (fields the shot drawing reads) with their models.
    const u32 wdefs = o.alloc(256 * 0x115);
    {
        std::map<std::string, u32> wmodels;
        auto upper = [](std::string s) {
            for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            return s;
        };
        for (int slot = 0; slot < 256; ++slot) {
            const ta::WeaponDefRecord& d = world.weapons_.slots[static_cast<std::size_t>(slot)];
            std::vector<u8> w(0x115, 0);
            std::string name = d.model_name;
            if (name.empty() && d.model_slot >= 0 && d.model_slot < 256) name = world.weapons_.slots[static_cast<std::size_t>(d.model_slot)].model_name;
            u32 model = 0;
            if (!name.empty()) {
                auto it = wmodels.find(upper(name));
                if (it != wmodels.end()) {
                    model = it->second;
                } else {
                    const auto wm = world.weapon_models_.find(upper(name));
                    if (wm != world.weapon_models_.end() && wm->second && wm->second->root) model = o.model(wm->second->file, wm->second->model, 0);
                    wmodels[upper(name)] = model;
                }
            }
            put_at<u32>(w, 0x74, model);
            put_at<i16>(w, 0xE6, d.weapon_timer);
            w[0x10A] = static_cast<u8>(slot);
            w[0x10C] = d.render_type;
            w[0x10D] = d.color;
            w[0x10E] = d.color2;
            put_at<u32>(w, 0x111, d.flags);
            r.write(wdefs + static_cast<u32>(slot) * 0x115, w.data(), w.size());
        }
    }
    if (opt.verbose) {
        std::map<int, std::string> by_type;
        for (int slot = 0; slot < 256; ++slot) {
            const ta::WeaponDefRecord& d = world.weapons_.slots[static_cast<std::size_t>(slot)];
            if (d.name.empty()) continue;
            std::string& s = by_type[d.render_type];
            if (s.size() < 300) s += " " + d.name;
        }
        for (const auto& [t, s] : by_type) std::printf("render type %d:%s\n", t, s.c_str());
    }
    const u32 proj_at = o.alloc(300 * 0x6B + 16);
    r.w32(gs + 0x141F7, proj_at);
    // Scratch arrays of the projected-model routine (0x46BAE0): points, screen points, polygon.
    r.w32(gs + 0x14383, o.alloc(0x10000));
    r.w32(gs + 0x14387, o.alloc(0x10000));
    r.w32(gs + 0x1438B, o.alloc(0x10000));
    // Players: records, info blocks (team colour), sight count maps; explored map.
    const u32 infos = o.alloc(10 * 0x100);
    const int bw = b.sight.width, bh = b.sight.height;
    std::vector<u32> counts_at(10, 0);
    for (int p = 0; p < 10; ++p) {
        const u32 rec = gs + gPlayers + static_cast<u32>(p) * kPlayerSize;
        r.w32(rec + pInfo, infos + static_cast<u32>(p) * 0x100);
        const u8 colour = world.request().start.players[static_cast<std::size_t>(p)].colour;
        r.write(infos + static_cast<u32>(p) * 0x100 + 0x96, &colour, 1);
        const u8 index = static_cast<u8>(p);
        r.write(rec + pIndex, &index, 1);
        r.w32(rec + pNextTurn, 0xFFFFFFFFu);
        counts_at[static_cast<std::size_t>(p)] = o.alloc(static_cast<u32>(bw * bh + 8));
        r.w32(rec + pCounts, counts_at[static_cast<std::size_t>(p)]);
        r.w32(rec + pCountW, static_cast<u32>(bw));
        r.w32(rec + pCountH, static_cast<u32>(bh));
    }
    const u32 explored_at = o.alloc(static_cast<u32>(bw * bh) * 2 + 16);
    r.w32(gs + gExplored, explored_at);
    // Fog: the edge art and the grid.
    {
        const std::vector<u8>* fog = anims.file("fog");
        if (!fog) fog = anims.file("FOG");
        std::vector<u8> fog_file;
        if (!fog && vfs.read("anims\\fog.gaf", fog_file)) fog = &fog_file;
        if (fog) {
            ta::GafDirectory dir;
            ta::gaf_read_directory(*fog, dir);
            const u32 base = o.gaf("anims\\fog.gaf", *fog);
            for (int k = 0; k < 4; ++k) {
                const int bi = ta::gaf_find_sequence(dir, "Black" + std::to_string(k + 1));
                const int gi = ta::gaf_find_sequence(dir, "Gray" + std::to_string(k + 1));
                r.w32(gs + gFogBlack + 4 * static_cast<u32>(k), bi >= 0 ? base + Original::gaf_entry(*fog, bi) : 0);
                r.w32(gs + gFogGray + 4 * static_cast<u32>(k), gi >= 0 ? base + Original::gaf_entry(*fog, gi) : 0);
            }
        }
        const int vw = W - 128, vh = H - 64;
        const u32 gw = static_cast<u32>(vw / 32 + ((vw & 31) ? 3 : 2)), gh = static_cast<u32>(vh / 32 + ((vh & 31) ? 3 : 2));
        const u32 grid = o.alloc(16), cellsb = o.alloc(gw * gh * 2 + 16);
        r.w32(grid, cellsb);
        r.w32(grid + 4, gw);
        r.w32(grid + 8, gh);
        r.w32(gs + gFogGrid, grid);
    }
    // The effect art the loader (0x429870) finds by name in anims\fx.gaf and the in-game titles.
    {
        auto file_seq = [&](const std::string& path, const std::string& seq) -> u32 {
            static std::map<std::string, std::vector<u8>> files;
            std::vector<u8>& f = files[path];
            if (f.empty() && !vfs.read(path, f)) return 0;
            ta::GafDirectory dir;
            if (!ta::gaf_read_directory(f, dir)) return 0;
            const int i = ta::gaf_find_sequence(dir, seq);
            return i < 0 ? 0 : o.gaf(path, f) + Original::gaf_entry(f, i);
        };
        const std::pair<u32, const char*> fx[] = {
            {0x147CF, "smoke 1"}, {0x147D3, "smoke 2"}, {0x147D7, "fire1"}, {0x147DB, "alfboom1"}, {0x147DF, "radlogo"},
            {0x147E3, "radlogohigh"}, {0x147E7, "nuclogo"}, {0x147EB, "h2oboom2"}, {0x147EF, "lavasplash"}, {0x147BB, "cannonshell"},
            {0x147BF, "plasmasm"}, {0x147C3, "plasmamd"}, {0x147C7, "ultrashell"}, {0x147CB, "plasmasm"}, {0x147F3, "flamestream"},
            {0x147F7, "explosion"}, {0x147FB, "explode2"}, {0x147FF, "explode3"}, {0x14803, "explode4"}, {0x14807, "explode5"},
            {0x1480B, "nuke1"}, {0x1480F, "shadow"}};
        for (const auto& [off, name] : fx) r.w32(gs + off, file_seq("anims\\FX.GAF", name));
        r.w32(gs + 0x14813, file_seq("anims\\igtitles.gaf", "igvictory"));
        r.w32(gs + 0x14817, file_seq("anims\\igtitles.gaf", "igdefeat"));
        r.w32(gs + 0x1481B, file_seq("anims\\igtitles.gaf", "igpaused"));
    }
    // Effect particle lists (10 x {?, begin, end, capacity}): empty.
    const u32 fx_lists = o.alloc(10 * 16);
    {
        const u32 empty = o.alloc(16);
        for (u32 k = 0; k < 10; ++k) {
            r.w32(fx_lists + 16 * k + 4, empty);
            r.w32(fx_lists + 16 * k + 8, empty);
            r.w32(fx_lists + 16 * k + 12, empty);
        }
        r.w32(gs + 0x38D77, fx_lists);
    }
    // Row buckets of the frame draw.
    {
        const u32 rows = static_cast<u32>((H - 64) / 16 + 32), cap = 2048;
        const u32 base = o.alloc(rows * cap * 4), ptrs = o.alloc(rows * 4), counts = o.alloc(rows * 2);
        r.w32(gs + gRowsBase, base);
        r.w32(gs + gRowPtrs, ptrs);
        r.w32(gs + gRowCounts, counts);
        r.w32(gs + gRowCap, cap);
        r.w32(gs + gRowCount, rows);
    }
    // Unit table, visible list, feature slots and the proxy unit of 3D features.
    const u32 units_at = o.alloc(static_cast<u32>(b.units.size()) * kUnitSize);
    r.w32(gs + gUnits, units_at);
    r.w32(gs + gUnitsEnd, units_at + static_cast<u32>(b.units.size() - 1) * kUnitSize);
    r.w32(gs + gVisible, o.alloc(static_cast<u32>(b.units.size()) * 2 + 16));
    const u32 slots_at = o.alloc(2048 * kSlotSize);
    r.w32(gs + gSlots, slots_at);
    const u32 proxy = o.alloc(kUnitSize);
    {
        std::vector<u8> pb(kUnitSize, 0);
        put_at<u32>(pb, 0x110, 0x200u | 0x20000000u);
        put_at<u32>(pb, 0x114, 1u);
        put_at<u32>(pb, 0x92, udefs);
        r.write(proxy, pb.data(), pb.size());
        r.w32(gs + gProxy, proxy);
    }
    const u32 motions = o.alloc(static_cast<u32>(b.units.size()) * 0x40);
    std::vector<u32> unit_state(b.units.size(), 0);
    std::vector<u16> unit_state_type(b.units.size(), 0);

    // --hud: what the original's bottom bar (0x46A860) reads besides the units.
    const u32 head_orders = o.alloc(static_cast<u32>(b.units.size()) * 0x58);
    if (opt.hud) {
        // The C runtime's floating-point set-up (its start-up never ran here): sprintf's "%f"
        // goes through conversion routines it installs.
        o.call(0x004E4320, 0, {1});
        // The display's screen size (the bars place their art by it), and the screen height the
        // side data's rows are for (sidedata.tdf [GENERAL] baseheight, 480 when absent: the
        // loading's default).
        r.w32(o.display + 0xD4, static_cast<u32>(W));
        r.w32(o.display + 0xD8, static_cast<u32>(H));
        r.w32(gs + 0x147A7, 480);
        // Type names and descriptions are read from the definitions (+0 name, +0x40 description).
        for (std::size_t k = 0; k < world.types_.size() && k < world.defs_.units.size(); ++k) {
            const ta::UnitDefRecord& d = world.defs_.units[k];
            char nm[0x20] = {}, ds[0x40] = {};
            std::strncpy(nm, d.name.c_str(), sizeof nm - 1);
            std::strncpy(ds, d.description.c_str(), sizeof ds - 1);
            r.write(udefs + static_cast<u32>(k) * kDefSize, nm, sizeof nm);
            r.write(udefs + static_cast<u32>(k) * kDefSize + 0x40, ds, sizeof ds);
        }
        // The mission table (0x00512344, 0x19-byte entries; the bar reads each entry's text at
        // +0): built from the executable's own static entries, found by their lookup names.
        {
            const std::vector<std::string> kinds = ta::UnitDefContext::default_mission_names();
            std::vector<u8> img(0x6000);
            r.read(0x004FB000, img.data(), img.size());
            std::map<std::string, u32> text_of;
            for (std::size_t off = 0; off + 0x19 <= img.size(); ++off) {
                const u32 text = get_at<u32>(img, off), look = get_at<u32>(img, off + 0x15), fn = get_at<u32>(img, off + 4);
                if (text < 0x00500000 || text >= 0x00520000 || look < 0x00500000 || look >= 0x00520000 || fn < 0x00401000 || fn >= 0x004E0000) continue;
                char name[40] = {};
                r.read(look, name, sizeof name - 1);
                if (name[0]) text_of.emplace(name, text);
            }
            const u32 table = o.alloc(static_cast<u32>(kinds.size()) * 0x19 + 16);
            std::vector<u8> tb(kinds.size() * 0x19, 0);
            put_at<u32>(tb, 0, r.r32(0x004FD288));  // the first entry (Ready, no lookup name)
            int found = 0;
            for (std::size_t k = 1; k < kinds.size(); ++k)
                if (auto it = text_of.find(kinds[k]); it != text_of.end()) {
                    put_at<u32>(tb, k * 0x19, it->second);
                    ++found;
                }
            r.write(table, tb.data(), tb.size());
            r.w32(0x00512344, table);
            r.w32(0x00512348, table + static_cast<u32>(tb.size()));
            std::printf("hud: mission table %d of %zu texts found\n", found + 1, kinds.size());
        }
        // Each side's bottom-bar regions, console font and bar art (sidedata.tdf).
        std::vector<u8> sd;
        vfs.read("gamedata\\sidedata.tdf", sd);
        const ta::TdfParseResult sr = ta::tdf_parse(std::string_view(reinterpret_cast<const char*>(sd.data()), sd.size()));
        for (std::size_t side = 0; side < world.content().sides.size() && side < 8; ++side) {
            const client::SideLayout& L = world.content().sides[side];
            const u32 rec = gs + static_cast<u32>(side) * 0x232 + 0x37F87;
            auto region = [&](u32 dword_index, const client::Region& g) {
                const i32 v[4] = {g.x1, g.y1, g.x2, g.y2};
                r.write(rec + dword_index * 4, v, sizeof v);
            };
            region(0x3A, L.logo2);
            region(0x3E, L.unit_name);
            region(0x42, L.damage_bar);
            region(0x46, L.unit_energy_make);
            region(0x4A, L.unit_energy_use);
            region(0x4E, L.unit_metal_make);
            region(0x52, L.unit_metal_use);
            region(0x56, L.mission_text);
            region(0x5A, L.unit_name2);
            region(0x5E, L.damage_bar2);
            region(0x62, L.name);
            region(0x66, L.description);
            const ta::TdfSection* s = sr.root ? sr.root->child("SIDE" + std::to_string(side)) : nullptr;
            if (!s) continue;
            std::vector<u8> fnt;
            if (vfs.read("fonts\\" + s->get_string("font", 256, "console") + ".FNT", fnt)) {
                const u32 at = o.alloc(static_cast<u32>(fnt.size()) + 16);
                r.write(at, fnt.data(), fnt.size());
                r.w32(gs + static_cast<u32>(side) * 0x232 + 0x3816B, at);
            }
            const std::string intgaf = "anims\\" + s->get_string("intgaf", 30, "") + ".GAF";
            std::vector<u8> gf;
            if (vfs.read(intgaf, gf)) {
                ta::GafDirectory dir;
                ta::gaf_read_directory(gf, dir);
                const int i = ta::gaf_find_sequence(dir, "PANELBOT");
                if (i >= 0) r.w32(gs + static_cast<u32>(side) * 4 + 0x14833, o.gaf(intgaf, gf) + Original::gaf_entry(gf, i));
            }
        }
        // The colour logos (textures\logos.gaf "32xlogos", frame = colour) of the LOGO boxes.
        {
            std::vector<u8> lg;
            if (vfs.read("textures\\logos.gaf", lg)) {
                ta::GafDirectory dir;
                ta::gaf_read_directory(lg, dir);
                const int i = ta::gaf_find_sequence(dir, "32xlogos");
                if (i >= 0) r.w32(gs + 0x148DB, o.gaf("textures\\logos.gaf", lg) + Original::gaf_entry(lg, i));
            }
            std::printf("hud: colour logos %s\n", r.r32(gs + 0x148DB) ? "loaded" : "missing");
        }
        for (int p = 0; p < 10; ++p) {
            const u8 side = static_cast<u8>(p < static_cast<int>(setup.players.size()) ? setup.players[static_cast<std::size_t>(p)].side : 0);
            r.write(infos + static_cast<u32>(p) * 0x100 + 0x95, &side, 1);
        }
        gc.set_pointer_hidden(true);
    }

    Stats st;
    int written = 0;
    for (int sc = 0; sc < opt.scenes; ++sc) {
        // Run the battle on.
        const int steps = rnd(opt.steps / 2, opt.steps * 3 / 2);
        for (int k = 0; k < steps; ++k) {
            b.step();
            b.end_frame();
        }
        if (!b.first_fault().empty()) std::printf("engine fault: %s\n", b.first_fault().c_str());
        // The view: over a random unit (or a random place).
        int sx = 0, sy = 0;
        std::vector<int> alive;
        for (std::size_t i = 1; i < b.units.size(); ++i)
            if (b.ticks[i].type_index) alive.push_back(static_cast<int>(i));
        if (!alive.empty() && rnd(0, 4)) {
            const sim::Unit& u = b.units[static_cast<std::size_t>(alive[static_cast<std::size_t>(rnd(0, static_cast<int>(alive.size()) - 1))])];
            sx = (u.pos[0] >> 16) - (W - 128) / 2 + rnd(-100, 100);
            sy = (u.pos[2] >> 16) - (u.pos[1] >> 17) - (H - 64) / 2 + rnd(-100, 100);
        } else {
            sx = rnd(0, mw * 16);
            sy = rnd(0, mh * 16);
        }
        gc.set_scroll(sx, sy);
        sx = gc.scroll_x();
        sy = gc.scroll_y();
        // --hud: some units selected (any owner), and the pointer put on a unit of the view (the
        // client's own pick test decides which unit it is over; the original is told that one).
        std::uint16_t hover = 0;
        if (opt.hud) {
            for (std::size_t i = 1; i < b.units.size(); ++i)
                if (b.ticks[i].type_index) {
                    if (rnd(0, 2) == 0) b.units[i].flags |= 0x10u;
                    else b.units[i].flags &= ~0x10u;
                }
            (void)gc.draw();  // the units on screen (the pick test uses them)
            std::vector<std::pair<int, int>> spots;
            for (std::size_t i = 1; i < b.units.size(); ++i) {
                if (!b.ticks[i].type_index) continue;
                const sim::Unit& u = b.units[i];
                const int px = static_cast<std::int16_t>(static_cast<std::uint32_t>(u.pos[0]) >> 16) - sx + 128;
                const int py = static_cast<std::int16_t>(static_cast<std::uint32_t>(u.pos[2]) >> 16) -
                               (static_cast<std::int16_t>(static_cast<std::uint32_t>(u.pos[1]) >> 16) >> 1) - sy + 32;
                if (px > 128 && px < W - 1 && py > 32 && py < H - 33) spots.push_back({px, py});
            }
            client::InputEvent mv;
            mv.type = client::InputEvent::Type::MouseMove;
            if (!spots.empty() && rnd(0, 5)) {
                const auto& sp = spots[static_cast<std::size_t>(rnd(0, static_cast<int>(spots.size()) - 1))];
                mv.x = sp.first + rnd(-2, 2);
                mv.y = sp.second + rnd(-2, 2);
            } else {
                mv.x = rnd(129, W - 2);
                mv.y = rnd(33, H - 34);
            }
            gc.handle(mv);
            hover = gc.hover_unit();
        }
        const u8 view = 0;
        u16 visual = 0x02 | 0x04 | 0x08 | 0x10 | 0x20;  // anti-aliasing, shadows, unit and feature shadows, shading

        // Engine frame.
        // Extra shots of random weapons around the view (coverage of every render type); the
        // battle's own table is put back after the scene.
        const sim::ProjectileTable saved_shots = b.projectiles;
        {
            std::vector<int> armed;
            for (int slot = 0; slot < 256; ++slot)
                if (!world.weapons_.slots[static_cast<std::size_t>(slot)].name.empty() && static_cast<std::size_t>(slot) < world.lb_->weapons.size())
                    armed.push_back(slot);
            const int extra = armed.empty() ? 0 : rnd(0, opt.shots);
            for (int k = 0; k < extra && b.projectiles.count < sim::kMaxProjectiles; ++k) {
                sim::Projectile& p = b.projectiles.slots[static_cast<std::size_t>(b.projectiles.count++)];
                p = sim::Projectile();
                p.weapon = &world.lb_->weapons[static_cast<std::size_t>(armed[static_cast<std::size_t>(rnd(0, static_cast<int>(armed.size()) - 1))])];
                const int wx = sx + rnd(-40, W - 128 + 40), wz = sy + rnd(-40, H - 64 + 40);
                const int gh = std::max(0, world.hm_.sample(static_cast<std::int16_t>(wx), static_cast<std::int16_t>(wz)));
                p.pos = {(wx << 16) | rnd(0, 0xFFFF), ((gh + rnd(0, 80)) << 16) | rnd(0, 0xFFFF), (wz << 16) | rnd(0, 0xFFFF)};
                p.trail = {p.pos[0] + (rnd(-150, 150) << 16), p.pos[1] + (rnd(-60, 60) << 16), p.pos[2] + (rnd(-150, 150) << 16)};
                p.tumble = static_cast<std::int16_t>(rnd(-32768, 32767));
                p.yaw = static_cast<std::uint16_t>(rnd(0, 65535));
                p.elevation = static_cast<std::uint16_t>(rnd(0, 65535));
                p.propeller = static_cast<std::int16_t>(rnd(-32768, 32767));
                p.spawn_tick = b.game_tick() - static_cast<std::uint32_t>(rnd(0, 40));
                p.expire_tick = b.game_tick() + static_cast<std::uint32_t>(rnd(-5, 40));
                p.ground_height = static_cast<std::int16_t>(gh);
            }
        }
        // Engine frame: the client's battle view (its scene and draw list; the interface around and
        // over it belongs to the front end and is not compared).
        render::BattleScene scene;
        scene.camera.screen_width = W;
        scene.camera.screen_height = H;
        scene.camera.view_width = W - 128;
        scene.camera.view_height = H - 64;
        scene.camera.scroll_x = sx;
        scene.camera.scroll_y = sy;
        world.fill_scene(scene, view, 0);
        if (!opt.fog) scene.fog = nullptr;
        if (opt.hud) gc.add_selection_outlines(scene);
        render::DrawList list = render::build_draw_list(scene);
        {
            // The same background pattern under the frame (only the interface's area keeps it).
            render::Sprite8 bgs;
            bgs.width = W;
            bgs.height = H;
            bgs.has_key = false;
            bgs.pixels.resize(static_cast<std::size_t>(W * H));
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) bgs.pixels[static_cast<std::size_t>(y * W + x)] = static_cast<u8>((x / 8 + y / 8) & 1 ? 0xC5 : 0x50);
            list.cmds.insert(list.cmds.begin(), render::DrawCmd{render::DrawOp::Blit, list.keep(std::move(bgs)), false, 0, 0, 0, 0, 0, {0, 0, W - 1, H - 1}});
        }
        const render::Image8 mine = render::execute(list, world.blend_);

        // ---- Original state ----
        r.w32(gs + gTick, b.game_tick());
        r.write(gs + gLocal, &view, 1);
        r.write(gs + gView, &view, 1);
        r.w32(gs + gScrollX, static_cast<u32>(sx));
        r.w32(gs + gScrollY, static_cast<u32>(sy));
        r.write(gs + gVisual, &visual, 2);
        {
            std::vector<u8> top(33, 0);
            top[0] = view;
            r.write(gs + gTopBar, top.data(), top.size());
        }
        const u8 sea = b.settings.sea_level;
        r.write(gs + gSea, &sea, 1);
        u16 mflags = static_cast<u16>(b.sight.flags & ~sim::sight_flags::kViewClean);
        r.write(gs + gMapFlags, &mflags, 2);
        // Cells.
        {
            std::vector<u8> cb(static_cast<std::size_t>(mw * mh) * kCellSize, 0);
            for (std::size_t i = 0; i < b.map.cells.size(); ++i) {
                const sim::MapCell& c = b.map.cells[i];
                u8* d = &cb[i * kCellSize];
                std::memcpy(d, &c.unit, 2);
                std::memcpy(d + 2, &c.unit2, 2);
                d[4] = c.height;
                d[5] = c.height_max;
                d[6] = c.height_min;
                d[7] = c.metal;
                std::memcpy(d + 8, &c.feature, 2);
                std::memcpy(d + 10, &c.feature_aux, 2);
                d[12] = c.flags;
            }
            r.write(cells, cb.data(), cb.size());
        }
        // Feature types (with their type-wide cursors) and slots.
        {
            std::vector<u8> db(b.features.size() * kFeatureDefSize, 0);
            for (std::size_t k = 0; k < b.features.size(); ++k) {
                const sim::FeatureTypeInfo& t = b.features[k];
                u8* p = &db[k * kFeatureDefSize];
                std::memcpy(p + 0x94, &t.footprint_x, 2);
                std::memcpy(p + 0x96, &t.footprint_z, 2);
                for (int s = 0; s < 8; ++s) {
                    const u32 a = remote_seq(t.sequences[static_cast<std::size_t>(s)]);
                    std::memcpy(p + 0xAC + 4 * s, &a, 4);
                }
                for (int c = 0; c < 2; ++c) {
                    const sim::AnimCursor& a = b.feature_state.type_cursors[k][static_cast<std::size_t>(c)];
                    std::uint8_t cur[12];
                    std::memcpy(cur, &a, 12);
                    const std::vector<u8> rc = remote_cursor(cur);
                    std::memcpy(p + 0xCC + 12 * c, rc.data(), 12);
                }
                p[0xFA] = t.height;
                const u16 fl = k < frecs.size() ? frecs[k].flags : t.flags;
                std::memcpy(p + 0xFE, &fl, 2);
            }
            r.write(fdefs, db.data(), db.size());
            std::vector<u8> sb(2048 * kSlotSize, 0);
            for (int i = 0; i < 2048; ++i) std::memcpy(&sb[static_cast<std::size_t>(i) * kSlotSize], b.feature_state.slots[static_cast<std::size_t>(i)].b.data(), kSlotSize);
            for (const sim::MapCell& c : b.map.cells) {
                if (!(c.flags & 1) || c.feature >= b.features.size()) continue;
                const std::size_t si = c.feature_aux % 2048;
                u8* p = &sb[si * kSlotSize];
                const sim::FeatureTypeInfo& t = b.features[c.feature];
                if (!(t.flags & sim::feature_type_flags::kFlat)) {
                    const u32 m = feature_model(t.model);
                    u32 state = 0;
                    if (m) o.call(kFeatureState, 0, {m}, &state);
                    std::memcpy(p + 4, &state, 4);
                } else {
                    for (int off : {4, 0x10}) {
                        const std::vector<u8> rc = remote_cursor(p + off);
                        std::memcpy(p + off, rc.data(), 12);
                    }
                }
            }
            r.write(slots_at, sb.data(), sb.size());
        }
        // Sight: the viewed player's counts and the explored map; unit visibility table.
        for (int p = 0; p < 10; ++p) {
            const auto& c = b.sight.players[static_cast<std::size_t>(p)].counts;
            if (!c.empty()) r.write(counts_at[static_cast<std::size_t>(p)], c.data(), std::min<std::size_t>(c.size(), static_cast<std::size_t>(bw * bh)));
        }
        if (!b.explored.empty()) r.write(explored_at, b.explored.data(), std::min<std::size_t>(b.explored.size(), static_cast<std::size_t>(bw * bh)) * 2);
        {
            std::vector<u8> vis(0x10000, 0);
            for (std::size_t i = 1; i < b.units.size(); ++i)
                if (b.ticks[i].type_index) vis[b.units[i].id] = b.unit_seen_by(view, b.units[i]) ? 1 : 0;
            r.write(o.vis_table, vis.data(), vis.size());
        }
        // Shots (0x6B bytes each) and the C runtime's random state.
        {
            std::vector<u8> pt(300 * 0x6B, 0);
            auto unit_ptr = [&](const sim::Unit* u) { return u ? units_at + static_cast<u32>(u->id) * kUnitSize : 0u; };
            for (i32 s = 0; s < b.projectiles.count && s < 300; ++s) {
                const sim::Projectile& p = b.projectiles.slots[static_cast<std::size_t>(s)];
                const std::size_t o0 = static_cast<std::size_t>(s) * 0x6B;
                put_at<u32>(pt, o0, p.weapon ? wdefs + static_cast<u32>(p.weapon->index) * 0x115 : 0u);
                for (int k = 0; k < 3; ++k) {
                    put_at<i32>(pt, o0 + 4 + 4 * static_cast<std::size_t>(k), p.pos[static_cast<std::size_t>(k)]);
                    put_at<i32>(pt, o0 + 0x10 + 4 * static_cast<std::size_t>(k), p.trail[static_cast<std::size_t>(k)]);
                    put_at<i32>(pt, o0 + 0x1C + 4 * static_cast<std::size_t>(k), p.vel[static_cast<std::size_t>(k)]);
                    put_at<i32>(pt, o0 + 0x28 + 4 * static_cast<std::size_t>(k), p.aim[static_cast<std::size_t>(k)]);
                }
                put_at<i16>(pt, o0 + 0x34, p.tumble);
                put_at<u16>(pt, o0 + 0x36, p.yaw);
                put_at<u16>(pt, o0 + 0x38, p.elevation);
                put_at<i32>(pt, o0 + 0x3A, p.speed);
                put_at<i32>(pt, o0 + 0x3E, p.distance);
                put_at<u32>(pt, o0 + 0x42, p.spawn_tick);
                put_at<u32>(pt, o0 + 0x46, p.expire_tick);
                put_at<u32>(pt, o0 + 0x4A, p.next_smoke_tick);
                put_at<u32>(pt, o0 + 0x4E, unit_ptr(p.target));
                put_at<u32>(pt, o0 + 0x52, unit_ptr(p.owner));
                put_at<u32>(pt, o0 + 0x56, p.intercepted >= 0 ? proj_at + static_cast<u32>(p.intercepted) * 0x6B : 0u);
                put_at<i16>(pt, o0 + 0x5A, p.last_feature_x);
                put_at<i16>(pt, o0 + 0x5C, p.last_feature_z);
                put_at<i16>(pt, o0 + 0x5E, p.ground_height);
                put_at<u16>(pt, o0 + 0x60, p.burst_left);
                put_at<i16>(pt, o0 + 0x62, p.piece);
                put_at<i16>(pt, o0 + 0x64, p.propeller);
                pt[o0 + 0x66] = p.owner_slot;
                put_at<i16>(pt, o0 + 0x67, p.compact_index);
                put_at<u16>(pt, o0 + 0x69, p.flags);
            }
            r.write(proj_at, pt.data(), pt.size());
            r.w32(gs + 0x141F3, static_cast<u32>(b.projectiles.count));
            r.w32(o.rand_state, b.crt_rand.state());
        }
        // Effect list 6: the nano streams (objects of vtable 0x4FD5B8: +4 deadline, +8 next
        // emission, +0x10/+0x14 particle array of 0x30-byte particles {pos, target, velocity, flags,
        // colour, expiry}).
        {
            const auto& streams = b.nano.streams;
            const u32 ptrs = o.alloc(static_cast<u32>(streams.size()) * 4 + 16);
            for (std::size_t k = 0; k < streams.size(); ++k) {
                const sim::NanoStream& ns = streams[k];
                const u32 obj = o.alloc(0x50);
                const u32 parts = o.alloc(static_cast<u32>(ns.particles.size()) * 0x30 + 16);
                std::vector<u8> ob(0x4C, 0);
                put_at<u32>(ob, 0, 0x004FD5B8u);
                put_at<i32>(ob, 4, ns.deadline);
                put_at<u32>(ob, 8, ns.next_emit);
                put_at<u32>(ob, 0x10, parts);
                put_at<u32>(ob, 0x14, parts + static_cast<u32>(ns.particles.size()) * 0x30);
                put_at<u32>(ob, 0x18, parts + static_cast<u32>(ns.particles.size()) * 0x30);
                r.write(obj, ob.data(), ob.size());
                std::vector<u8> pb(ns.particles.size() * 0x30, 0);
                for (std::size_t q = 0; q < ns.particles.size(); ++q) {
                    const sim::NanoParticle& p = ns.particles[q];
                    std::memcpy(&pb[q * 0x30], p.pos.data(), 12);
                    std::memcpy(&pb[q * 0x30 + 12], p.target.data(), 12);
                    std::memcpy(&pb[q * 0x30 + 24], p.vel.data(), 12);
                    put_at<u32>(pb, q * 0x30 + 36, p.flags);
                    put_at<u32>(pb, q * 0x30 + 40, p.colour);
                    put_at<i32>(pb, q * 0x30 + 44, p.expire);
                }
                if (!pb.empty()) r.write(parts, pb.data(), pb.size());
                r.w32(ptrs + static_cast<u32>(k) * 4, obj);
            }
            r.w32(fx_lists + 16 * 6 + 4, ptrs);
            r.w32(fx_lists + 16 * 6 + 8, ptrs + static_cast<u32>(streams.size()) * 4);
            r.w32(fx_lists + 16 * 6 + 12, ptrs + static_cast<u32>(streams.size()) * 4);
        }
        // Units.
        bool ok = true;
        {
            std::vector<u8> table(b.units.size() * kUnitSize, 0);
            for (std::size_t i = 1; i < b.units.size() && ok; ++i) {
                const sim::UnitTickState& t = b.ticks[i];
                if (!t.type_index || t.type_index >= world.types_.size() || !unit_type(t.type_index)) continue;
                const sim::Unit& u = b.units[i];
                const u32 at = units_at + static_cast<u32>(i) * kUnitSize;
                // Object state: built once per unit and type, then the engine's pose copied in.
                if (!unit_state[i] || unit_state_type[i] != t.type_index) {
                    u32 state = 0;
                    if (!o.call(kBuildState, 0, {type_model[t.type_index], type_cob[t.type_index], at}, &state) || !state) {
                        ok = false;
                        break;
                    }
                    unit_state[i] = state;
                    unit_state_type[i] = t.type_index;
                }
                const u32 state = unit_state[i];
                r.w32(state + 0xC, at);
                const sim::PoseState& ps = u.pose;
                const i32 count = static_cast<i32>(r.r32(state));
                r.w32(state + 4, static_cast<u32>(ps.cache_draw_count));
                r.w32(state + 8, static_cast<u32>(ps.anim_dirty));
                r.write(state + 0x18, ps.base_angle.data(), 6);
                for (std::size_t p = 0; p < ps.pieces.size() && static_cast<i32>(p) < count; ++p) {
                    const sim::PosePiece& pc = ps.pieces[p];
                    const u32 pa = state + kStateHeader + static_cast<u32>(p) * kPieceSize;
                    std::uint8_t e[kPieceSize];
                    r.read(pa, e, kPieceSize);
                    std::memcpy(e + 4, pc.move.data(), 12);
                    std::memcpy(e + 16, pc.turn.data(), 6);
                    std::memcpy(e + 22, pc.xform.data(), 12);
                    std::memcpy(e + 38, &pc.skip_transform, 2);
                    std::memcpy(e + 40, &pc.flags, 2);
                    r.write(pa, e, kPieceSize);
                    u32 pts = 0;
                    std::memcpy(&pts, e + 34, 4);
                    if (pts && !pc.points.empty()) r.write(pts, pc.points.data(), pc.points.size() * 4);
                }
                u8* d = &table[i * kUnitSize];
                const u32 motion = motions + static_cast<u32>(i) * 0x40;
                if (t.has_motion) {
                    r.write(motion + 8, t.velocity.data(), 12);
                    r.w32(motion + 0x20, static_cast<u32>(t.motion.speed));
                    std::memcpy(d + 0x00, &motion, 4);
                }
                std::memcpy(d + 0x64, &u.pitch, 2);
                std::memcpy(d + 0x66, &u.heading, 2);
                std::memcpy(d + 0x68, &u.bank, 2);
                std::memcpy(d + 0x6A, u.pos.data(), 12);
                std::memcpy(d + 0x76, &u.cell_x, 2);
                std::memcpy(d + 0x78, &u.cell_z, 2);
                auto uptr = [&](const sim::Unit* x) { return x ? units_at + static_cast<u32>(x->id) * kUnitSize : 0u; };
                const u32 carrier = uptr(u.carrier), head = uptr(u.cargo_head), next = uptr(u.cargo_next);
                std::memcpy(d + 0x86, &carrier, 4);
                std::memcpy(d + 0x8A, &head, 4);
                std::memcpy(d + 0x8E, &next, 4);
                const u32 def = udefs + static_cast<u32>(t.type_index) * kDefSize;
                std::memcpy(d + 0x92, &def, 4);
                const u32 owner = gs + gPlayers + static_cast<u32>(t.owner) * kPlayerSize;
                std::memcpy(d + 0x96, &owner, 4);
                const u32 script = 1;  // drawn when nonzero
                std::memcpy(d + 0x9A, &script, 4);
                std::memcpy(d + 0x9E, &state, 4);
                std::memcpy(d + 0xA6, &t.type_index, 2);
                std::memcpy(d + 0xA8, &u.id, 2);
                const u32 group = u.group;
                std::memcpy(d + 0xAC, &group, 4);
                d[0xFF] = static_cast<u8>(t.owner);
                if (opt.hud) {
                    // The bottom bar's reads: the head order (+0x5C: kind +4, target link's unit
                    // +0x16), no secondary orders (+0x60), kills (+0xB8), the resource slot's last
                    // settlement (+0xCC energy made, +0xD0 energy used, +0xE4 metal made, +0xE8 metal used).
                    if (u.orders) {
                        const u32 ord = head_orders + static_cast<u32>(i) * 0x58;
                        std::vector<u8> ob(0x56, 0);
                        ob[4] = u.orders->kind;
                        put_at<u32>(ob, 0x16, uptr(u.orders->target.unit));
                        r.write(ord, ob.data(), ob.size());
                        std::memcpy(d + 0x5C, &ord, 4);
                    }
                    // Weapon slots (+0x10, 0x1C each): the bar only remembers their reload state,
                    // so each gets a definition with reload time 0 (state "none").
                    for (u32 k = 0; k < 3; ++k) std::memcpy(d + 0x10 + 0x1C * k, &wdefs, 4);
                    std::memcpy(d + 0xB8, &t.kill_count, 2);
                    std::memcpy(d + 0xCC, &t.energy_make_prev, 4);
                    std::memcpy(d + 0xD0, &t.energy_use_prev, 4);
                    std::memcpy(d + 0xE4, &t.metal_make_prev, 4);
                    std::memcpy(d + 0xE8, &t.metal_use_prev, 4);
                }
                std::memcpy(d + 0x104, &u.build_fraction_left, 4);
                std::memcpy(d + 0x108, &u.hit_points, 2);
                d[0x10E] = u.activate_flags;
                d[0x10F] = u.script_flags;
                std::memcpy(d + 0x110, &u.flags, 4);
                std::memcpy(d + 0x114, &u.draw_flags, 4);
            }
            // Table entries are written after the states (the state builder reads the unit).
            if (ok) r.write(units_at, table.data(), table.size());
        }
        if (!ok) {
            ++st.failures;
            std::printf("scene %d: unit set-up failed\n", sc);
            continue;
        }
        // Background: a pattern (the view is fully covered by the terrain).
        std::vector<u8> bg(static_cast<std::size_t>(W * H));
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) bg[static_cast<std::size_t>(y * W + x)] = static_cast<u8>((x / 8 + y / 8) & 1 ? 0xC5 : 0x50);
        r.write(o.frame, bg.data(), bg.size());
        if (opt.hud) {
            // Selection boxes on (option bit 2); the bottom bar: the unit under the pointer, no
            // build button and no feature under it, no debug view, and its last-drawn state
            // forgotten (it redraws only when that changes).
            u8 go = 0;
            r.read(gs + gGameOptions, &go, 1);
            go = static_cast<u8>(go | 4);
            r.write(gs + gGameOptions, &go, 1);
            const u16 none = 0xFFFF, zero = 0;
            r.write(gs + 0x2CBA, &hover, 2);
            r.write(gs + 0x2CBC, &none, 2);
            r.w32(gs + 0x581, 0xFFFFFFFFu);
            r.write(gs + 0x3923B, &zero, 2);
            std::vector<u8> ff(0x3C, 0xFF);
            r.write(gs + 0x37E60, ff.data(), ff.size());
            if (opt.verbose && hover)
                std::printf("  hover unit %u: owner %d kills %u flags %08X order %d hp %d\n", hover, b.ticks[hover].owner, b.ticks[hover].kill_count,
                            b.units[hover].flags, b.units[hover].orders ? b.units[hover].orders->kind : -1, b.units[hover].hit_points);
            if (std::getenv("FH_TEXT")) {
                // Debugging: one line of text with the side's font straight onto the frame.
                const u32 side_font = r.r32(gs + static_cast<u32>(setup.players[0].side) * 0x232 + 0x3816B);
                o.call(0x004C1420, 0, {side_font});
                o.call(0x004C13A0, 0, {0x53, 0xFFFFFFFFu});
                u32 ctx_clip[4] = {};
                r.read(o.ctx + 0x1C, ctx_clip, sizeof ctx_clip);
                const u32 txt = o.alloc(16);
                r.write(txt, "TEST 123", 9);
                const bool t_ok = o.call(0x004C14F0, 0, {o.ctx, txt, 200, static_cast<u32>(H - 20), 0xFFFFFFFFu});
                std::vector<u8> row(static_cast<std::size_t>(W) * 12);
                r.read(o.frame + static_cast<u32>((H - 20) * W), row.data(), row.size());
                int ink = 0;
                for (u8 v : row) ink += v == 0x53;
                // And a colour logo stretched into (300, 300)-(339, 339).
                const u32 rect = o.alloc(16);
                const i32 rv[4] = {300, 300, 339, 339};
                r.write(rect, rv, sizeof rv);
                std::vector<u8> before(40 * 40), after(40 * 40);
                for (int yy = 0; yy < 40; ++yy) r.read(o.frame + static_cast<u32>((300 + yy) * W + 300), &before[static_cast<std::size_t>(yy) * 40], 40);
                const bool l_ok = o.call(0x00467C00, 0, {o.ctx, gs + gPlayers, rect, 0});
                for (int yy = 0; yy < 40; ++yy) r.read(o.frame + static_cast<u32>((300 + yy) * W + 300), &after[static_cast<std::size_t>(yy) * 40], 40);
                int changed = 0;
                for (std::size_t q = 0; q < before.size(); ++q) changed += before[q] != after[q];
                std::printf("  logo test: %s, %d pixels changed (logos %08X)\n", l_ok ? "ok" : "failed", changed, r.r32(gs + 0x148DB));
                std::printf("  text test: %s, font %08X (height %u), ctx clip %d,%d,%d,%d, ink %d\n", t_ok ? "ok" : "failed", side_font,
                            side_font ? r.r32(side_font) & 0xFF : 0, static_cast<int>(ctx_clip[0]), static_cast<int>(ctx_clip[1]),
                            static_cast<int>(ctx_clip[2]), static_cast<int>(ctx_clip[3]), ink);
            }
            if (std::getenv("FH_BAR_ALONE")) {
                // Debugging: the bar on its own first.
                const bool bar_ok = o.call(kBottomBar, 0, {o.ctx}, nullptr, 5000);
                std::printf("  bottom bar alone: %s\n", bar_ok ? "ok" : "failed");
                r.write(gs + 0x37E60, ff.data(), ff.size());
            }
        }
        // The original's frame.
        if (!o.call(kVisibleList, 0, {}) || !o.call(kDrawFrame, 0, {1, 0})) {
            ++st.failures;
            std::printf("scene %d: original frame failed\n", sc);
            if (const char* dbg = std::getenv("FH_DUMP")) {
                const u32 a = static_cast<u32>(std::strtoul(dbg, nullptr, 16));
                std::uint8_t m[64];
                r.read(a, m, sizeof m);
                for (int k = 0; k < 64; ++k) std::printf("%02X%s", m[k], k % 16 == 15 ? "\n" : " ");
                for (const auto& [name, base] : o.gafs) {
                    std::printf("gaf %s at %08X\n", name.c_str(), base);
                    if (const std::vector<u8>* fb = anims.file(name)) {
                        std::string fn = name;
                        for (char& ch : fn)
                            if (ch == '\\' || ch == '/') ch = '_';
                        if (FILE* f = std::fopen((opt.out_dir + "/" + fn + ".gafdump").c_str(), "wb")) {
                            std::fwrite(fb->data(), 1, fb->size(), f);
                            std::fclose(f);
                        }
                    }
                }
            }
            // A fault leaves the process in an unknown state: it is ended (o.r's destructor) and
            // the run stops.
            return 1;
        }
        std::vector<u8> theirs(static_cast<std::size_t>(W * H));
        r.read(o.frame, theirs.data(), theirs.size());
        b.projectiles = saved_shots;

        // Compare the battle view.
        long long diff = 0, n = 0;
        render::Image8 dimg(W, H, 0);
        for (int y = 32; y <= H - 33; ++y)
            for (int x = 128; x < W; ++x) {
                const std::size_t i = static_cast<std::size_t>(y * W + x);
                ++n;
                if (theirs[i] != mine.pixels[i]) {
                    ++diff;
                    dimg.pixels[i] = 0xFF;
                }
            }
        ++st.scenes;
        st.pixels += diff;
        if (!diff) ++st.identical;
        int nunits = 0;
        for (std::size_t i = 1; i < b.units.size(); ++i) nunits += b.ticks[i].type_index != 0;
        std::printf("scene %d: tick %u scroll %d,%d units %d visible %u shots %d: %lld of %lld view pixels differ\n", sc, b.game_tick(), sx, sy, nunits,
                    r.r32(gs + gVisibleCount), static_cast<int>(scene.projectiles.size()), diff, n);
        if (opt.hud) {
            // The bottom bar: the client's whole frame against the original's, over the bar's rows
            // right of the side panel.
            int nsel = 0;
            for (const render::UnitInstance& ui : scene.units) nsel += ui.outline;
            const render::Image8 cf = render::execute(gc.draw(), world.blend_);
            long long bd = 0, bn = 0;
            render::Image8 bimg(W, H, 0);
            for (int y = H - 32; y < H; ++y)
                for (int x = 129; x < W; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y * W + x);
                    ++bn;
                    if (theirs[i] != cf.pixels[i]) {
                        ++bd;
                        bimg.pixels[i] = 0xFF;
                    }
                }
            const sim::Unit* hu = hover ? &b.units[hover] : nullptr;
            std::printf("  hud: %d selection outlines in the view; bottom bar (unit %u%s, order %d): %lld of %lld pixels differ\n", nsel, hover,
                        hu ? (b.ticks[hover].owner == 0 ? " own" : " other") : "", hu && hu->orders ? hu->orders->kind : -1, bd, bn);
            if (!opt.out_dir.empty() && bd && written < 40) {
                render::Image8 a(W, H);
                a.pixels = theirs;
                const std::string base = opt.out_dir + "/hud_" + std::to_string(opt.seed) + "_" + std::to_string(sc);
                render::write_bmp(base + "_orig.bmp", a, world.palette_);
                render::write_bmp(base + "_client.bmp", cf, world.palette_);
                render::write_bmp(base + "_diff.bmp", bimg, world.palette_);
            }
            st.pixels += bd;
            if (bd && !diff) --st.identical;
        }
        if (opt.verbose) {
            std::uint16_t w[4];
            r.read(o.probe, w, 8);
            std::printf("  stack words at the shot drawing's entry - 0x40: %04X %04X %04X %04X\n", w[0], w[1], w[2], w[3]);
            // Who wrote them: the frame again with write watchpoints on the two words.
            u32 esp0 = 0;
            std::vector<u32> hits;
            r.call_watch(kVisibleList, 0, {}, {}, hits);
            if (r.call_watch(kDrawFrame, 0, {1, 0}, {}, hits, &esp0)) {
                const int delta = static_cast<int>(r.r32(o.probe + 8)) - static_cast<int>(esp0);
                r.call_watch(kDrawFrame, 0, {1, 0}, {delta - 0x40, delta - 0x3C}, hits, &esp0);
                const std::size_t at_entry = std::min<std::size_t>(r.r32(o.probe + 12), hits.size());
                std::printf("  writers before the shot drawing (%zu):", at_entry);
                for (std::size_t k = at_entry > 8 ? at_entry - 8 : 0; k < at_entry; ++k) std::printf(" %08X", hits[k]);
                std::printf("\n");
            }
        }
        if (diff && opt.verbose)
            for (const render::ProjectileInstance& p : scene.projectiles)
                std::printf("  shot type %d at %d,%d colour %u frame %p %dx%d off %d,%d\n", p.render_type, render::screen_x(scene.camera, p.pos[0]),
                            render::screen_y(scene.camera, p.pos[1], p.pos[2]), p.colour, static_cast<const void*>(p.frame), p.frame ? p.frame->width : 0,
                            p.frame ? p.frame->height : 0, p.frame ? p.frame->x_offset : 0, p.frame ? p.frame->y_offset : 0);
        if (!opt.out_dir.empty() && (diff || opt.verbose) && written < 40) {
            ++written;
            render::Image8 a(W, H), e(W, H);
            a.pixels = theirs;
            e.pixels = mine.pixels;
            const std::string base = opt.out_dir + "/frame_" + std::to_string(opt.seed) + "_" + std::to_string(sc);
            render::write_bmp(base + "_orig.bmp", a, world.palette_);
            render::write_bmp(base + "_engine.bmp", e, world.palette_);
            render::write_bmp(base + "_diff.bmp", dimg, world.palette_);
        }
    }
    std::printf("scenes %d: identical %d, different %d (%lld pixels), failures %d\n", st.scenes, st.identical, st.scenes - st.identical, st.pixels,
                st.failures);
    return st.scenes == st.identical && !st.failures ? 0 : 1;
}
