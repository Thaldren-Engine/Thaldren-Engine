// Records this processor's x87 FSINCOS results for every rotation angle the
// simulation uses, so the engine can reproduce that exact processor
// (sim::x87::FpuProfile::Captured).
//
// Built without the C runtime and stamped as a Windows 4.0 executable, so it
// runs unchanged on Windows 95/98/NT4 and later, on a 486DX, Pentium,
// Pentium Pro, K5/K6, 6x86, ... (any x87 with FSINCOS). Writes FSINCOS.CAP in
// the current directory (1,310,764 bytes).
//
// Per angle k in -32768..32767 (k != 0) it computes exactly what the game does:
//   FILD k ; FMUL qword step (0x3F1921FB54442C5A) ; FSINCOS
// with control word 0x027F (precision 53, round to nearest, all masked),
// and stores the sine and cosine as 10-byte extended values.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" int _fltused = 0;

namespace {

const unsigned kHeader = 64, kCount = 65535;
unsigned char g_out[64 + 65535 * 20];

void put32(unsigned char* p, unsigned v) {
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
    p[2] = static_cast<unsigned char>(v >> 16);
    p[3] = static_cast<unsigned char>(v >> 24);
}

void say(const char* s) {
    DWORD n = 0, len = 0;
    while (s[len]) ++len;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, len, &n, nullptr);
}

// CPUID is present if EFLAGS.ID (bit 21) can be toggled.
bool has_cpuid() {
    unsigned changed = 0;
    __asm {
        pushfd
        pop eax
        mov ecx, eax
        xor eax, 0x200000
        push eax
        popfd
        pushfd
        pop eax
        xor eax, ecx
        mov changed, eax
        push ecx
        popfd
    }
    return (changed & 0x200000) != 0;
}

} // namespace

extern "C" void __cdecl start() {
    unsigned char* h = g_out;
    const char magic[8] = {'F', 'P', 'U', 'C', 'A', 'P', '1', 0};
    for (int i = 0; i < 8; ++i) h[i] = static_cast<unsigned char>(magic[i]);
    put32(h + 8, kCount);
    if (has_cpuid()) {
        unsigned sig = 0, v0 = 0, v1 = 0, v2 = 0;
        __asm {
            push ebx
            xor eax, eax
            cpuid
            mov v0, ebx
            mov v1, edx
            mov v2, ecx
            mov eax, 1
            cpuid
            mov sig, eax
            pop ebx
        }
        put32(h + 12, sig);
        put32(h + 16, v0);
        put32(h + 20, v1);
        put32(h + 24, v2);
        put32(h + 28, 1);
    }

    // The angle step exactly as stored in the original executable.
    static const unsigned step_bits[2] = {0x54442C5Au, 0x3F1921FBu};
    unsigned short cw = 0x027F, old_cw = 0;
    unsigned char* out = g_out + kHeader;
    __asm {
        fnstcw old_cw
        fldcw cw
    }
    for (int k = -32768; k <= 32767; ++k) {
        if (k == 0) continue;
        int kk = k;
        unsigned char* o = out;
        __asm {
            mov eax, o
            fild dword ptr kk
            fmul qword ptr step_bits
            fsincos
            fstp tbyte ptr [eax + 10]
            fstp tbyte ptr [eax]
        }
        out += 20;
    }
    __asm fldcw old_cw

    HANDLE f = CreateFileA("FSINCOS.CAP", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const DWORD total = kHeader + kCount * 20;
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, g_out, total, &written, nullptr) || written != total) {
        say("fpu_capture: could not write FSINCOS.CAP\r\n");
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        ExitProcess(1);
    }
    CloseHandle(f);
    say("fpu_capture: wrote FSINCOS.CAP (65535 angles). Copy it back to the engine machine.\r\n");
    ExitProcess(0);
}
