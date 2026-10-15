#pragma once
// Calling routines of the original TotalA.exe inside a suspended child
// process (its image is mapped, none of its code has run). Shared by the
// differential harnesses.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace harness {

using u32 = std::uint32_t;

// Thread procedure: block = {fn, this, nargs, args[8], result}. Pushes the
// arguments, sets ECX = this, calls fn and restores ESP afterwards, so it works
// for cdecl, stdcall and thiscall targets alike.
inline const std::uint8_t kTrampoline[] = {
    0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57,  // push ebp; mov ebp,esp; push ebx; push esi; push edi
    0x8B, 0x5D, 0x08,                    // mov ebx,[ebp+8]
    0x8B, 0x4B, 0x08,                    // mov ecx,[ebx+8]
    0x8D, 0x73, 0x0C,                    // lea esi,[ebx+12]
    0x85, 0xC9,                          // loop: test ecx,ecx
    0x74, 0x06,                          // jz call
    0x49,                                // dec ecx
    0xFF, 0x34, 0x8E,                    // push dword [esi+ecx*4]
    0xEB, 0xF6,                          // jmp loop
    0x8B, 0x4B, 0x04,                    // call: mov ecx,[ebx+4]
    0xFF, 0x13,                          // call dword [ebx]
    0x89, 0x43, 0x2C,                    // mov [ebx+0x2C],eax
    0x8D, 0x65, 0xF4,                    // lea esp,[ebp-12]
    0x5F, 0x5E, 0x5B, 0x5D,              // pop edi; pop esi; pop ebx; pop ebp
    0x33, 0xC0,                          // xor eax,eax
    0xC2, 0x04, 0x00,                    // ret 4
};

class Remote {
public:
    ~Remote() { stop(); }

