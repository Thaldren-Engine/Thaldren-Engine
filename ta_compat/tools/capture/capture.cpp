// Golden-trace capture for the original TotalA.exe (TA 3.1, image base 0x400000).
//
// Runs the game under the Win32 debug API, places int3 breakpoints at known
// simulation addresses, optionally overrides the two RNG seeds, and writes one
// JSON object per line describing what happened. Windows / 32-bit only.
//
// Usage:
//   ta_capture --exe <TotalA.exe> [--cwd <dir>] --out <trace.jsonl>
//              [--battle-seed <u32>] [--rand-seed <u32>]
//              [--max-ticks <n>] [--rng-calls] [--dump-defs <file>]
//              [--game] [--auto-skirmish]
//
// --game: whole-game capture for the replay test (ta_compat/tools/game_replay): the battle's
// configuration at the first step, the start-position shuffle's rand() state, the map, every
// frame (end of its steps), every order entering the simulation (interface and computer
// player, with the caller), the stale stack words the engine takes as inputs, and a digest of
// the state after every step. --auto-skirmish presses the menus' quick keys (S, S, S) until a
// skirmish with the last used settings starts.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

// --- Addresses in TotalA.exe (see docs/RE_MEMORY.md) -------------------------
constexpr DWORD kGameStatePtr = 0x00511DE8;     // GameState*
constexpr DWORD kOffGameTick = 0x38A47;         // GameState::dwGameTick
constexpr DWORD kOffSimStepsPending = 0x38A3B;  // GameState::nSimStepsPending
constexpr DWORD kOffUnitTable = 0x14357;        // Unit* (0x118 bytes each)
constexpr DWORD kDisplayContextPtr = 0x0051FBD0;
constexpr DWORD kOffTickRateHz = 0xE8;
constexpr DWORD kBattleRngState = 0x0051FC88;

constexpr DWORD kBpBattleRngSeed = 0x004B6CA0;  // entry: [esp+4] = seed
constexpr DWORD kBpBattleRngNext = 0x004B6C30;  // entry: [esp] = caller, [esp+4] = n
constexpr DWORD kBpSrand = 0x004E4860;          // entry: [esp+4] = seed
constexpr DWORD kBpSrandTd = 0x004E4865;        // after TLS lookup: eax = per-thread data
constexpr DWORD kBpRandTd = 0x004E4875;         // after TLS lookup: eax = per-thread data
constexpr DWORD kBpSimStep = 0x004954C6;        // after dwGameTick++ in the step loop
constexpr DWORD kOffRandState = 0x14;           // per-thread data: rand seed
constexpr DWORD kBpArchiveOpen = 0x004BDD70;    // HPI open: [esp+4] = path, [esp+8] = keep open

// Definition tables (GameState offsets / statics), for --dump-defs.
constexpr DWORD kOffWeaponDefs = 0x2CF3;        // WeaponDef[256], 0x115 bytes each (inline)
constexpr DWORD kWeaponDefSize = 0x115;
constexpr DWORD kOffFeatureDefCount = 0x14253;
constexpr DWORD kOffFeatureDefs = 0x1426F;      // FeatureDef*, 0x100 bytes each
constexpr DWORD kOffUnitDefCount = 0x1438F;
constexpr DWORD kOffUnitDefs = 0x1439B;         // UnitDef*, 0x249 bytes each
constexpr DWORD kUnitDefSize = 0x249;
constexpr DWORD kOffSoundCategories = 0x37E13;  // SoundCategory*, 0x160 bytes each
constexpr DWORD kOffSoundCategoryCount = 0x37E17;
constexpr DWORD kMoveInfoArray = 0x00512358;    // MoveInfo[32], 0x20 bytes each
constexpr DWORD kCategoryTableBegin = 0x0051E6B4;
constexpr DWORD kMissionTableBegin = 0x00512344;  // MissionOrderTableEntry*, 0x19 bytes each
constexpr DWORD kTranslateLanguage = 0x0051FDC0;  // char[64]
constexpr DWORD kOffUnitModels = 0x14377;        // Model3do** (one per unit def)
constexpr DWORD kOffSoundBank = 0x33A0B;          // {int count; ptr slots[256]; char names[256][32]; char paths[256][32]}

// Whole-game capture (--game): battle start inputs, frames, orders, stale stack words and a
// digest of the simulation state after every step (docs/RE_MEMORY.md "Whole-game replay").
constexpr DWORD kBpStepEnd = 0x0049566C;        // end of one step in Game_RunSimulationStep (DEC EBX)
constexpr DWORD kBpFrameEnd = 0x00495674;       // after the frame's steps (also with none), before its tail
constexpr DWORD kBpShuffleRandA = 0x004979E7;   // battle init: the start-position shuffle's rand() calls
constexpr DWORD kBpShuffleRandB = 0x00497A32;
constexpr DWORD kBpMapPath = 0x00483632;        // Map_LoadTnt: eax = the map file path
constexpr DWORD kBpEnqueueOrder = 0x0043ADC0;   // Unit_EnqueueOrder(kind, queued, unit, target, pos*, p0, p1)
constexpr DWORD kBpIssueOrCancel = 0x0043AFC0;  // the interface's per-unit order (same arguments)
constexpr DWORD kBpBuildCount = 0x0043B0B0;     // Unit_AdjustBuildCount(kind, unit, type, delta)
constexpr DWORD kBpBuildMenu = 0x00419B00;      // build menu command (name, unit, delta)
constexpr DWORD kBpIssueSelected = 0x0048CF30;  // Unit_IssueOrdersToSelected (interface)
constexpr DWORD kBpAiConstructors = 0x00408100; // Ai_RoleConstructorsTick: site height = [esp-0x2C]
constexpr DWORD kBpReclaimSites = 0x0047EA40;   // Map_PickReclaimSites: site y = [esp-8]
constexpr DWORD kOffPlayers = 0x1B63, kPlayerSize = 0x14B, kUnitSize = 0x118;
constexpr DWORD kOffLocalPlayer = 0x2A42, kOffLocalView = 0x2A43;
constexpr DWORD kOffProjCount = 0x141F3, kOffProjTable = 0x141F7, kProjSize = 0x6B, kMaxProjectiles = 300;
constexpr DWORD kOffMapW = 0x14233, kOffMapH = 0x14237, kOffCells = 0x14287, kCellSize = 13;
constexpr DWORD kOffMapFlags = 0x14281, kOffUnitLimit = 0x37EE6, kOffAiDifficulty = 0x37EEE, kOffGameMode = 0x37EF6;
constexpr DWORD kOffWindHeading = 0x37ED8, kOffWindSpeed = 0x37EDA;

