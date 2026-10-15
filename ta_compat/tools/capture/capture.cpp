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
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
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
constexpr DWORD kBpVtolRepairPatrol = 0x004152F0;  // Mission_VtolRepairPatrol: site record at [entry ESP - 0x2C]
constexpr DWORD kBpHoverClock = 0x0048A66F;      // Unit_FitToSlope after its clock call (0x4B6340): eax = clock
constexpr DWORD kBpExplTablesBegin = 0x00420620; // battle loading: the explosion tables are built (loading thread rand())
constexpr DWORD kBpExplTablesEnd = 0x00419560;   // the next loading call after them
constexpr DWORD kBpDxCheck = 0x004266A5;       // after the DirectX version check (0x4B5070): eax 0 shows a notice
constexpr DWORD kBpSoundNotice = 0x00426721;   // after the sound driver check (0x4CFF20): eax != 0 shows a notice
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
    std::uint32_t timeout_s = 900;  // the game is ended after this many seconds
    std::string script;           // the local player's scripted actions (see Script below)
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
// --watch: a data breakpoint on one unit field (set on the simulation thread at the first step).
unsigned g_watch_unit = 0, g_watch_len = 0;
int g_watch_off = 0;
DWORD g_watch_addr = 0;
bool g_watch_armed = false;

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
    if (g_watch_len) g_watch_addr = g_unit_table + g_watch_unit * kUnitSize + static_cast<DWORD>(g_watch_off);
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
    // The route search's scheduling state (search object GS+0x14207 and program-wide statics).
    {
        std::uint32_t so = 0, refresh = 0;
        std::uint16_t player_count = 0, per_player = 0;
        read_u32(gs + 0x14207, so);
        read_block(gs + 0x2A3C, &player_count, 2);
        read_block(gs + 0x1434F, &per_player, 2);
        read_u32(0x00511A38, refresh);
        std::uint8_t sb[0xC9] = {};
        if (so) read_block(so, sb, sizeof sb);
        std::int32_t scale[10] = {}, turns[10] = {};
        read_block(0x005119E8, scale, sizeof scale);
        read_block(0x00511A10, turns, sizeof turns);
        std::string cur, cred, sc, tu;
        for (int p = 0; p < 10; ++p) {
            std::uint32_t c = 0;
            std::int32_t k = 0;
            std::memcpy(&c, sb + 0x79 + 4 * p, 4);
            std::memcpy(&k, sb + 0xA1 + 4 * p, 4);
            const char* sep = p ? "," : "";
            cur += sep + std::to_string(unit_id_of(c));
            cred += sep + std::to_string(k);
            sc += sep + std::to_string(scale[p]);
            tu += sep + std::to_string(turns[p]);
        }
        std::fprintf(g_out,
                     "{\"ev\":\"route_state\",\"player_count\":%u,\"units_per_player\":%u,\"refresh\":%u,\"player\":%u,\"active\":%d,"
                     "\"cursor\":[%s],\"credit\":[%s],\"scale\":[%s],\"turns\":[%s]}\n",
                     player_count, per_player, refresh, sb[0x78], unit_id_of(*reinterpret_cast<std::uint32_t*>(sb + 0x58)), cur.c_str(),
                     cred.c_str(), sc.c_str(), tu.c_str());
    }
    // The map cells at the start (sparse): [index, unit, unit2, feature, aux, flags, metal] for
    // every cell with a unit, a feature, flags or metal.
    std::uint32_t cells = 0, mw = 0, mh = 0;
    read_u32(gs + kOffCells, cells);
    read_u32(gs + kOffMapW, mw);
    read_u32(gs + kOffMapH, mh);
    if (cells && mw && mh && mw < 4096 && mh < 4096) {
        std::vector<std::uint8_t> c(static_cast<std::size_t>(mw) * mh * kCellSize);
        read_block(cells, c.data(), c.size());
        std::string line = "{\"ev\":\"cells\",\"width\":" + std::to_string(mw) + ",\"height\":" + std::to_string(mh) + ",\"cells\":[";
        bool f = true;
        for (std::size_t i = 0; i < static_cast<std::size_t>(mw) * mh; ++i) {
            const std::uint8_t* p = &c[i * kCellSize];
            const auto unit = get<std::uint16_t>(c, i * kCellSize), unit2 = get<std::uint16_t>(c, i * kCellSize + 2);
            const auto feat = get<std::uint16_t>(c, i * kCellSize + 8), aux = get<std::uint16_t>(c, i * kCellSize + 10);
            if (!unit && !unit2 && feat == 0xFFFF && !aux && !p[12] && !p[7]) continue;
            line += std::string(f ? "" : ",") + "[" + std::to_string(i) + "," + std::to_string(unit) + "," + std::to_string(unit2) + "," +
                    std::to_string(feat) + "," + std::to_string(aux) + "," + std::to_string(p[12]) + "," + std::to_string(p[7]) + "]";
            f = false;
        }
        std::fputs((line + "]}\n").c_str(), g_out);
    }
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
            line += order_kinds(get<std::uint32_t>(u, o + 0x5C)) + "," + order_kinds(get<std::uint32_t>(u, o + 0x60));
            // Motion object: [speed, turn, flags & 7, goal vtable, route count, route search
            // tick, route flags & 15, route points...] ([] without motion).
            line += ",[";
            if (const std::uint32_t mo = get<std::uint32_t>(u, o + 0)) {
                std::vector<std::uint8_t> m(0x30);
                read_block(mo, m.data(), m.size());
                line += std::to_string(get<std::int32_t>(m, 0x20)) + "," + std::to_string(get<std::int16_t>(m, 0x24)) + "," +
                        std::to_string(m[0x2E] & 7);
                if (const std::uint32_t g = get<std::uint32_t>(m, 0)) {
                    std::vector<std::uint8_t> gb(0x68);
                    read_block(g, gb.data(), gb.size());
                    const std::int32_t count = get<std::int32_t>(gb, 0x5C);
                    line += "," + std::to_string(get<std::uint32_t>(gb, 0)) + "," + std::to_string(count) + "," +
                            std::to_string(get<std::uint32_t>(gb, 0x60)) + "," + std::to_string(gb[0x64] & 0xF);
                    for (std::int32_t k = 0; k < count && k < 20; ++k)
                        line += "," + std::to_string(get<std::uint32_t>(gb, 0xC + 4u * static_cast<std::uint32_t>(k)));
                }
            }
            // Motion velocity (+8) and tilt (+0x14), [] without motion.
            line += "],[";
            if (const std::uint32_t mo = get<std::uint32_t>(u, o + 0)) {
                std::int32_t vt[6] = {};
                read_block(mo + 8, vt, sizeof vt);
                for (int k = 0; k < 6; ++k) line += (k ? "," : "") + std::to_string(vt[k]);
            }
            line += "]]";
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

