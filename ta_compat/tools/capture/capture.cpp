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

struct Options {
    std::string exe;
    std::string cwd;
    std::string out;
    std::optional<std::uint32_t> battle_seed;
    std::optional<std::uint32_t> rand_seed;
    std::uint32_t max_ticks = 0;  // 0 = unlimited
    bool rng_calls = false;
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
                for (DWORD a : {kBpBattleRngSeed, kBpSrand, kBpSrandTd, kBpRandTd, kBpBattleRngNext, kBpSimStep}) add_bp(a);
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