struct Options {
    std::string exe;
    std::string cwd;
    std::string out;
    std::optional<std::uint32_t> battle_seed;
    std::optional<std::uint32_t> rand_seed;
    std::uint32_t max_ticks = 0;  // 0 = unlimited
    bool rng_calls = false;
    std::string dump_defs;        // write definition tables at the first sim step
    bool game = false;            // whole-game capture (digests, orders, frames, stale words)
    bool auto_skirmish = false;   // press the menus' quick keys until a skirmish starts
};

struct Breakpoint {
    DWORD addr;
    BYTE original;
    bool enabled;
};

HANDLE g_process = nullptr;
FILE* g_out = nullptr;
std::map<DWORD, Breakpoint> g_bps;
std::map<DWORD, HANDLE> g_threads;          // thread id -> handle
std::map<DWORD, DWORD> g_thread_td;         // thread id -> per-thread CRT data
std::map<DWORD, DWORD> g_pending_reinsert;  // thread id -> bp address to re-arm after single-step
std::uint64_t g_battle_draws_this_step = 0;
std::uint64_t g_rand_draws_this_step = 0;

std::uint32_t parse_u32(const char* s) { return static_cast<std::uint32_t>(std::strtoul(s, nullptr, 0)); }

bool read_u32(DWORD addr, std::uint32_t& v) {
    SIZE_T n = 0;
    return ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(addr)), &v, 4, &n) && n == 4;
}

bool write_u32(DWORD addr, std::uint32_t v) {
    SIZE_T n = 0;
    return WriteProcessMemory(g_process, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(addr)), &v, 4, &n) && n == 4;
}

bool write_byte(DWORD addr, BYTE b) {
    SIZE_T n = 0;
    const bool ok = WriteProcessMemory(g_process, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(addr)), &b, 1, &n) && n == 1;
    FlushInstructionCache(g_process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(addr)), 1);
    return ok;
}

void add_bp(DWORD addr) {
    BYTE orig = 0;
    SIZE_T n = 0;
    if (!ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(addr)), &orig, 1, &n) || n != 1) {
        std::fprintf(stderr, "cannot read breakpoint address 0x%08lX\n", addr);
        return;
    }
    g_bps[addr] = Breakpoint{addr, orig, true};
    write_byte(addr, 0xCC);
}

bool read_block(DWORD addr, void* dst, std::size_t size) {
    SIZE_T n = 0;
    return ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(addr)), dst, size, &n) &&
           n == size;
}

std::string read_cstr(DWORD addr) {
    std::string s;
    char c = 0;
    while (addr && s.size() < 4096 && read_block(addr++, &c, 1) && c) s += c;
    return s;
}

// Chunked binary: 4-char tag, u32 payload size, payload.
void write_chunk(FILE* f, const char tag[4], const std::vector<std::uint8_t>& data) {
    const auto size = static_cast<std::uint32_t>(data.size());
    std::fwrite(tag, 1, 4, f);
    std::fwrite(&size, 4, 1, f);
    if (size) std::fwrite(data.data(), 1, size, f);
}

void put_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

void put_str(std::vector<std::uint8_t>& v, const std::string& s) {
    v.insert(v.end(), s.begin(), s.end());
    v.push_back(0);
}

// Serializes a loaded 3DO piece tree: node, then its child subtree, then its sibling chain.
void dump_model(std::vector<std::uint8_t>& out, DWORD node, int depth) {
    while (node && depth < 64) {
        std::uint8_t h[0x34] = {};
        if (!read_block(node, h, sizeof h)) return;
        std::uint32_t v[13];
        std::memcpy(v, h, sizeof v);
        put_u32(out, 1);  // node marker
        put_str(out, read_cstr(v[7]));
        for (int k : {0, 1, 2, 3, 4, 5, 6}) put_u32(out, v[k]);
        const std::uint32_t nverts = v[1] < 100000 ? v[1] : 0, nprims = v[2] < 100000 ? v[2] : 0;
        std::vector<std::uint8_t> verts(nverts * 12u);
        if (!verts.empty()) read_block(v[9], verts.data(), verts.size());
        out.insert(out.end(), verts.begin(), verts.end());
        for (std::uint32_t p = 0; p < nprims; ++p) {
            std::uint8_t ph[0x20] = {};
            read_block(v[10] + p * 0x20u, ph, sizeof ph);
            std::uint32_t color = 0, nidx = 0, pidx = 0;
            std::memcpy(&color, ph, 4);
            std::memcpy(&nidx, ph + 4, 4);
            std::memcpy(&pidx, ph + 12, 4);
            if (nidx > 1000) nidx = 0;
            put_u32(out, color);
            put_u32(out, nidx);
            std::vector<std::uint8_t> idx(nidx * 2u);
            if (!idx.empty()) read_block(pidx, idx.data(), idx.size());
            out.insert(out.end(), idx.begin(), idx.end());
        }
        if (v[12]) dump_model(out, v[12], depth + 1);  // child
        node = v[11];                                  // sibling
    }
    put_u32(out, 0);  // end of chain
}