HDESK g_desktop = nullptr;
ULONGLONG g_deadline = 0;

// --- Scripted input for the local player ----------------------------------------------------
//
// A script is a text file of actions, one per line ('#' starts a comment):
//   <tick> build   <builder> <TYPE> <dx> <dz> [q]          mobile build order (intent 14) of TYPE
//   <tick> factory <factory> <TYPE> <count>                 the build menu button (+count, -count)
//   <tick> order   <unit> <intent> <target|-> <dx> <dz> [q] any intent (2 move, 3 attack, 5 unload,
//                                                           6 pick up, 7 guard, 8 repair, 9 patrol,
//                                                           10 stop, 12 reclaim, ...)
// Units are named by type: "ARMCOM" = the last finished live unit of that type of the local player,
// "ARMPW:2" = its second (by id), "ARMPW*" = every one (one call each); a target "ENEMY" = the
// enemy unit nearest to the acting unit, "-" = none. dx/dz are world units from the acting unit
// (a target's position when dx = dz = "t"). An action runs at the first frame end at or after its
// tick at which all its units exist (actions run in file order, one call per frame). Each call
// goes through the original's own routines, as the interface makes them, so the capture's order
// records see it like any other interface order: intents through Unit_ResolveMissionKind
// (0x43F0E0) then the interface's per-unit order (0x43AFC0); factories through the build menu
// command (0x419B00). The calls run on the simulation thread between steps (at the frame end
// breakpoint), from a small stub written into the game's memory.
struct ScriptAction {
    std::uint32_t tick = 0;
    std::string kind, unit, type, target;
    std::int32_t intent = 0, count = 0, dx = 0, dz = 0;
    bool queued = false, at_target = false;
    std::string line;
};
std::vector<ScriptAction> g_script;
std::size_t g_script_next = 0;
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_pending_calls;  // (unit, target) left of an "every one" action
DWORD g_stub = 0;              // stub page in the game (code at +0, data at +0x800)
DWORD g_stub_end = 0;          // its final int3
bool g_hijacking = false;      // a stub call is running
bool g_skip_frame_end = false; // the frame end breakpoint hit again after a call: pass it
CONTEXT g_saved_ctx{};
DWORD g_hijack_tid = 0;
std::int32_t g_local_resources = 0;  // start energy and metal of the local player (0: as set)

