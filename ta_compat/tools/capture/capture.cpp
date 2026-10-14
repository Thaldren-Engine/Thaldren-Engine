// Golden-trace capture for the original TotalA.exe (TA 3.1, image base 0x400000).
//
// Runs the game under the Win32 debug API, places int3 breakpoints at known
// simulation addresses, optionally overrides the two RNG seeds, and writes one
// JSON object per line describing what happened. Windows / 32-bit only.
//
// Usage:
//   ta_capture --exe <TotalA.exe> [--cwd <dir>] --out <trace.jsonl>
//              [--battle-seed <u32>] [--rand-seed <u32>]
//              [--max-ticks <n>] [--rng-calls]

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
constexpr DWORD kOffSoundBank = 0x33A0B;          // {int count; ptr slots[256]; char names[256][32]; char paths[256][32]}

struct Options {
    std::string exe;
    std::string cwd;
    std::string out;
    std::optional<std::uint32_t> battle_seed;
    std::optional<std::uint32_t> rand_seed;
    std::uint32_t max_ticks = 0;  // 0 = unlimited
    bool rng_calls = false;
    std::string dump_defs;        // write definition tables at the first sim step
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

    std::vector<std::uint8_t> sbnk(8 + 256 * 4 + 256 * 32 * 2);  // +4 count, +8 slots, +0x408 names, +0x2408 paths
    read_block(gs + kOffSoundBank, sbnk.data(), sbnk.size());
    write_chunk(f, "SBNK", sbnk);

    std::vector<std::uint8_t> ft(static_cast<std::size_t>(ft_count) * 0x100);
    if (ft_ptr) read_block(ft_ptr, ft.data(), ft.size());
    write_chunk(f, "FEAT", ft);
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
    case kBpBattleRngSeed:
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
        static bool dumped = false;
        if (!opt.dump_defs.empty() && !dumped) {
            dumped = true;
            const bool ok = dump_defs(opt.dump_defs);
            std::fprintf(g_out, "{\"ev\":\"dump_defs\",\"tick\":%u,\"ok\":%s}\n", tick, ok ? "true" : "false");
        }
        std::fflush(g_out);
        if (opt.max_ticks && tick >= opt.max_ticks) return true;
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
                for (DWORD a : {kBpArchiveOpen, kBpBattleRngSeed, kBpSrand, kBpSrandTd, kBpRandTd, kBpBattleRngNext,
                                kBpSimStep}) {
                    add_bp(a);
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