bool dump_defs(const std::string& path) {
    std::uint32_t gs = 0;
    if (!read_u32(kGameStatePtr, gs) || !gs) return false;
    std::uint32_t ud_count = 0, ud_ptr = 0, ft_count = 0, ft_ptr = 0, sc_count = 0, sc_ptr = 0;
    read_u32(gs + kOffUnitDefCount, ud_count);
    read_u32(gs + kOffUnitDefs, ud_ptr);
    read_u32(gs + kOffFeatureDefCount, ft_count);
    read_u32(gs + kOffFeatureDefs, ft_ptr);
    read_u32(gs + kOffSoundCategoryCount, sc_count);
    read_u32(gs + kOffSoundCategories, sc_ptr);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    std::vector<std::uint8_t> hdr;
    const std::uint32_t header[] = {gs,     ud_count, ud_ptr,   static_cast<std::uint32_t>(gs + kOffWeaponDefs),
                                    static_cast<std::uint32_t>(kMoveInfoArray), ft_count, ft_ptr, sc_count, sc_ptr};
    for (std::uint32_t v : header) put_u32(hdr, v);
    write_chunk(f, "HDR ", hdr);

    std::vector<std::uint8_t> ud(static_cast<std::size_t>(ud_count) * kUnitDefSize);
    read_block(ud_ptr, ud.data(), ud.size());
    write_chunk(f, "UDEF", ud);

    std::vector<std::uint8_t> wd(256u * kWeaponDefSize);
    read_block(gs + kOffWeaponDefs, wd.data(), wd.size());
    write_chunk(f, "WDEF", wd);

    std::vector<std::uint8_t> mv(32u * 0x20);
    read_block(kMoveInfoArray, mv.data(), mv.size());
    write_chunk(f, "MOVE", mv);
    std::vector<std::uint8_t> mvn;
    for (int i = 0; i < 32; ++i) {
        std::uint32_t p = 0;
        std::memcpy(&p, &mv[static_cast<std::size_t>(i) * 0x20], 4);
        put_u32(mvn, p);
        put_str(mvn, read_cstr(p));
    }
    write_chunk(f, "MVNM", mvn);

    std::uint32_t cb = 0, ce = 0;
    read_u32(kCategoryTableBegin, cb);
    read_u32(kCategoryTableBegin + 4, ce);
    std::vector<std::uint8_t> cats;
    put_u32(cats, (ce - cb) / 8);
    for (std::uint32_t p = cb; p < ce; p += 8) {
        std::uint32_t name = 0, mask = 0;
        read_u32(p, name);
        read_u32(p + 4, mask);
        std::uint8_t bits[64] = {};
        read_block(mask, bits, sizeof bits);
        put_u32(cats, mask);
        cats.insert(cats.end(), bits, bits + 64);
        put_str(cats, read_cstr(name));
    }
    write_chunk(f, "CATS", cats);

    std::vector<std::uint8_t> yard, bopt;
    for (std::uint32_t i = 0; i < ud_count; ++i) {
        const std::uint8_t* u = &ud[static_cast<std::size_t>(i) * kUnitDefSize];
        std::int16_t fx = 0, fz = 0;
        std::uint32_t yp = 0, bp = 0;
        std::memcpy(&fx, u + 330, 2);
        std::memcpy(&fz, u + 332, 2);
        std::memcpy(&yp, u + 334, 4);
        std::memcpy(&bp, u + 342, 4);
        if (yp && fx > 0 && fz > 0) {
            std::vector<std::uint8_t> y(static_cast<std::size_t>(fx) * static_cast<std::size_t>(fz));
            read_block(yp, y.data(), y.size());
            put_u32(yard, i);
            put_u32(yard, static_cast<std::uint32_t>(y.size()));
            yard.insert(yard.end(), y.begin(), y.end());
        }
        if (bp) {
            std::uint8_t b[0x3C] = {};
            read_block(bp, b, sizeof b);
            put_u32(bopt, i);
            bopt.insert(bopt.end(), b, b + sizeof b);
        }
    }
    write_chunk(f, "YARD", yard);
    write_chunk(f, "BOPT", bopt);

    std::uint32_t mb = 0, me = 0;
    read_u32(kMissionTableBegin, mb);
    read_u32(kMissionTableBegin + 4, me);
    std::vector<std::uint8_t> miss;
    put_u32(miss, (me - mb) / 0x19);
    for (std::uint32_t p = mb; p < me; p += 0x19) {
        std::uint32_t display = 0, lookup = 0;
        read_u32(p, display);
        read_u32(p + 21, lookup);
        put_str(miss, read_cstr(lookup));
        put_str(miss, read_cstr(display));
    }
    write_chunk(f, "MISS", miss);
    std::vector<std::uint8_t> lang;
    put_str(lang, read_cstr(kTranslateLanguage));
    write_chunk(f, "LANG", lang);

    // Per weapon slot: damage override pairs and explosion sequence names.
    std::vector<std::uint8_t> wdmg, wseq;
    for (std::uint32_t i = 0; i < 256; ++i) {
        const std::uint8_t* w = &wd[i * kWeaponDefSize];
        std::uint32_t dmap = 0, seq = 0, wseq_p = 0;
        std::memcpy(&dmap, w + 100, 4);
        std::memcpy(&seq, w + 120, 4);
        std::memcpy(&wseq_p, w + 124, 4);
        if (dmap) {
            std::uint32_t b = 0, e = 0;
            read_u32(dmap + 5, b);
            read_u32(dmap + 9, e);
            put_u32(wdmg, i);
            put_u32(wdmg, (e - b) / 8);
            for (std::uint32_t p = b; p < e; p += 8) {
                std::uint32_t name = 0, dmg = 0;
                read_u32(p, name);
                read_u32(p + 4, dmg);
                put_u32(wdmg, dmg);
                put_str(wdmg, read_cstr(name));
            }
        }
        put_u32(wseq, i);
        put_str(wseq, seq ? read_cstr(seq + 8) : std::string());
        put_str(wseq, wseq_p ? read_cstr(wseq_p + 8) : std::string());
    }
    write_chunk(f, "WDMG", wdmg);
    write_chunk(f, "WSEQ", wseq);

    // Models: one tree per unit def (index order), then one per weapon slot.
    {
        std::vector<std::uint8_t> modl;
        std::uint32_t models = 0;
        read_u32(gs + kOffUnitModels, models);
        put_u32(modl, ud_count);
        for (std::uint32_t i = 0; i < ud_count; ++i) {
            std::uint32_t m = 0;
            // Entry 0 ("None") is never written by the game: its pointer is uninitialised heap memory.
            if (models && i > 0) read_u32(models + i * 4, m);
            put_u32(modl, m);
            if (m) dump_model(modl, m, 0);
        }
        write_chunk(f, "MODL", modl);
        std::vector<std::uint8_t> wmdl;
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t m = 0;
            std::memcpy(&m, &wd[i * kWeaponDefSize + 116], 4);
            put_u32(wmdl, m);
            if (m) dump_model(wmdl, m, 0);
        }
        write_chunk(f, "WMDL", wmdl);
    }

    std::vector<std::uint8_t> sbnk(8 + 256 * 4 + 256 * 32 * 2);  // +4 count, +8 slots, +0x408 names, +0x2408 paths
    read_block(gs + kOffSoundBank, sbnk.data(), sbnk.size());
    write_chunk(f, "SBNK", sbnk);

    std::vector<std::uint8_t> ft(static_cast<std::size_t>(ft_count) * 0x100);
    if (ft_ptr) read_block(ft_ptr, ft.data(), ft.size());
    write_chunk(f, "FEAT", ft);
    // Feature details behind pointers: GAF loaded?, 8 sequence names (2D) or the model tree (3D),
    // burn weapon slot.
    {
        std::vector<std::uint8_t> fx;
        for (std::uint32_t i = 0; i < ft_count; ++i) {
            const std::uint8_t* r = &ft[static_cast<std::size_t>(i) * 0x100];
            std::uint16_t flags = 0;
            std::uint32_t p = 0;
            std::memcpy(&flags, r + 0xFE, 2);
            if (flags & 1) {
                std::memcpy(&p, r + 0xA8, 4);
                put_u32(fx, p ? 1 : 0);
                for (int k = 0; k < 8; ++k) {
                    std::uint32_t seq = 0;
                    std::memcpy(&seq, r + 0xAC + k * 4, 4);
                    put_str(fx, seq ? read_cstr(seq + 8) : std::string());
                }
            } else {
                std::memcpy(&p, r + 0x98, 4);
                put_u32(fx, p);
                if (p) dump_model(fx, p, 0);
            }
            std::memcpy(&p, r + 0xE4, 4);
            const std::uint32_t base = gs + kOffWeaponDefs;
            put_u32(fx, p ? (p - base) / kWeaponDefSize : 0xFFFFFFFFu);
        }
        write_chunk(f, "FTX2", fx);
    }
    std::vector<std::uint8_t> sc(static_cast<std::size_t>(sc_count) * 0x160);
    if (sc_ptr) read_block(sc_ptr, sc.data(), sc.size());
    write_chunk(f, "SNDC", sc);
    std::fclose(f);
    return true;
}