bool load_script(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[512];
    while (std::fgets(buf, sizeof buf, f)) {
        std::string l = buf;
        if (const auto h = l.find('#'); h != std::string::npos) l.resize(h);
        std::vector<std::string> w;
        std::size_t p = 0;
        while (p < l.size()) {
            while (p < l.size() && std::isspace(static_cast<unsigned char>(l[p]))) ++p;
            std::size_t q = p;
            while (q < l.size() && !std::isspace(static_cast<unsigned char>(l[q]))) ++q;
            if (q > p) w.push_back(l.substr(p, q - p));
            p = q;
        }
        if (w.size() < 3) continue;
        ScriptAction a;
        a.line = l;
        while (!a.line.empty() && (a.line.back() == '\n' || a.line.back() == '\r')) a.line.pop_back();
        a.tick = parse_u32(w[0].c_str());
        a.kind = w[1];
        a.unit = w[2];
        auto num = [&](std::size_t i) { return i < w.size() ? std::atoi(w[i].c_str()) : 0; };
        if (a.kind == "build" && w.size() >= 6) {
            a.type = w[3];
            a.dx = num(4);
            a.dz = num(5);
            a.queued = w.size() > 6 && w[6] == "q";
        } else if (a.kind == "factory" && w.size() >= 5) {
            a.type = w[3];
            a.count = num(4);
        } else if (a.kind == "order" && w.size() >= 7) {
            a.intent = num(3);
            a.target = w[4];
            a.at_target = w[5] == "t";
            a.dx = num(5);
            a.dz = num(6);
            a.queued = w.size() > 7 && w[7] == "q";
        } else {
            std::fprintf(stderr, "script: bad line: %s\n", a.line.c_str());
            continue;
        }
        g_script.push_back(a);
    }
    std::fclose(f);
    return true;
}

struct LiveUnit {
    std::uint32_t addr;
    int id;
    std::uint16_t type;
    std::int32_t x, y, z;
    int owner;
    bool finished;
};
std::vector<LiveUnit> live_units() {
    std::vector<LiveUnit> out;
    const std::uint32_t gs = game_state();
    if (!gs || !g_unit_table) return out;
    std::uint32_t last_ptr = 0;
    read_u32(gs + kOffPlayers + 9 * kPlayerSize + 0x6B, last_ptr);
    const int last = unit_id_of(last_ptr);
    if (last <= 0) return out;
    std::vector<std::uint8_t> u(static_cast<std::size_t>(last + 1) * kUnitSize);
    read_block(g_unit_table, u.data(), u.size());
    for (int id = 1; id <= last; ++id) {
        const std::size_t o = static_cast<std::size_t>(id) * kUnitSize;
        const auto type = get<std::uint16_t>(u, o + 0xA6);
        const auto flags = get<std::uint32_t>(u, o + 0x110);
        if (!type || !(flags & 0x10000000u) || get<std::int16_t>(u, o + 0x108) <= 0) continue;
        float left = 0;
        std::memcpy(&left, &u[o + 0x104], 4);
        out.push_back({g_unit_table + static_cast<std::uint32_t>(id) * kUnitSize, id, type, get<std::int32_t>(u, o + 0x6A),
                       get<std::int32_t>(u, o + 0x6E), get<std::int32_t>(u, o + 0x72), u[o + 0xFF], left == 0.0f});
    }
    return out;
}

// Unit definition index of a unit name (0: none).
std::uint16_t def_index(const std::string& name) {
    const std::uint32_t gs = game_state();
    std::uint32_t count = 0, table = 0;
    read_u32(gs + kOffUnitDefCount, count);
    read_u32(gs + kOffUnitDefs, table);
    for (std::uint32_t k = 1; k < count && k < 2000; ++k)
        if (_stricmp(read_cstr(table + k * kUnitDefSize + 0x20).c_str(), name.c_str()) == 0) return static_cast<std::uint16_t>(k);
    return 0;
}

// Units of the local player matching a selector (see the script description).
std::vector<LiveUnit> select_units(const std::vector<LiveUnit>& all, const std::string& sel, int local) {
    std::string name = sel;
    int nth = 0;
    bool every = false;
    if (const auto h = sel.find(':'); h != std::string::npos) {
        name = sel.substr(0, h);
        nth = std::atoi(sel.c_str() + h + 1);
    } else if (!sel.empty() && sel.back() == '*') {
        name = sel.substr(0, sel.size() - 1);
        every = true;
    }
    const std::uint16_t type = def_index(name);
    std::vector<LiveUnit> mine;
    for (const LiveUnit& u : all)
        if (u.owner == local && u.type == type && u.finished) mine.push_back(u);
    if (mine.empty() || every) return mine;
    if (nth > 0) return nth <= static_cast<int>(mine.size()) ? std::vector<LiveUnit>{mine[static_cast<std::size_t>(nth - 1)]} : std::vector<LiveUnit>{};
    return {mine.back()};
}

void emit_push(std::vector<std::uint8_t>& c, std::uint32_t v) {
    c.push_back(0x68);
    put_u32(c, v);
}
void emit_call(std::vector<std::uint8_t>& c, std::uint32_t target) {
    const std::uint32_t at = g_stub + static_cast<std::uint32_t>(c.size());
    c.push_back(0xE8);
    put_u32(c, target - (at + 5));
}

