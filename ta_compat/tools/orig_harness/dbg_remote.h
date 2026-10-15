#pragma once
// Calling routines of the original TotalA.exe in a child process held at the start of its
// WinMain: the process runs under the debug API until the C runtime is initialised (static
// imports loaded, heap and per-thread data working), then its main thread is suspended before
// any of the game's own code has run. Routines are called on remote threads (see remote.h for
// the trampoline). Nothing reaches the screen: the entry points of every window, dialog,
// message-box, screen-drawing, display-mode, DirectX and sound API are replaced by traps, so a
// call into one of them ends that remote thread and is reported; faults are caught by the
// debugger (no error boxes). The process is killed when the object goes.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "remote.h"

namespace harness {

class DbgRemote {
public:
    // The game's WinMain (the main thread stops here).
    static constexpr u32 kWinMain = 0x0049E830;

    ~DbgRemote() { stop(); }

    bool start(const std::string& exe) {
        exe_ = exe;
        ready_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        loop_ = CreateThread(nullptr, 0, &DbgRemote::loop_entry, this, 0, nullptr);
        if (!loop_) return false;
        HANDLE w[2] = {ready_, loop_};
        if (WaitForMultipleObjects(2, w, FALSE, 30000) != WAIT_OBJECT_0 || !process_) return false;
        code_ = alloc(0x1000, PAGE_EXECUTE_READWRITE);
        block_ = alloc(0x1000, PAGE_READWRITE);
        traps_ = alloc(0x1000, PAGE_EXECUTE_READWRITE);
        if (!code_ || !block_ || !traps_ || !write(code_, kTrampoline, sizeof kTrampoline)) return false;
        std::vector<std::uint8_t> cc(0x1000, 0xCC);
        write(traps_, cc.data(), cc.size());
        install_traps();
        return true;
    }
    void stop() {
        if (process_) TerminateProcess(process_, 0);
        if (loop_) {
            WaitForSingleObject(loop_, 10000);
            CloseHandle(loop_);
            loop_ = nullptr;
        }
        if (ready_) CloseHandle(ready_);
        ready_ = nullptr;
        process_ = nullptr;
    }

    u32 alloc(SIZE_T n, DWORD prot = PAGE_READWRITE) {
        return static_cast<u32>(reinterpret_cast<std::uintptr_t>(VirtualAllocEx(process_, nullptr, n, MEM_RESERVE | MEM_COMMIT, prot)));
    }
    bool write(u32 at, const void* p, std::size_t n) {
        SIZE_T k = 0;
        LPVOID a = reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(at));
        if (WriteProcessMemory(process_, a, p, n, &k) && k == n) return true;
        DWORD old = 0;
        if (!VirtualProtectEx(process_, a, n, PAGE_EXECUTE_READWRITE, &old)) return false;
        const bool ok = WriteProcessMemory(process_, a, p, n, &k) && k == n;
        VirtualProtectEx(process_, a, n, old, &old);
        FlushInstructionCache(process_, a, n);
        return ok;
    }
    bool read(u32 at, void* p, std::size_t n) {
        SIZE_T k = 0;
        return ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(at)), p, n, &k) && k == n;
    }
    bool w32(u32 at, u32 v) { return write(at, &v, 4); }
    bool w16(u32 at, std::uint16_t v) { return write(at, &v, 2); }
    bool w8(u32 at, std::uint8_t v) { return write(at, &v, 1); }
    u32 r32(u32 at) {
        u32 v = 0;
        read(at, &v, 4);
        return v;
    }
    // A string in the child's memory (kept for the run).
    u32 str(const std::string& s) {
        const u32 at = alloc(s.size() + 16);
        write(at, s.c_str(), s.size() + 1);
        return at;
    }
    // Replaces a routine by "mov eax, value; ret n" (n: the bytes its callers pushed and it pops).
    bool stub(u32 at, u32 value, std::uint16_t pop) {
        std::uint8_t s[8] = {0xB8};
        std::memcpy(s + 1, &value, 4);
        if (pop) {
            s[5] = 0xC2;
            std::memcpy(s + 6, &pop, 2);
        } else {
            s[5] = 0xC3;
        }
        return write(at, s, pop ? 8 : 6);
    }
    // Calls fn(args...) with ECX = self; false on failure, timeout or a fault (reported).
    bool call(u32 fn, u32 self, const std::vector<u32>& args, u32* result = nullptr, DWORD timeout_ms = 20000) {
        u32 blk[12] = {fn, self, static_cast<u32>(args.size())};
        for (std::size_t i = 0; i < args.size() && i < 8; ++i) blk[3 + i] = args[i];
        if (!write(block_, blk, sizeof blk)) return false;
        HANDLE th = CreateRemoteThread(process_, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(static_cast<std::uintptr_t>(code_)),
                                       reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(block_)), 0, nullptr);
        if (!th) return false;
        const DWORD w = WaitForSingleObject(th, timeout_ms);
        DWORD exit_code = 1;
        GetExitCodeThread(th, &exit_code);
        if (w != WAIT_OBJECT_0) {
            std::printf("call 0x%08X: timed out\n", fn);
            TerminateThread(th, 0xDEAD);
        }
        CloseHandle(th);
        if (w != WAIT_OBJECT_0 || exit_code != 0) {
            std::lock_guard<std::mutex> g(m_);
            std::printf("call 0x%08X failed: %s\n", fn, last_fault_.c_str());
            return false;
        }
        if (result) *result = r32(block_ + 0x2C);
        return true;
    }
    HANDLE process() const { return process_; }