std::uint32_t game_tick() {
    std::uint32_t gs = 0, tick = 0;
    if (read_u32(kGameStatePtr, gs) && gs != 0) read_u32(gs + kOffGameTick, tick);
    return tick;
}

std::uint32_t rand_state_for(DWORD tid, bool& known) {
    const auto it = g_thread_td.find(tid);
    known = it != g_thread_td.end();
    std::uint32_t v = 0;
    if (known) read_u32(it->second + kOffRandState, v);
    return v;
}

// --- Whole-game capture ------------------------------------------------------------------

bool g_in_step = false;           // between the step start and the step end breakpoints
bool g_battle_loading = false;    // the battle seed was seen (the menus are done)
bool g_start_written = false;
std::uint32_t g_unit_table = 0;

std::uint32_t game_state() {
    std::uint32_t gs = 0;
    read_u32(kGameStatePtr, gs);
    return gs;
}

int unit_id_of(std::uint32_t ptr) {
    if (!ptr || !g_unit_table || ptr < g_unit_table) return ptr ? -1 : 0;
    const std::uint32_t d = ptr - g_unit_table;
    return d % kUnitSize == 0 ? static_cast<int>(d / kUnitSize) : -1;
}

std::string json_escape(const std::string& s) {
    std::string e;
    for (char c : s) {
        if (c == '\\' || c == '"') e += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) e += c;
    }
    return e;
}

template <class T>
T get(const std::vector<std::uint8_t>& b, std::size_t off) {
    T v{};
    if (off + sizeof(T) <= b.size()) std::memcpy(&v, &b[off], sizeof(T));
    return v;
}