// Builds the stub for one call; returns false when nothing is to be done.
bool build_call(const ScriptAction& a, const LiveUnit& u, const LiveUnit* target, std::vector<std::uint8_t>& code,
                std::vector<std::uint8_t>& data) {
    const std::uint32_t d = g_stub + 0x800;  // data: +0 position (3 ints), +12 resolved kind, +16 name
    code.clear();
    data.assign(0x60, 0);
    std::int32_t pos[3] = {u.x + (a.dx << 16), u.y, u.z + (a.dz << 16)};
    if (a.at_target && target) pos[0] = target->x, pos[1] = target->y, pos[2] = target->z;
    if (a.kind == "factory") {
        const std::string n = a.type;
        std::memcpy(&data[16], n.c_str(), std::min<std::size_t>(n.size(), 40));
        emit_push(code, static_cast<std::uint32_t>(a.count));
        emit_push(code, u.addr);
        emit_push(code, d + 16);
        emit_call(code, kBpBuildMenu);
    } else {
        std::uint32_t intent = static_cast<std::uint32_t>(a.intent), p0 = 0;
        if (a.kind == "build") {
            intent = 14;
            p0 = def_index(a.type);
            if (!p0) return false;
            pos[0] = (pos[0] >> 20) << 20;  // whole cells, as placement gives
            pos[2] = (pos[2] >> 20) << 20;
        }
        std::memcpy(&data[0], pos, 12);
        const std::uint32_t t = target ? target->addr : 0;
        // Unit_ResolveMissionKind(&kind, intent, unit, target, &pos)
        emit_push(code, d);
        emit_push(code, t);
        emit_push(code, u.addr);
        emit_push(code, intent);
        emit_push(code, d + 12);
        emit_call(code, 0x0043F0E0);
        // movzx eax, byte [d + 12]; test eax, eax; jz end
        code.insert(code.end(), {0x0F, 0xB6, 0x05});
        put_u32(code, d + 12);
        code.insert(code.end(), {0x85, 0xC0, 0x74, 0x00});
        const std::size_t jz = code.size() - 1;
        // the interface's per-unit order (kind, queued, unit, target, &pos, p0, p1)
        emit_push(code, 0);
        emit_push(code, p0);
        emit_push(code, d);
        emit_push(code, t);
        emit_push(code, u.addr);
        emit_push(code, a.queued ? 1u : 0u);  // queued (shift held)
        code.push_back(0x50);                 // push eax (the kind)
        emit_call(code, kBpIssueOrCancel);
        code[jz] = static_cast<std::uint8_t>(code.size() - (jz + 1));
    }
    code.push_back(0xCC);
    return true;
}

// At a frame end on the simulation thread: start the next due script call (returns true when
// the thread now runs a stub).
bool script_step(DWORD tid, CONTEXT& ctx) {
    if (!g_stub || g_script_next >= g_script.size()) return false;
    const ScriptAction& a = g_script[g_script_next];
    if (game_tick() < a.tick) return false;
    std::uint8_t local = 0;
    read_block(game_state() + kOffLocalPlayer, &local, 1);
    const std::vector<LiveUnit> all = live_units();
    std::vector<LiveUnit> units = select_units(all, a.unit, local);
    if (units.empty()) return false;  // wait for it
    const LiveUnit* target = nullptr;
    std::vector<LiveUnit> targets;
    if (!a.target.empty() && a.target != "-") {
        if (a.target == "ENEMY") {
            long long best = -1;
            for (const LiveUnit& e : all) {
                if (e.owner == local || e.owner >= 10) continue;
                const long long dx = (e.x - units[0].x) >> 16, dz = (e.z - units[0].z) >> 16, dd = dx * dx + dz * dz;
                if (best < 0 || dd < best) {
                    best = dd;
                    targets = {e};
                }
            }
        } else {
            targets = select_units(all, a.target, local);
        }
        if (targets.empty()) return false;
        target = &targets[0];
    }
    // "every one": one unit per frame, the selector's units in id order.
    static std::size_t sub = 0;
    if (sub >= units.size()) sub = 0;
    const LiveUnit u = units[sub];
    std::vector<std::uint8_t> code, data;
    const bool ok = build_call(a, u, target, code, data);
    if (++sub >= units.size() || !ok) {
        sub = 0;
        ++g_script_next;
    }
    std::fprintf(g_out, "{\"ev\":\"script\",\"tick\":%u,\"action\":\"%s\",\"unit\":%d,\"target\":%d,\"ok\":%d}\n", game_tick(),
                 json_escape(a.line).c_str(), u.id, target ? target->id : 0, ok ? 1 : 0);
    if (!ok) return false;
    SIZE_T n = 0;
    WriteProcessMemory(g_process, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(g_stub)), code.data(), code.size(), &n);
    WriteProcessMemory(g_process, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(g_stub + 0x800)), data.data(), data.size(), &n);
    FlushInstructionCache(g_process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(g_stub)), code.size());
    g_stub_end = g_stub + static_cast<DWORD>(code.size()) - 1;
    g_saved_ctx = ctx;  // Eip = the frame end breakpoint
    g_hijacking = true;
    g_hijack_tid = tid;
    ctx.Eip = g_stub;
    return true;
}