    bool start(const std::string& exe) {
        STARTUPINFOA si{};
        si.cb = sizeof si;
        std::string cmd = "\"" + exe + "\"";
        const std::string dir = exe.substr(0, exe.find_last_of("\\/"));
        if (!CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi_))
            return false;
        code_ = alloc(0x1000, PAGE_EXECUTE_READWRITE);
        block_ = alloc(0x1000, PAGE_READWRITE);
        return code_ && block_ && write(code_, kTrampoline, sizeof kTrampoline);
    }
    void stop() {
        if (pi_.hProcess) {
            TerminateProcess(pi_.hProcess, 0);
            CloseHandle(pi_.hThread);
            CloseHandle(pi_.hProcess);
        }
        pi_ = {};
    }
    u32 alloc(SIZE_T n, DWORD prot = PAGE_READWRITE) {
        return static_cast<u32>(reinterpret_cast<std::uintptr_t>(
            VirtualAllocEx(pi_.hProcess, nullptr, n, MEM_RESERVE | MEM_COMMIT, prot)));
    }
    bool write(u32 at, const void* p, std::size_t n) {
        SIZE_T k = 0;
        return WriteProcessMemory(pi_.hProcess, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(at)), p, n, &k) && k == n;
    }
    bool read(u32 at, void* p, std::size_t n) {
        SIZE_T k = 0;
        return ReadProcessMemory(pi_.hProcess, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(at)), p, n, &k) && k == n;
    }
    bool w32(u32 at, u32 v) { return write(at, &v, 4); }
    u32 r32(u32 at) {
        u32 v = 0;
        read(at, &v, 4);
        return v;
    }
    template <class T> T rd(u32 at) {
        T v{};
        read(at, &v, sizeof v);
        return v;
    }
    // Makes `n` bytes at `at` writable (import table slots of the mapped image).
    bool make_writable(u32 at, SIZE_T n) {
        DWORD old = 0;
        return VirtualProtectEx(pi_.hProcess, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(at)), n, PAGE_READWRITE, &old) != 0;
    }
    // Replaces the code at `at` with a jump to `target` (makes the page writable first).
    bool patch_jump(u32 at, u32 target) {
        DWORD old = 0;
        if (!VirtualProtectEx(pi_.hProcess, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(at)), 5, PAGE_EXECUTE_READWRITE, &old))
            return false;
        std::uint8_t j[5] = {0xE9};
        const u32 rel = target - (at + 5);
        std::memcpy(j + 1, &rel, 4);
        return write(at, j, 5);
    }
    // Calls fn(args...) with ECX = self; false on failure or timeout.
    bool call(u32 fn, u32 self, const std::vector<u32>& args, u32* result = nullptr, DWORD timeout_ms = 5000) {
        u32 blk[12] = {fn, self, static_cast<u32>(args.size())};
        for (std::size_t i = 0; i < args.size() && i < 8; ++i) blk[3 + i] = args[i];
        if (!write(block_, blk, sizeof blk)) return false;
        HANDLE th = CreateRemoteThread(pi_.hProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(static_cast<std::uintptr_t>(code_)),
                                       reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(block_)), 0, nullptr);
        if (!th) return false;
        const DWORD w = WaitForSingleObject(th, timeout_ms);
        DWORD exit_code = 1;
        GetExitCodeThread(th, &exit_code);
        CloseHandle(th);
        if (w != WAIT_OBJECT_0 || exit_code != 0) return false;
        if (result) *result = r32(block_ + 0x2C);
        return true;
    }

    // Crash reports: a vectored exception handler in the child records the exception (code,
    // address, ESP, EBP and 96 stack words at `crash_info()`) and ends the thread with exit
    // code 0xDEAD, so a failed call() can say where the original faulted.
    bool install_crash_catcher() {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        const u32 add_veh = static_cast<u32>(reinterpret_cast<std::uintptr_t>(GetProcAddress(k32, "AddVectoredExceptionHandler")));
        const u32 exit_thread = static_cast<u32>(reinterpret_cast<std::uintptr_t>(GetProcAddress(k32, "ExitThread")));
        if (!add_veh || !exit_thread) return false;
        info_ = alloc(0x1000);
        const u32 code = alloc(0x1000, PAGE_EXECUTE_READWRITE);
        if (!info_ || !code) return false;
        std::vector<std::uint8_t> s;
        auto put = [&](std::initializer_list<int> b) {
            for (int v : b) s.push_back(static_cast<std::uint8_t>(v));
        };
        auto put32 = [&](u32 v) {
            for (int i = 0; i < 4; ++i) s.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        const u32 exit_stub_at = code + 0x80;
        put({0x56, 0x57});                    // push esi; push edi
        put({0x8B, 0x44, 0x24, 0x0C});        // mov eax,[esp+12] (EXCEPTION_POINTERS*)
        put({0x8B, 0x08});                    // mov ecx,[eax] (record)
        put({0x8B, 0x11});                    // mov edx,[ecx] (code)
        put({0x89, 0x15}); put32(info_);      // mov [info],edx
        put({0x8B, 0x51, 0x0C});              // mov edx,[ecx+0C] (address)
        put({0x89, 0x15}); put32(info_ + 4);  // mov [info+4],edx
        put({0x8B, 0x48, 0x04});              // mov ecx,[eax+4] (context)
        put({0x8B, 0x91, 0xC4, 0, 0, 0});     // mov edx,[ecx+C4] (esp)
        put({0x89, 0x15}); put32(info_ + 8);  // mov [info+8],edx
        put({0x8B, 0xB1, 0xB4, 0, 0, 0});     // mov esi,[ecx+B4] (ebp)
        put({0x89, 0x35}); put32(info_ + 12); // mov [info+12],esi
        put({0x8B, 0xF2});                    // mov esi,edx
        put({0xBF}); put32(info_ + 16);       // mov edi,info+16
        put({0x51});                          // push ecx
        put({0xB9, 96, 0, 0, 0});             // mov ecx,96
        put({0xF3, 0xA5});                    // rep movsd
        put({0x59});                          // pop ecx
        put({0x8D, 0xB1, 0x9C, 0, 0, 0});     // lea esi,[ecx+9C] (edi, esi, ebx, edx, ecx, eax)
        put({0xBF}); put32(info_ + 16 + 96 * 4);  // mov edi,info+regs
        put({0x51});                          // push ecx
        put({0xB9, 6, 0, 0, 0});              // mov ecx,6
        put({0xF3, 0xA5});                    // rep movsd
        put({0x59});                          // pop ecx
        put({0xC7, 0x81, 0xB8, 0, 0, 0}); put32(exit_stub_at);  // mov [ecx+B8],exit_stub
        put({0x5F, 0x5E});                    // pop edi; pop esi
        put({0x83, 0xC8, 0xFF});              // or eax,-1 (EXCEPTION_CONTINUE_EXECUTION)
        put({0xC2, 0x04, 0x00});              // ret 4
        while (s.size() < 0x80) s.push_back(0xCC);
        put({0x68}); put32(0xDEAD);           // push 0xDEAD
        put({0xB8}); put32(exit_thread);      // mov eax,ExitThread
        put({0xFF, 0xD0});                    // call eax
        if (!write(code, s.data(), s.size())) return false;
        return call(add_veh, 0, {1, code});
    }
    struct Crash {
        u32 code = 0, address = 0, esp = 0, ebp = 0;
        u32 stack[96] = {};
        u32 edi = 0, esi = 0, ebx = 0, edx = 0, ecx = 0, eax = 0;
    };
    Crash crash_info() {
        Crash c;
        if (info_) read(info_, &c, sizeof c);
        return c;
    }
    void clear_crash_info() {
        if (info_) {
            Crash c;
            write(info_, &c, sizeof c);
        }
    }

private:
    PROCESS_INFORMATION pi_{};
    u32 code_ = 0, block_ = 0, info_ = 0;
};

} // namespace harness