std::uint32_t fnv(std::uint32_t h, const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// Kinds of an order list (up to 32 orders).
std::string order_kinds(std::uint32_t head) {
    std::string s = "[";
    int n = 0;
    for (std::uint32_t o = head; o && n < 32; ++n) {
        std::uint8_t kind = 0;
        read_block(o + 4, &kind, 1);
        if (n) s += ',';
        s += std::to_string(kind);
        std::uint32_t next = 0;
        if (!read_u32(o + 0x4A, next)) break;
        o = next;
    }
    return s + "]";
}

// The battle's configuration as the original holds it at the first step.
void write_start(DWORD tid) {
    const std::uint32_t gs = game_state();
    if (!gs) return;
    read_u32(gs + 0x14357, g_unit_table);
    std::uint8_t lp = 0, lv = 0;
    read_block(gs + kOffLocalPlayer, &lp, 1);
    read_block(gs + kOffLocalView, &lv, 1);
    std::uint16_t map_flags = 0, limit = 0;
    read_block(gs + kOffMapFlags, &map_flags, 2);
    read_block(gs + kOffUnitLimit, &limit, 2);
    std::uint32_t diff = 0, mode = 0;
    read_u32(gs + kOffAiDifficulty, diff);
    read_u32(gs + kOffGameMode, mode);
    bool known = false;
    const std::uint32_t rs = rand_state_for(tid, known);
    std::fprintf(g_out,
                 "{\"ev\":\"start\",\"game_state\":%u,\"unit_table\":%u,\"local_player\":%u,\"local_view\":%u,\"map_flags\":%u,"
                 "\"unit_limit\":%u,\"ai_difficulty\":%u,\"game_mode\":%u,\"sim_rand\":%u,\"sim_rand_known\":%d,\"players\":[",
                 gs, g_unit_table, lp, lv, map_flags, limit, diff, mode, rs, known ? 1 : 0);
    bool first = true;
    for (int p = 0; p < 10; ++p) {
        const std::uint32_t rec = gs + kOffPlayers + static_cast<std::uint32_t>(p) * kPlayerSize;
        std::vector<std::uint8_t> r(kPlayerSize);
        if (!read_block(rec, r.data(), r.size())) continue;
        const std::uint32_t active = get<std::uint32_t>(r, 0);
        const std::uint8_t controller = r[0x73];
        if (!active && !controller) continue;
        const int first_id = unit_id_of(get<std::uint32_t>(r, 0x67)), last_id = unit_id_of(get<std::uint32_t>(r, 0x6B));
        // The commander: the first unit of the player's range with a type.
        std::string commander;
        if (first_id > 0) {
            std::uint32_t def = 0;
            read_u32(g_unit_table + static_cast<std::uint32_t>(first_id) * kUnitSize + 0x92, def);
            if (def) commander = read_cstr(def + 0x20);
        }
        std::string allied;
        for (int k = 0; k < 10; ++k) allied += std::to_string(r[0x108 + static_cast<std::size_t>(k)]) + (k < 9 ? "," : "");
        std::fprintf(g_out,
                     "%s{\"slot\":%d,\"active\":%u,\"controller\":%u,\"net_id\":%u,\"state\":%u,\"first\":%d,\"last\":%d,"
                     "\"energy\":%u,\"metal\":%u,\"energy_storage\":%u,\"metal_storage\":%u,\"commander\":\"%s\",\"allied\":[%s]}",
                     first ? "" : ",", p, active, controller, get<std::uint32_t>(r, 4), r[0x146], first_id, last_id,
                     get<std::uint32_t>(r, 0x8C), get<std::uint32_t>(r, 0x98), get<std::uint32_t>(r, 0xA4),
                     get<std::uint32_t>(r, 0xA8), json_escape(commander).c_str(), allied.c_str());
        first = false;
    }
    std::fprintf(g_out, "]}\n");
}

// The simulation state after a step: generators, wind, players' resources, every unit,
// every projectile, and hashes of the map cells' features and occupancy.
void write_digest(DWORD tid) {
    const std::uint32_t gs = game_state();
    if (!gs) return;
    std::uint32_t tick = 0, brng = 0, proj_table = 0, cells = 0, mw = 0, mh = 0, wind_speed = 0;
    std::uint16_t wind_heading = 0;
    read_u32(gs + kOffGameTick, tick);
    read_u32(kBattleRngState, brng);
    read_u32(gs + kOffProjTable, proj_table);
    read_u32(gs + kOffCells, cells);
    read_u32(gs + kOffMapW, mw);
    read_u32(gs + kOffMapH, mh);
    read_block(gs + kOffWindHeading, &wind_heading, 2);
    read_u32(gs + kOffWindSpeed, wind_speed);
    bool known = false;
    const std::uint32_t rs = rand_state_for(tid, known);
    std::string line = "{\"ev\":\"digest\",\"tick\":" + std::to_string(tick) + ",\"rand\":" + std::to_string(rs) +
                       ",\"brng\":" + std::to_string(brng) + ",\"wind\":[" + std::to_string(wind_heading) + "," +
                       std::to_string(static_cast<std::int32_t>(wind_speed)) + "],\"players\":[";
    std::vector<std::uint8_t> recs(10 * kPlayerSize);
    read_block(gs + kOffPlayers, recs.data(), recs.size());
    int lo = 0x7FFFFFFF, hi = -1;
    bool first = true;
    for (int p = 0; p < 10; ++p) {
        const std::size_t o = static_cast<std::size_t>(p) * kPlayerSize;
        if (!get<std::uint32_t>(recs, o) && !recs[o + 0x73]) continue;
        const int a = unit_id_of(get<std::uint32_t>(recs, o + 0x67)), b = unit_id_of(get<std::uint32_t>(recs, o + 0x6B));
        if (a > 0 && a < lo) lo = a;
        if (b > hi) hi = b;
        line += std::string(first ? "" : ",") + "[" + std::to_string(p) + "," + std::to_string(get<std::uint32_t>(recs, o + 0x8C)) +
                "," + std::to_string(get<std::uint32_t>(recs, o + 0x98)) + "," +
                std::to_string(get<std::uint32_t>(recs, o + 0xA4)) + "," + std::to_string(get<std::uint32_t>(recs, o + 0xA8)) + "]";
        first = false;
    }
    line += "],\"units\":[";
    if (hi >= lo && g_unit_table) {
        std::vector<std::uint8_t> u(static_cast<std::size_t>(hi - lo + 1) * kUnitSize);
        read_block(g_unit_table + static_cast<std::uint32_t>(lo) * kUnitSize, u.data(), u.size());
        first = true;
        char buf[512];
        for (int id = lo; id <= hi; ++id) {
            const std::size_t o = static_cast<std::size_t>(id - lo) * kUnitSize;
            if (!get<std::uint16_t>(u, o + 0xA6)) continue;  // type index 0: empty entry
            std::snprintf(buf, sizeof buf, "%s[%d,%u,%u,%d,%d,%d,%d,%u,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,", first ? "" : ",", id,
                          get<std::uint16_t>(u, o + 0xA6), u[o + 0xFF], get<std::int32_t>(u, o + 0x6A), get<std::int32_t>(u, o + 0x6E),
                          get<std::int32_t>(u, o + 0x72), get<std::int16_t>(u, o + 0x64), get<std::uint16_t>(u, o + 0x66),
                          get<std::int16_t>(u, o + 0x68), get<std::int16_t>(u, o + 0x108), get<std::uint32_t>(u, o + 0x104),
                          u[o + 0x10E], u[o + 0x10F], get<std::uint32_t>(u, o + 0x110), u[o + 0xF6], get<std::uint16_t>(u, o + 0xB8),
                          get<std::uint16_t>(u, o + 4 + 0x14), get<std::uint16_t>(u, o + 4 + 0x1C + 0x14),
                          get<std::uint16_t>(u, o + 4 + 0x38 + 0x14));
            line += buf;
            line += order_kinds(get<std::uint32_t>(u, o + 0x5C)) + "," + order_kinds(get<std::uint32_t>(u, o + 0x60)) + "]";
            first = false;
        }
    }
    line += "],\"proj\":[";
    if (proj_table) {
        std::vector<std::uint8_t> pt(kMaxProjectiles * kProjSize);
        read_block(proj_table, pt.data(), pt.size());
        first = true;
        char buf[256];
        const std::uint32_t wbase = gs + kOffWeaponDefs;
        for (DWORD s = 0; s < kMaxProjectiles; ++s) {
            const std::size_t o = static_cast<std::size_t>(s) * kProjSize;
            const std::uint32_t w = get<std::uint32_t>(pt, o);
            if (!w) continue;
            std::snprintf(buf, sizeof buf, "%s[%lu,%u,%d,%d,%d,%d,%d,%d,%d,%d]", first ? "" : ",", s, (w - wbase) / kWeaponDefSize,
                          get<std::int32_t>(pt, o + 4), get<std::int32_t>(pt, o + 8), get<std::int32_t>(pt, o + 12),
                          get<std::int32_t>(pt, o + 0x1C), get<std::int32_t>(pt, o + 0x20), get<std::int32_t>(pt, o + 0x24),
                          unit_id_of(get<std::uint32_t>(pt, o + 0x52)), unit_id_of(get<std::uint32_t>(pt, o + 0x4E)));
            line += buf;
            first = false;
        }
    }
    std::uint32_t fh = 2166136261u, uh = 2166136261u;
    if (cells && mw && mh && mw < 4096 && mh < 4096) {
        std::vector<std::uint8_t> c(static_cast<std::size_t>(mw) * mh * kCellSize);
        read_block(cells, c.data(), c.size());
        for (std::size_t i = 0; i < static_cast<std::size_t>(mw) * mh; ++i) {
            uh = fnv(uh, &c[i * kCellSize], 4);
            fh = fnv(fh, &c[i * kCellSize + 8], 5);
        }
    }
    line += "],\"cells\":[" + std::to_string(fh) + "," + std::to_string(uh) + "]}\n";
    std::fputs(line.c_str(), g_out);
}

void write_order(const char* fn, const CONTEXT& ctx, bool build_count) {
    std::uint32_t a[8] = {};
    for (int k = 0; k < 8; ++k) read_u32(ctx.Esp + 4u * static_cast<DWORD>(k), a[k]);  // a[0] = return address
    std::string pos = "null";
    if (build_count) {
        std::fprintf(g_out, "{\"ev\":\"order\",\"fn\":\"%s\",\"tick\":%u,\"in_step\":%d,\"ret\":%u,\"kind\":%u,\"unit\":%d,\"type\":%u,"
                            "\"delta\":%d}\n",
                     fn, game_tick(), g_in_step ? 1 : 0, a[0], a[1] & 0xFF, unit_id_of(a[2]), a[3], static_cast<std::int32_t>(a[4]));
        return;
    }
    if (a[5]) {
        std::int32_t p[3] = {};
        read_block(a[5], p, sizeof p);
        pos = "[" + std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]) + "]";
    }
    std::fprintf(g_out, "{\"ev\":\"order\",\"fn\":\"%s\",\"tick\":%u,\"in_step\":%d,\"ret\":%u,\"kind\":%u,\"queued\":%u,\"unit\":%d,"
                        "\"target\":%d,\"pos\":%s,\"p0\":%d,\"p1\":%d}\n",
                 fn, game_tick(), g_in_step ? 1 : 0, a[0], a[1] & 0xFF, a[2], unit_id_of(a[3]), unit_id_of(a[4]), pos.c_str(),
                 static_cast<std::int32_t>(a[6]), static_cast<std::int32_t>(a[7]));
}