// --- Keeping the game off the user's display -------------------------------------------------
//
// The game is started with "-d", which makes it draw into a GDI DIB section in memory instead
// of taking DirectDraw (the display context's DirectDraw flag is the inverse of that option's
// bit 0, see Gfx_RecreateDisplaySurfaces 0x4B5510), on its own hidden desktop. Imports that
// could still reach the user's machine are replaced, at the loader breakpoint (imports bound,
// entry point not run), by stubs returning a fixed value: DirectDraw (the movie player creates
// one of its own), DirectSound and other sound output, the CD audio commands, the system
// volume, the cursor position, the foreground window, hot keys, system parameters (the game
// switches mouse trails off) and message boxes. A monitor thread watches the user's display
// mode and desktop and ends the game at the first change.
struct ImportStub {
    const char* name;
    std::uint32_t result;
    std::uint16_t arg_bytes;
};
constexpr ImportStub kImportStubs[] = {
    {"DirectDrawCreate", 0x80004005u, 12},  {"DirectSoundCreate", 0x88780078u, 12}, {"mciSendStringA", 263, 16},
    {"PlaySoundA", 0, 12},                  {"waveOutSetVolume", 0, 8},             {"auxSetVolume", 0, 8},
    {"SetCursorPos", 1, 8},                 {"SetForegroundWindow", 1, 4},          {"RegisterHotKey", 1, 16},
    {"SystemParametersInfoA", 1, 16},       {"MessageBoxA", 1, 16},
};
bool g_imports_patched = false;

void patch_imports() {
    g_imports_patched = true;
    const DWORD base = 0x00400000;
    std::uint32_t e_lfanew = 0, imp_rva = 0;
    read_u32(base + 0x3C, e_lfanew);
    read_u32(base + e_lfanew + 0x80, imp_rva);  // optional header data directory 1 (imports)
    const auto page = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(
        VirtualAllocEx(g_process, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)));
    DWORD next = page;
    for (DWORD d = base + imp_rva;; d += 20) {
        std::uint32_t desc[5] = {};
        if (!read_block(d, desc, sizeof desc) || !desc[3]) break;
        const std::string dll = read_cstr(base + desc[3]);
        const DWORD names = base + (desc[0] ? desc[0] : desc[4]), iat = base + desc[4];
        for (DWORD k = 0;; ++k) {
            std::uint32_t thunk = 0;
            if (!read_u32(names + 4 * k, thunk) || !thunk) break;
            if (thunk & 0x80000000u) continue;  // by ordinal
            const std::string fn = read_cstr(base + thunk + 2);
            for (const ImportStub& s : kImportStubs) {
                if (fn != s.name) continue;
                // mov eax, result ; ret arg_bytes
                std::uint8_t code[8] = {0xB8, 0, 0, 0, 0, 0xC2, 0, 0};
                std::memcpy(code + 1, &s.result, 4);
                std::memcpy(code + 6, &s.arg_bytes, 2);
                SIZE_T n = 0;
                WriteProcessMemory(g_process, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(next)), code, sizeof code, &n);
                DWORD old = 0;
                VirtualProtectEx(g_process, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(iat + 4 * k)), 4, PAGE_READWRITE, &old);
                write_u32(iat + 4 * k, next);
                std::fprintf(g_out, "{\"ev\":\"import_stub\",\"dll\":\"%s\",\"name\":\"%s\",\"stub\":%lu}\n", json_escape(dll).c_str(),
                             s.name, next);
                next += 8;
            }
        }
    }
    FlushInstructionCache(g_process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(page)), 0x1000);
    std::fflush(g_out);
}

volatile LONG g_safety_stop = 0;
DWORD g_game_pid = 0;
DWORD WINAPI safety_monitor(LPVOID) {
    DEVMODEA start{};
    start.dmSize = sizeof start;
    EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &start);
    while (g_process) {
        DEVMODEA now{};
        now.dmSize = sizeof now;
        EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &now);
        bool bad = now.dmPelsWidth != start.dmPelsWidth || now.dmPelsHeight != start.dmPelsHeight ||
                   now.dmBitsPerPel != start.dmBitsPerPel || now.dmDisplayFrequency != start.dmDisplayFrequency;
        // Any window of the game on the user's desktop (EnumWindows lists this thread's desktop).
        struct Find {
            DWORD pid;
            bool found;
        } f{g_game_pid, false};
        EnumWindows(
            [](HWND w, LPARAM lp) -> BOOL {
                auto* x = reinterpret_cast<Find*>(lp);
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                if (pid == x->pid) x->found = true;
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&f));
        bad = bad || f.found;
        if (bad && g_process) {
            InterlockedExchange(&g_safety_stop, 1);
            TerminateProcess(g_process, 0x5AFE);
            std::fprintf(stderr, "SAFETY STOP: %s\n", f.found ? "a game window appeared on the user's desktop" : "the display mode changed");
            return 1;
        }
        Sleep(50);
    }
    return 0;
}

bool another_game_running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL ok = Process32First(snap, &pe); ok && !found; ok = Process32Next(snap, &pe))
        found = _stricmp(pe.szExeFile, "TotalA.exe") == 0;
    CloseHandle(snap);
    return found;
}