private:
    static DWORD WINAPI loop_entry(LPVOID self) { return static_cast<DbgRemote*>(self)->loop(); }

    DWORD loop() {
        STARTUPINFOA si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::string cmd = "\"" + exe_ + "\"";
        const std::string dir = exe_.substr(0, exe_.find_last_of("\\/"));
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        if (!CreateProcessA(exe_.c_str(), cmd.data(), nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS | CREATE_DEFAULT_ERROR_MODE, nullptr,
                            dir.c_str(), &si, &pi))
            return 1;
        std::uint8_t entry_byte = 0;
        bool armed = false;
        for (;;) {
            DEBUG_EVENT ev{};
            if (!WaitForDebugEvent(&ev, INFINITE)) break;
            DWORD status = DBG_CONTINUE;
            switch (ev.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT: {
                process_ = pi.hProcess;
                threads_[ev.dwThreadId] = ev.u.CreateProcessInfo.hThread;
                main_thread_ = ev.u.CreateProcessInfo.hThread;
                if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
                read(kWinMain, &entry_byte, 1);
                const std::uint8_t cc = 0xCC;
                armed = write(kWinMain, &cc, 1);
                break;
            }
            case CREATE_THREAD_DEBUG_EVENT: threads_[ev.dwThreadId] = ev.u.CreateThread.hThread; break;
            case EXIT_THREAD_DEBUG_EVENT: threads_.erase(ev.dwThreadId); break;
            case LOAD_DLL_DEBUG_EVENT:
                if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                process_ = nullptr;
                SetEvent(ready_);
                return 0;
            case EXCEPTION_DEBUG_EVENT: {
                const auto& rec = ev.u.Exception.ExceptionRecord;
                const u32 addr = static_cast<u32>(reinterpret_cast<std::uintptr_t>(rec.ExceptionAddress));
                HANDLE th = threads_[ev.dwThreadId];
                const bool bp = rec.ExceptionCode == EXCEPTION_BREAKPOINT || rec.ExceptionCode == 0x4000001F;
                if (bp && armed && addr == kWinMain) {
                    write(kWinMain, &entry_byte, 1);
                    armed = false;
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_FULL;
                    GetThreadContext(th, &ctx);
                    ctx.Eip = kWinMain;
                    SetThreadContext(th, &ctx);
                    SuspendThread(th);  // the game's own code never runs
                    SetEvent(ready_);
                } else if (bp && trap_names_.count(addr)) {
                    fault("blocked call to " + trap_names_[addr], ev.dwThreadId);
                    TerminateThread(th, 0xDEAD);
                } else if (bp) {
                    // the loader's breakpoint
                } else if (ev.u.Exception.dwFirstChance) {
                    status = DBG_EXCEPTION_NOT_HANDLED;
                } else {
                    char b[96];
                    std::snprintf(b, sizeof b, "fault %08lX at %08X", rec.ExceptionCode, addr);
                    fault(b, ev.dwThreadId);
                    TerminateThread(th, 0xDEAD);
                }
                break;
            }
            default: break;
            }
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
        }
        return 0;
    }
    void fault(const std::string& what, DWORD tid) {
        std::lock_guard<std::mutex> g(m_);
        char b[32];
        std::snprintf(b, sizeof b, " (thread %lu)", tid);
        last_fault_ = what + b;
    }

    // Window, dialog, drawing, display and sound entry points: in the system DLLs (same address
    // in this 32-bit process and the child) and through the game's import table.
    void install_traps() {
        u32 next = traps_;
        auto add = [&](u32 at, const std::string& name) {
            const std::uint8_t cc = 0xCC;
            if (at && write(at, &cc, 1)) trap_names_[at] = name;
        };
        const char* user32[] = {"CreateWindowExA", "CreateWindowExW", "ShowWindow", "MessageBoxA", "MessageBoxW", "MessageBoxExA", "MessageBoxExW",
                                "MessageBoxIndirectA", "DialogBoxParamA", "CreateDialogParamA", "GetDC", "GetWindowDC", "GetDCEx", "SetCursorPos",
                                "ChangeDisplaySettingsA", "ChangeDisplaySettingsExA", "SetWindowPos", "UpdateWindow", "SetForegroundWindow",
                                "ClipCursor", "MoveWindow"};
        const char* gdi32[] = {"BitBlt", "StretchBlt", "StretchDIBits", "SetDIBitsToDevice", "PatBlt"};
        HMODULE u = LoadLibraryA("user32.dll"), g = LoadLibraryA("gdi32.dll");
        for (const char* n : user32) add(static_cast<u32>(reinterpret_cast<std::uintptr_t>(GetProcAddress(u, n))), n);
        for (const char* n : gdi32) add(static_cast<u32>(reinterpret_cast<std::uintptr_t>(GetProcAddress(g, n))), n);
        // Sound and multimedia imports answer without doing anything: {name, result, bytes popped}.
        struct Quiet {
            const char* name;
            u32 result;
            std::uint16_t pop;
        };
        const Quiet quiet[] = {{"DirectSoundCreate", 0x80004005u, 12}, {"DirectInputCreateA", 0x80004005u, 16}, {"mciSendCommandA", 263, 16},
                               {"mciSendStringA", 263, 16},          {"waveOutSetVolume", 0, 8},              {"auxSetVolume", 0, 8},
                               {"PlaySoundA", 0, 12},                {"sndPlaySoundA", 0, 8},                 {"timeSetEvent", 0, 20},
                               {"midiOutSetVolume", 0, 8}};
        for (const Quiet& q : quiet) {
            const u32 slot = import_slot(q.name);
            if (!slot) continue;
            const u32 code = next;
            next += 16;
            std::uint8_t s[8] = {0xB8};
            std::memcpy(s + 1, &q.result, 4);
            s[5] = 0xC2;
            std::memcpy(s + 6, &q.pop, 2);
            write(code, s, sizeof s);
            DWORD old = 0;
            VirtualProtectEx(process_, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(slot)), 4, PAGE_READWRITE, &old);
            write(slot, &code, 4);
        }
        // DirectDraw (the display) through the game's imports: a trap.
        const char* imports[] = {"DirectDrawCreate"};
        for (const char* n : imports) {
            const u32 slot = import_slot(n);
            if (!slot) continue;
            const u32 trap = next;
            next += 16;
            trap_names_[trap] = n;
            DWORD old = 0;
            LPVOID s = reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(slot));
            VirtualProtectEx(process_, s, 4, PAGE_READWRITE, &old);
            write(slot, &trap, 4);
        }
    }
    // The import table slot of a named import of the game.
    u32 import_slot(const char* name) {
        const u32 base = 0x00400000;
        const u32 pe = r32(base + 0x3C), dir = r32(base + pe + 0x80);
        if (!dir) return 0;
        for (u32 d = base + dir;; d += 20) {
            u32 desc[5] = {};
            if (!read(d, desc, sizeof desc) || !desc[3]) break;
            const u32 names = desc[0] ? desc[0] : desc[4];
            for (u32 k = 0;; ++k) {
                const u32 thunk = r32(base + names + k * 4);
                if (!thunk) break;
                if (thunk & 0x80000000) continue;
                char fn[64] = {};
                read(base + thunk + 2, fn, sizeof fn - 1);
                if (std::strcmp(fn, name) == 0) return base + desc[4] + k * 4;
            }
        }
        return 0;
    }

    std::string exe_;
    HANDLE ready_ = nullptr, loop_ = nullptr, process_ = nullptr, main_thread_ = nullptr;
    std::map<DWORD, HANDLE> threads_;
    std::map<u32, std::string> trap_names_;
    std::mutex m_;
    std::string last_fault_;
    u32 code_ = 0, block_ = 0, traps_ = 0;
};

}  // namespace harness