// Presses the menus' quick keys (main menu S, single player S = skirmish, skirmish S = start)
// and dismisses message boxes, until the battle starts loading.
DWORD g_pid = 0;
struct WindowSearch {
    HWND game = nullptr;
    HWND dialog = nullptr;
};
BOOL CALLBACK find_windows(HWND w, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != g_pid || !IsWindowVisible(w)) return TRUE;
    auto* s = reinterpret_cast<WindowSearch*>(lp);
    char cls[64] = {};
    GetClassNameA(w, cls, sizeof cls);
    if (std::strcmp(cls, "#32770") == 0) s->dialog = w;
    else if (!s->game) s->game = w;
    return TRUE;
}
DWORD WINAPI menu_driver(LPVOID) {
    Sleep(8000);
    for (int round = 0; round < 60 && !g_battle_loading; ++round) {
        WindowSearch s;
        EnumWindows(find_windows, reinterpret_cast<LPARAM>(&s));
        if (s.dialog) {
            PostMessageA(s.dialog, WM_COMMAND, IDOK, 0);
        } else if (s.game) {
            PostMessageA(s.game, WM_ACTIVATE, WA_ACTIVE, 0);
            PostMessageA(s.game, WM_KEYDOWN, 'S', 0x001F0001);
            PostMessageA(s.game, WM_CHAR, 's', 0x001F0001);
            PostMessageA(s.game, WM_KEYUP, 'S', 0xC01F0001);
        }
        Sleep(4000);
    }
    return 0;
}