DWORD WINAPI watchdog(LPVOID) {
    while (GetTickCount64() < g_deadline) Sleep(1000);
    if (g_process) TerminateProcess(g_process, 0xDEAD);
    return 0;
}

// The game's frame as it drew it (GDI mode: the DIB section described at display context + 0x50:
// width, height, pitch, pixels; palette at + 0x214), written as an 8-bit BMP.
std::string g_frames_dir;
bool dump_frame(const std::string& path) {
    std::uint32_t dc = 0;
    if (!read_u32(kDisplayContextPtr, dc) || !dc) return false;
    std::uint32_t s[4] = {};
    if (!read_block(dc + 0x50, s, sizeof s) || !s[3] || !s[0] || !s[1] || s[0] > 4096 || s[1] > 4096 || s[2] < s[0]) return false;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(s[2]) * s[1]);
    if (!read_block(s[3], px.data(), px.size())) return false;
    std::uint8_t pal[1024] = {};
    read_block(dc + 0x214, pal, sizeof pal);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const std::uint32_t row = (s[0] + 3) & ~3u, off = 14 + 40 + 1024, size = off + row * s[1];
    std::vector<std::uint8_t> h;
    h.push_back('B');
    h.push_back('M');
    put_u32(h, size);
    put_u32(h, 0);
    put_u32(h, off);
    put_u32(h, 40);
    put_u32(h, s[0]);
    put_u32(h, static_cast<std::uint32_t>(-static_cast<std::int32_t>(s[1])));  // top-down
    h.push_back(1);
    h.push_back(0);
    h.push_back(8);
    h.push_back(0);
    for (int k = 0; k < 6; ++k) put_u32(h, k == 1 ? row * s[1] : 0);
    std::fwrite(h.data(), 1, h.size(), f);
    for (int i = 0; i < 256; ++i) {
        const std::uint8_t q[4] = {pal[i * 4 + 2], pal[i * 4 + 1], pal[i * 4], 0};  // PALETTEENTRY r,g,b -> BGR
        std::fwrite(q, 1, 4, f);
    }
    std::vector<std::uint8_t> line(row, 0);
    for (std::uint32_t y = 0; y < s[1]; ++y) {
        std::memcpy(line.data(), &px[static_cast<std::size_t>(y) * s[2]], s[0]);
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
    return true;
}

// Presses the menus' quick keys (main menu S, single player S = skirmish, skirmish S = start)
// and dismisses message boxes, until the battle starts loading.
DWORD g_pid = 0;
struct WindowSearch {
    std::vector<HWND> game;
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
    else s->game.push_back(w);
    return TRUE;
}
void post_key(HWND w, WPARAM vk, WPARAM ch) {
    const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC)) << 16;
    PostMessageA(w, WM_KEYDOWN, vk, scan | 1);
    if (ch) PostMessageA(w, WM_CHAR, ch, scan | 1);
    PostMessageA(w, WM_KEYUP, vk, scan | 1 | 0xC0000000);
}
DWORD WINAPI menu_driver(LPVOID) {
    Sleep(8000);
    // The game's "DirectX version" notice is skipped at its check (kBpDxCheck). Then S until
    // the battle loads (main menu S, single player S = skirmish, skirmish S = start). Keys go
    // to the process's visible top-level windows.
    for (int round = 0; round < 90 && !g_battle_loading; ++round) {
        WindowSearch s;
        EnumDesktopWindows(g_desktop, find_windows, reinterpret_cast<LPARAM>(&s));
        if (!g_frames_dir.empty()) dump_frame(g_frames_dir + "\\menu_" + std::to_string(round) + ".bmp");
        if (s.dialog) {
            PostMessageA(s.dialog, WM_COMMAND, IDOK, 0);
        } else {
            for (HWND w : s.game) {
                PostMessageA(w, WM_ACTIVATE, WA_ACTIVE, 0);
                post_key(w, 'S', 's');
            }
        }
        Sleep(3000);
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
        if (g_skip_frame_end) {  // back from a script call: this frame end is already logged
            g_skip_frame_end = false;
            if (script_step(tid, ctx)) {}  // the next call of an "every unit" action, if any
            break;
        }
        std::uint32_t brng = 0;
        std::uint8_t view = 0;
        std::uint16_t map_flags = 0;
        read_u32(kBattleRngState, brng);
        if (const std::uint32_t gs = game_state()) {
            read_block(gs + kOffLocalView, &view, 1);
            read_block(gs + kOffMapFlags, &map_flags, 2);  // line-of-sight options (console toggles)
        }
        bool known = false;
        const std::uint32_t rs = rand_state_for(tid, known);
        std::fprintf(g_out, "{\"ev\":\"frame\",\"tick\":%u,\"rand\":%u,\"brng\":%u,\"view\":%u,\"map_flags\":%u}\n", game_tick(), rs, brng,
                     view, map_flags);
        script_step(tid, ctx);
        break;
    }
    case kBpVtolRepairPatrol: {
        // The air repair patrol's metal site record lives in its frame; its address becomes the
        // order's event word when a site is found (a run-varying stack address: an input).
        std::fprintf(g_out, "{\"ev\":\"stale\",\"what\":\"air_repair_site\",\"tick\":%u,\"in_step\":%d,\"value\":%u}\n", game_tick(),
                     g_in_step ? 1 : 0, ctx.Esp - 0x2Cu);
        break;
    }
    case kBpHoverClock:
        // The hover bob's wall clock in steps (0x4B6340 from Unit_FitToSlope): an input.
        std::fprintf(g_out, "{\"ev\":\"stale\",\"what\":\"clock\",\"tick\":%u,\"in_step\":%d,\"value\":%u}\n", game_tick(), g_in_step ? 1 : 0,
                     ctx.Eax);
        break;
    case kBpExplTablesBegin:
    case kBpExplTablesEnd: {
        bool known = false;
        const std::uint32_t rs = rand_state_for(tid, known);
        std::fprintf(g_out, "{\"ev\":\"loading_rand\",\"at\":\"%s\",\"tid\":%lu,\"state\":%u,\"known\":%d}\n",
                     addr == kBpExplTablesBegin ? "explosion_tables" : "after_explosion_tables", tid, rs, known ? 1 : 0);
        break;
    }
    case kBpShuffleRandA:
    case kBpShuffleRandB: {
        bool known = false;
        const std::uint32_t rs = rand_state_for(tid, known);
        std::fprintf(g_out, "{\"ev\":\"shuffle_rand\",\"tid\":%lu,\"at\":%lu,\"state\":%u,\"known\":%d}\n", tid, addr, rs, known ? 1 : 0);
        break;
    }
    case kBpSoundNotice:
        ctx.Eax = 0;  // no "No sound driver" notice (sound output is stubbed: start-up interface only)
        break;
    case kBpDxCheck:
        ctx.Eax = 1;  // version accepted: no notice to answer (start-up interface only)
        break;
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
        if (g_local_resources > 0) {
            // As if the player had picked these start amounts in the skirmish menu: the skirmish
            // player table (GS+0x29A0, 0x18 bytes per slot: +0xC metal, +0x10 energy).
            std::uint32_t gs = game_state(), table = 0;
            std::uint8_t local = 0;
            read_u32(gs + 0x29A0, table);
            read_block(gs + kOffLocalPlayer, &local, 1);
            if (table && local < 10) {
                write_u32(table + local * 0x18u + 0xC, static_cast<std::uint32_t>(g_local_resources));
                write_u32(table + local * 0x18u + 0x10, static_cast<std::uint32_t>(g_local_resources));
                std::fprintf(g_out, "{\"ev\":\"local_resources\",\"player\":%u,\"amount\":%d}\n", local, g_local_resources);
            }
        }
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
        else if (a == "--timeout" && (v = next())) opt.timeout_s = parse_u32(v);
        else if (a == "--script" && (v = next())) opt.script = v;
        else if (a == "--frames" && (v = next())) g_frames_dir = v;
        else if (a == "--watch" && (v = next())) {
            // unit id : field offset : length (1, 2 or 4): log every write to that unit field
            unsigned id = 0, off = 0, len = 0;
            if (std::sscanf(v, "%u:%i:%u", &id, &off, &len) == 3 && (len == 1 || len == 2 || len == 4)) {
                g_watch_unit = id;
                g_watch_off = off;
                g_watch_len = len;
            }
        }
        else if (a == "--local-resources" && (v = next())) g_local_resources = static_cast<std::int32_t>(parse_u32(v));
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

    if (!opt.script.empty() && !load_script(opt.script)) {
        std::fprintf(stderr, "cannot read the script %s\n", opt.script.c_str());
        return 1;
    }
    // Only one copy of the game can run (a second one exits at once): refuse to start next to one.
    if (another_game_running()) {
        std::fprintf(stderr, "a TotalA.exe is already running (one at a time): not started\n");
        std::fprintf(g_out, "{\"ev\":\"refused\",\"why\":\"another TotalA.exe is running\"}\n");
        std::fclose(g_out);
        return 3;
    }
    // The game runs on its own hidden desktop: nothing appears on the user's screen, and
    // error boxes are suppressed (the debugger records crashes instead).
    char desk_name[64];
    std::snprintf(desk_name, sizeof desk_name, "ta_capture_%lu", GetCurrentProcessId());
    g_desktop = CreateDesktopA(desk_name, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!g_desktop) {
        std::fprintf(stderr, "CreateDesktop failed: %lu\n", GetLastError());
        return 1;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.lpDesktop = desk_name;
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + opt.exe + "\" -d";  // -d: GDI drawing into memory, no DirectDraw
    if (!CreateProcessA(opt.exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS, nullptr,
                        opt.cwd.empty() ? nullptr : opt.cwd.c_str(), &si, &pi)) {
        std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        CloseDesktop(g_desktop);
        return 1;
    }
    g_process = pi.hProcess;
    g_game_pid = pi.dwProcessId;
    CreateThread(nullptr, 0, safety_monitor, nullptr, 0, nullptr);
    // Watchdog: the game is ended after --timeout seconds whatever happens.
    g_deadline = GetTickCount64() + static_cast<ULONGLONG>(opt.timeout_s) * 1000u;
    CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);

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
                                    kBpBuildMenu, kBpIssueSelected, kBpAiConstructors, kBpReclaimSites, kBpVtolRepairPatrol,
                                    kBpHoverClock, kBpExplTablesBegin, kBpExplTablesEnd}) {
                        add_bp(a);
                    }
                    if (opt.rng_calls) add_bp(kBpBattleRngNext);
                    // A page for the script's call stubs.
                    if (!opt.script.empty())
                        g_stub = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(
                            VirtualAllocEx(g_process, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)));
                } else {
                    for (DWORD a : {kBpArchiveOpen, kBpBattleRngSeed, kBpSrand, kBpSrandTd, kBpRandTd, kBpBattleRngNext,
                                    kBpSimStep}) {
                        add_bp(a);
                    }
                }
                if (opt.auto_skirmish) {
                    g_pid = ev.dwProcessId;
                    add_bp(kBpDxCheck);
                    add_bp(kBpSoundNotice);
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
                ctx.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
                GetThreadContext(th, &ctx);
                ctx.Eip = addr;  // re-execute the original instruction
                if (on_breakpoint(opt, ev.dwThreadId, ctx)) stop = true;
                if (g_watch_addr && !g_watch_armed && g_start_written) {
                    // A data breakpoint on the watched field (writes), on the simulation thread.
                    g_watch_armed = true;
                    ctx.Dr0 = g_watch_addr;
                    ctx.Dr7 = (ctx.Dr7 & ~0xF0003u) | 1u | (1u << 16) | (static_cast<DWORD>(g_watch_len - 1) << 18);
                }
                write_byte(addr, g_bps[addr].original);
                ctx.EFlags |= 0x100;  // single-step, then re-arm
                SetThreadContext(th, &ctx);
                g_pending_reinsert[ev.dwThreadId] = addr;
            } else if (rec.ExceptionCode == EXCEPTION_SINGLE_STEP) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
                GetThreadContext(th, &ctx);
                if (ctx.Dr6 & 1u) {
                    // The watched field was written: the writer (the instruction before EIP) and the value.
                    std::uint32_t v = 0;
                    read_block(g_watch_addr, &v, g_watch_len);
                    std::uint32_t ret = 0;
                    read_u32(ctx.Esp, ret);
                    std::fprintf(g_out, "{\"ev\":\"watch\",\"tick\":%u,\"in_step\":%d,\"eip\":%lu,\"stack_top\":%u,\"value\":%u}\n", game_tick(),
                                 g_in_step ? 1 : 0, ctx.Eip, ret, v);
                    ctx.Dr6 = 0;
                    SetThreadContext(th, &ctx);
                }
                if (g_pending_reinsert.count(ev.dwThreadId)) {
                    write_byte(g_pending_reinsert[ev.dwThreadId], 0xCC);
                    g_pending_reinsert.erase(ev.dwThreadId);
                }
            } else if (rec.ExceptionCode == EXCEPTION_BREAKPOINT && g_hijacking && addr == g_stub_end &&
                       ev.dwThreadId == g_hijack_tid) {
                // A script call returned: the thread goes back to the frame end breakpoint,
                // which then only passes (it is logged already).
                g_hijacking = false;
                g_skip_frame_end = true;
                SetThreadContext(th, &g_saved_ctx);
            } else if (rec.ExceptionCode == EXCEPTION_BREAKPOINT) {
                // Loader breakpoint (imports bound, entry point not run yet: the moment to replace
                // the imports that reach outside) or a breakpoint we did not set: continue.
                if (!g_imports_patched) patch_imports();
            } else {
                status = DBG_EXCEPTION_NOT_HANDLED;  // let the game's own handlers run
                if (ev.u.Exception.dwFirstChance == 0) {
                    std::fprintf(g_out, "{\"ev\":\"crash\",\"code\":%lu,\"addr\":%lu}\n", rec.ExceptionCode, addr);
                } else {
                    std::fprintf(g_out, "{\"ev\":\"exception\",\"code\":%lu,\"addr\":%lu,\"tid\":%lu,\"tick\":%u}\n", rec.ExceptionCode, addr,
                                 ev.dwThreadId, game_tick());
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

    if (g_safety_stop) std::fprintf(g_out, "{\"ev\":\"safety_stop\"}\n");
    std::fclose(g_out);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_process = nullptr;
    if (g_safety_stop) {
        CloseDesktop(g_desktop);
        return 4;
    }
    CloseDesktop(g_desktop);
    return 0;
}