// Returns true when the capture should stop.
bool on_breakpoint(const Options& opt, DWORD tid, CONTEXT& ctx) {
    const DWORD addr = ctx.Eip;
    std::uint32_t arg0 = 0, ret = 0;
    read_u32(ctx.Esp, ret);
    read_u32(ctx.Esp + 4, arg0);

    switch (addr) {
    case kBpArchiveOpen: {
        char path[264] = {};
        SIZE_T n = 0;
        ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(arg0)), path, sizeof(path) - 1, &n);
        std::uint32_t keep = 0;
        read_u32(ctx.Esp + 8, keep);
        std::string escaped;
        for (const char* p = path; *p; ++p) {
            if (*p == '\\' || *p == '"') escaped += '\\';
            escaped += *p;
        }
        std::fprintf(g_out, "{\"ev\":\"archive_open\",\"path\":\"%s\",\"keep_open\":%u}\n", escaped.c_str(), keep);
        std::fflush(g_out);
        break;
    }
    case kBpStepEnd:
        g_in_step = false;
        write_digest(tid);
        if (opt.max_ticks && game_tick() >= opt.max_ticks) return true;
        break;
    case kBpFrameEnd: {
        std::uint32_t brng = 0;
        std::uint8_t view = 0;
        read_u32(kBattleRngState, brng);
        if (const std::uint32_t gs = game_state()) read_block(gs + kOffLocalView, &view, 1);
        bool known = false;
        const std::uint32_t rs = rand_state_for(tid, known);
        std::fprintf(g_out, "{\"ev\":\"frame\",\"tick\":%u,\"rand\":%u,\"brng\":%u,\"view\":%u}\n", game_tick(), rs, brng, view);
        break;
    }
    case kBpShuffleRandA:
    case kBpShuffleRandB: {
        bool known = false;
        const std::uint32_t rs = rand_state_for(tid, known);
        std::fprintf(g_out, "{\"ev\":\"shuffle_rand\",\"tid\":%lu,\"at\":%lu,\"state\":%u,\"known\":%d}\n", tid, addr, rs, known ? 1 : 0);
        break;
    }
    case kBpMapPath:
        std::fprintf(g_out, "{\"ev\":\"map\",\"path\":\"%s\"}\n", json_escape(read_cstr(ctx.Eax)).c_str());
        break;
    case kBpEnqueueOrder:
        write_order("enqueue", ctx, false);
        break;
    case kBpIssueOrCancel:
        write_order("issue_or_cancel", ctx, false);
        break;
    case kBpBuildCount:
        write_order("build_count", ctx, true);
        break;
    case kBpBuildMenu: {
        std::uint32_t name = 0, unit = 0, delta = 0;
        read_u32(ctx.Esp + 4, name);
        read_u32(ctx.Esp + 8, unit);
        read_u32(ctx.Esp + 12, delta);
        std::fprintf(g_out, "{\"ev\":\"order\",\"fn\":\"build_menu\",\"tick\":%u,\"in_step\":%d,\"ret\":%u,\"name\":\"%s\",\"unit\":%d,\"delta\":%d}\n",
                     game_tick(), g_in_step ? 1 : 0, ret, json_escape(read_cstr(name)).c_str(), unit_id_of(unit),
                     static_cast<std::int32_t>(delta));
        break;
    }
    case kBpIssueSelected: {
        std::uint32_t a[6] = {};
        for (int k = 0; k < 6; ++k) read_u32(ctx.Esp + 4u + 4u * static_cast<DWORD>(k), a[k]);
        std::fprintf(g_out, "{\"ev\":\"order\",\"fn\":\"issue_selected\",\"tick\":%u,\"in_step\":%d,\"ret\":%u,\"args\":[%u,%u,%u,%u,%u,%u]}\n",
                     game_tick(), g_in_step ? 1 : 0, ret, a[0], a[1], a[2], a[3], a[4], a[5]);
        break;
    }
    case kBpAiConstructors:
    case kBpReclaimSites: {
        std::uint32_t v = 0;
        read_u32(ctx.Esp - (addr == kBpAiConstructors ? 0x2Cu : 8u), v);
        std::fprintf(g_out, "{\"ev\":\"stale\",\"what\":\"%s\",\"tick\":%u,\"in_step\":%d,\"value\":%d}\n",
                     addr == kBpAiConstructors ? "ai_site_y" : "reclaim_site_y", game_tick(), g_in_step ? 1 : 0,
                     static_cast<std::int32_t>(v));
        break;
    }
    case kBpBattleRngSeed:
        g_battle_loading = true;
        std::fprintf(g_out, "{\"ev\":\"battle_seed\",\"tid\":%lu,\"seed\":%u", tid, arg0);
        if (opt.battle_seed) {
            write_u32(ctx.Esp + 4, *opt.battle_seed);
            std::fprintf(g_out, ",\"override\":%u", *opt.battle_seed);
        }
        std::fprintf(g_out, "}\n");
        break;
    case kBpSrand:
        std::fprintf(g_out, "{\"ev\":\"srand\",\"tid\":%lu,\"seed\":%u,\"caller\":%u", tid, arg0, ret);
        if (opt.rand_seed) {
            write_u32(ctx.Esp + 4, *opt.rand_seed);
            std::fprintf(g_out, ",\"override\":%u", *opt.rand_seed);
        }
        std::fprintf(g_out, "}\n");
        break;
    case kBpSrandTd:
    case kBpRandTd: {
        const bool first = g_thread_td.find(tid) == g_thread_td.end();
        g_thread_td[tid] = ctx.Eax;
        if (first) std::fprintf(g_out, "{\"ev\":\"thread_td\",\"tid\":%lu,\"td\":%lu}\n", tid, ctx.Eax);
        if (addr == kBpRandTd) {
            ++g_rand_draws_this_step;
            if (opt.rng_calls) {
                std::uint32_t state = 0, caller = 0;
                read_u32(ctx.Eax + kOffRandState, state);
                read_u32(ctx.Esp + 0, caller);  // return address of rand() itself
                std::fprintf(g_out, "{\"ev\":\"rand\",\"tid\":%lu,\"tick\":%u,\"state\":%u,\"caller\":%u}\n", tid,
                             game_tick(), state, caller);
            }
        }
        break;
    }
    case kBpBattleRngNext: {
        ++g_battle_draws_this_step;
        if (opt.rng_calls) {
            std::uint32_t state = 0;
            read_u32(kBattleRngState, state);
            std::fprintf(g_out, "{\"ev\":\"randmod\",\"tid\":%lu,\"tick\":%u,\"n\":%u,\"state\":%u,\"caller\":%u}\n", tid,
                         game_tick(), arg0, state, ret);
        }
        break;
    }
    case kBpSimStep: {
        std::uint32_t gs = 0, tick = 0, pending = 0, brng = 0, dc = 0, hz = 0;
        read_u32(kGameStatePtr, gs);
        if (gs) {
            read_u32(gs + kOffGameTick, tick);
            read_u32(gs + kOffSimStepsPending, pending);
        }
        read_u32(kBattleRngState, brng);
        if (read_u32(kDisplayContextPtr, dc) && dc) read_u32(dc + kOffTickRateHz, hz);
        bool known = false;
        const std::uint32_t rs = rand_state_for(tid, known);
        std::fprintf(g_out,
                     "{\"ev\":\"step\",\"tid\":%lu,\"tick\":%u,\"pending\":%u,\"tick_rate_hz\":%u,\"battle_rng\":%u,"
                     "\"rand_state\":%s%u%s,\"battle_draws_prev\":%llu,\"rand_draws_prev\":%llu}\n",
                     tid, tick, pending, hz, brng, known ? "" : "\"unknown:", rs, known ? "" : "\"",
                     static_cast<unsigned long long>(g_battle_draws_this_step),
                     static_cast<unsigned long long>(g_rand_draws_this_step));
        g_battle_draws_this_step = 0;
        g_rand_draws_this_step = 0;
        if (opt.game) {
            g_in_step = true;
            if (!g_start_written) {
                g_start_written = true;
                write_start(tid);
                // From here on the simulation thread's rand() draws are wanted (the loading
                // thread's ~400,000 draws are not).
                if (opt.rng_calls && !g_bps.count(kBpRandTd)) add_bp(kBpRandTd);
            }
        }
        // Heap addresses some original behaviour depends on (stale stack words holding pointers).
        static bool layout_written = false;
        std::uint32_t unit_table = 0;
        if (gs && !layout_written && read_u32(gs + kOffUnitTable, unit_table)) {
            layout_written = true;
            std::fprintf(g_out, "{\"ev\":\"layout\",\"game_state\":%u,\"unit_table\":%u}\n", gs, unit_table);
        }
        static bool dumped = false;
        if (!opt.dump_defs.empty() && !dumped) {
            dumped = true;
            const bool ok = dump_defs(opt.dump_defs);
            std::fprintf(g_out, "{\"ev\":\"dump_defs\",\"tick\":%u,\"ok\":%s}\n", tick, ok ? "true" : "false");
        }
        std::fflush(g_out);
        if (!opt.game && opt.max_ticks && tick >= opt.max_ticks) return true;
        break;
    }
    default:
        break;
    }
    return false;
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--exe" && (v = next())) opt.exe = v;
        else if (a == "--cwd" && (v = next())) opt.cwd = v;
        else if (a == "--out" && (v = next())) opt.out = v;
        else if (a == "--battle-seed" && (v = next())) opt.battle_seed = parse_u32(v);
        else if (a == "--rand-seed" && (v = next())) opt.rand_seed = parse_u32(v);
        else if (a == "--max-ticks" && (v = next())) opt.max_ticks = parse_u32(v);
        else if (a == "--rng-calls") opt.rng_calls = true;
        else if (a == "--dump-defs" && (v = next())) opt.dump_defs = v;
        else if (a == "--game") opt.game = true;
        else if (a == "--auto-skirmish") opt.auto_skirmish = true;
        else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", a.c_str());
            return false;
        }
    }
    return !opt.exe.empty() && !opt.out.empty();
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        std::fprintf(stderr,
                     "usage: ta_capture --exe <TotalA.exe> [--cwd <dir>] --out <trace.jsonl> [--battle-seed N] "
                     "[--rand-seed N] [--max-ticks N] [--rng-calls]\n");
        return 2;
    }
    g_out = std::fopen(opt.out.c_str(), "w");
    if (!g_out) {
        std::fprintf(stderr, "cannot open %s\n", opt.out.c_str());
        return 1;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + opt.exe + "\"";
    if (!CreateProcessA(opt.exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS, nullptr,
                        opt.cwd.empty() ? nullptr : opt.cwd.c_str(), &si, &pi)) {
        std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }
    g_process = pi.hProcess;

    bool stop = false;
    bool running = true;
    while (running) {
        DEBUG_EVENT ev{};
        if (!WaitForDebugEvent(&ev, INFINITE)) break;
        DWORD status = DBG_CONTINUE;

        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT: {
            g_threads[ev.dwThreadId] = ev.u.CreateProcessInfo.hThread;
            const auto base = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(ev.u.CreateProcessInfo.lpBaseOfImage));
            std::fprintf(g_out, "{\"ev\":\"process\",\"pid\":%lu,\"image_base\":%lu}\n", ev.dwProcessId, base);
            if (base != 0x00400000) {
                std::fprintf(stderr, "unexpected image base 0x%08lX\n", base);
                stop = true;
            } else {
                if (opt.game) {
                    // Whole game: no per-draw breakpoints unless asked for (the loading thread's
                    // rand() calls are many); rand() per draw is armed at the first step.
                    for (DWORD a : {kBpArchiveOpen, kBpBattleRngSeed, kBpSrand, kBpSrandTd, kBpSimStep, kBpStepEnd, kBpFrameEnd,
                                    kBpShuffleRandA, kBpShuffleRandB, kBpMapPath, kBpEnqueueOrder, kBpIssueOrCancel, kBpBuildCount,
                                    kBpBuildMenu, kBpIssueSelected, kBpAiConstructors, kBpReclaimSites}) {
                        add_bp(a);
                    }
                    if (opt.rng_calls) add_bp(kBpBattleRngNext);
                } else {
                    for (DWORD a : {kBpArchiveOpen, kBpBattleRngSeed, kBpSrand, kBpSrandTd, kBpRandTd, kBpBattleRngNext,
                                    kBpSimStep}) {
                        add_bp(a);
                    }
                }
                if (opt.auto_skirmish) {
                    g_pid = ev.dwProcessId;
                    CreateThread(nullptr, 0, menu_driver, nullptr, 0, nullptr);
                }
            }
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            break;
        }
        case CREATE_THREAD_DEBUG_EVENT:
            g_threads[ev.dwThreadId] = ev.u.CreateThread.hThread;
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            g_threads.erase(ev.dwThreadId);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            std::fprintf(g_out, "{\"ev\":\"exit\",\"code\":%lu}\n", ev.u.ExitProcess.dwExitCode);
            running = false;
            break;
        case EXCEPTION_DEBUG_EVENT: {
            const auto& rec = ev.u.Exception.ExceptionRecord;
            const auto addr = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(rec.ExceptionAddress));
            HANDLE th = g_threads[ev.dwThreadId];
            if (rec.ExceptionCode == EXCEPTION_BREAKPOINT && g_bps.count(addr)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                GetThreadContext(th, &ctx);
                ctx.Eip = addr;  // re-execute the original instruction
                if (on_breakpoint(opt, ev.dwThreadId, ctx)) stop = true;
                write_byte(addr, g_bps[addr].original);
                ctx.EFlags |= 0x100;  // single-step, then re-arm
                SetThreadContext(th, &ctx);
                g_pending_reinsert[ev.dwThreadId] = addr;
            } else if (rec.ExceptionCode == EXCEPTION_SINGLE_STEP && g_pending_reinsert.count(ev.dwThreadId)) {
                write_byte(g_pending_reinsert[ev.dwThreadId], 0xCC);
                g_pending_reinsert.erase(ev.dwThreadId);
            } else if (rec.ExceptionCode == EXCEPTION_BREAKPOINT) {
                // Loader breakpoint or a breakpoint we did not set: just continue.
            } else {
                status = DBG_EXCEPTION_NOT_HANDLED;  // let the game's own handlers run
                if (ev.u.Exception.dwFirstChance == 0) {
                    std::fprintf(g_out, "{\"ev\":\"crash\",\"code\":%lu,\"addr\":%lu}\n", rec.ExceptionCode, addr);
                }
            }
            break;
        }
        default:
            break;
        }

        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
        if (stop && running) {
            std::fprintf(g_out, "{\"ev\":\"stopped\",\"tick\":%u}\n", game_tick());
            TerminateProcess(g_process, 0);
            stop = false;
        }
    }

    std::fclose(g_out);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
