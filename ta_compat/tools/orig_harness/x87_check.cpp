// Verifies sim::x87 rotation emulation:
//  1. this CPU's FSINCOS (inline x87, precision control 53) against the
//     correctly rounded software sin/cos for every angle; prints the
//     exceptions in the format of engine/sim/src/x87_sincos_exceptions.inc;
//  2. the original rotation routine (run inside a suspended TotalA.exe) against
//     sim::x87::rotate_point_2d on every angle with assorted points.
//
// Usage: x87_check <TotalA.exe> [exceptions_out.inc]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "sim/x87_trig.h"

namespace {

using u32 = std::uint32_t;
constexpr u32 kRotate2D = 0x004B7173;  // cdecl (short angle, int* xy)

// Thread proc: block = {count, angles*, points*}; calls the rotation for each.
const std::uint8_t kBatchStub[] = {
    0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57,  // push ebp; mov ebp,esp; push ebx; push esi; push edi
    0x8B, 0x5D, 0x08,                    // mov ebx,[ebp+8]
    0x8B, 0x33,                          // mov esi,[ebx]
    0x8B, 0x7B, 0x04,                    // mov edi,[ebx+4]
    0x8B, 0x5B, 0x08,                    // mov ebx,[ebx+8]
    0x85, 0xF6,                          // loop: test esi,esi
    0x74, 0x16,                          // jz done
    0x53,                                // push ebx
    0xFF, 0x37,                          // push dword [edi]
    0xB8, 0x73, 0x71, 0x4B, 0x00,        // mov eax,0x004B7173
    0xFF, 0xD0,                          // call eax
    0x83, 0xC4, 0x08,                    // add esp,8
    0x83, 0xC7, 0x04,                    // add edi,4
    0x83, 0xC3, 0x08,                    // add ebx,8
    0x4E,                                // dec esi
    0xEB, 0xE6,                          // jmp loop
    0x5F, 0x5E, 0x5B, 0x5D,              // done: pop edi; pop esi; pop ebx; pop ebp
    0x33, 0xC0, 0xC2, 0x04, 0x00,        // xor eax,eax; ret 4
};

struct Hw80 {
    std::uint64_t mantissa;
    std::uint16_t sign_exp;
};

void hw_sincos(short k, Hw80& s, Hw80& c) {
    const double step = sim::x87::angle_step();
    unsigned short cw = 0x027F, old = 0;
    unsigned char sb[10], cb[10];
    __asm {
        fnstcw old
        fldcw cw
        fild k
        fmul step
        fsincos
        fstp tbyte ptr cb
        fstp tbyte ptr sb
        fldcw old
    }
    std::memcpy(&s.mantissa, sb, 8);
    std::memcpy(&s.sign_exp, sb + 8, 2);
    std::memcpy(&c.mantissa, cb, 8);
    std::memcpy(&c.sign_exp, cb + 8, 2);
}

bool same(const Hw80& h, const sim::x87::Ext80& e) {
    if (h.mantissa == 0 || e.mantissa == 0) return h.mantissa == e.mantissa;
    const bool neg = (h.sign_exp & 0x8000) != 0;
    const int exp = static_cast<int>(h.sign_exp & 0x7FFF) - 16383 - 63;
    return h.mantissa == e.mantissa && exp == e.exponent && neg == e.negative;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: x87_check <TotalA.exe> [exceptions_out.inc]\n");
        return 2;
    }
    // 1. Hardware FSINCOS vs correct rounding, and vs the reduction model.
    std::string inc;
    int exceptions = 0, sign_or_exp_bad = 0, not_correctly_rounded = 0;
    long long max_delta = 0;
    for (int k = -32768; k <= 32767; ++k) {
        if (k == 0) continue;
        Hw80 hs, hc;
        hw_sincos(static_cast<short>(k), hs, hc);
        const double a = static_cast<double>(k) * sim::x87::angle_step();
        sim::x87::Ext80 cs, cc, s, c;
        sim::x87::sincos_correctly_rounded(a, cs, cc);
        if (!same(hs, cs)) ++not_correctly_rounded;
        if (!same(hc, cc)) ++not_correctly_rounded;
        sim::x87::sincos_x87_model(a, s, c);
        for (int which = 0; which < 2; ++which) {
            const Hw80& h = which ? hc : hs;
            sim::x87::Ext80 e = which ? c : s;
            if (same(h, e)) continue;
            const long long delta = static_cast<long long>(h.mantissa - e.mantissa);
            e.mantissa = h.mantissa;  // exceptions only patch the mantissa
            if (!same(h, e) || delta < -128 || delta > 127) {
                if (++sign_or_exp_bad <= 20)
                    std::printf("angle %d %s: differs beyond a small mantissa delta (%lld)\n", k, which ? "cos" : "sin", delta);
                continue;
            }
            ++exceptions;
            if (delta > max_delta) max_delta = delta;
            if (-delta > max_delta) max_delta = -delta;
            // Packed: bits 0-15 angle (as uint16), bit 16 cosine, bits 17-24 delta + 128.
            char line[32];
            const unsigned code = static_cast<unsigned>(static_cast<std::uint16_t>(k)) | (which ? 1u << 16 : 0u) |
                                  (static_cast<unsigned>(delta + 128) << 17);
            std::snprintf(line, sizeof line, "0x%07X,", code);
            inc += line;
            if (exceptions % 10 == 0) inc += "\n";
        }
    }
    std::printf("FSINCOS: %d of 131070 values not correctly rounded; vs the 66-bit-pi model: %d exceptions "
                "(max delta %lld units), %d larger differences\n",
                not_correctly_rounded, exceptions, max_delta, sign_or_exp_bad);
    // The emulation as built (with its compiled-in exception table) must equal the hardware everywhere.
    int table_bad = 0;
    for (int k = -32768; k <= 32767; ++k) {
        if (k == 0) continue;
        Hw80 hs, hc;
        hw_sincos(static_cast<short>(k), hs, hc);
        sim::x87::Ext80 s, c;
        sim::x87::sincos_for_angle(static_cast<std::int16_t>(k), s, c);
        if (!same(hs, s) || !same(hc, c)) ++table_bad;
    }
    std::printf("emulated FSINCOS (with exception table) vs hardware: %d angles differ\n", table_bad);
    if (argc > 2) {
        FILE* f = nullptr;
        if (fopen_s(&f, argv[2], "wb") == 0 && f) {
            std::fputs("// FSINCOS results that differ from correct rounding by one unit in the last place.\n", f);
            std::fputs("// Packed: bits 0-15 angle, bit 16 cosine, bit 17 +1 (else -1).\n", f);
            std::fputs("// Generated by ta_compat/tools/orig_harness/x87_check.\n", f);
            std::fputs(inc.c_str(), f);
            std::fputs("\n", f);
            std::fclose(f);
        }
    }

    // 2. Original rotation vs emulation.
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::string exe = argv[1];
    std::string cmd = "\"" + exe + "\"";
    std::string dir = exe.substr(0, exe.find_last_of("\\/"));
    if (!CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi)) {
        std::fprintf(stderr, "cannot start %s\n", exe.c_str());
        return 1;
    }
    std::mt19937 g(4242);
    const std::int32_t edge[] = {0, 1, -1, 7, -7, 65536, -65536, 1 << 20, -(1 << 20), 1 << 30, -(1 << 30), INT32_MAX, INT32_MIN};
    std::vector<std::int32_t> angles;
    std::vector<std::int32_t> pts;
    for (int k = -32768; k <= 32767; ++k) {
        for (int rep = 0; rep < 8; ++rep) {
            angles.push_back(k);
            for (int c = 0; c < 2; ++c) {
                const u32 r = g() % 10;
                std::int32_t v;
                if (r < 2) v = edge[g() % (sizeof edge / sizeof edge[0])];
                else if (r < 6) v = static_cast<std::int32_t>(g() % 0x400000) - 0x200000;  // model-scale coordinates
                else if (r < 9) v = static_cast<std::int32_t>(g() % 0x10000000) - 0x8000000;
                else v = static_cast<std::int32_t>(g());
                pts.push_back(v);
            }
        }
    }
    const u32 n = static_cast<u32>(angles.size());
    const SIZE_T bytes = 16 + n * 4ull + n * 8ull;
    auto* code = VirtualAllocEx(pi.hProcess, nullptr, sizeof kBatchStub, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    auto* data = static_cast<std::uint8_t*>(VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const u32 base = static_cast<u32>(reinterpret_cast<std::uintptr_t>(data));
    const u32 block[3] = {n, base + 16, base + 16 + n * 4};
    SIZE_T k = 0;
    WriteProcessMemory(pi.hProcess, code, kBatchStub, sizeof kBatchStub, &k);
    WriteProcessMemory(pi.hProcess, data, block, sizeof block, &k);
    WriteProcessMemory(pi.hProcess, data + 16, angles.data(), n * 4ull, &k);
    WriteProcessMemory(pi.hProcess, data + 16 + n * 4ull, pts.data(), n * 8ull, &k);
    HANDLE th = CreateRemoteThread(pi.hProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(code), data, 0, nullptr);
    if (!th || WaitForSingleObject(th, 600000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "remote batch failed\n");
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    std::vector<std::int32_t> out(pts.size());
    ReadProcessMemory(pi.hProcess, data + 16 + n * 4ull, out.data(), n * 8ull, &k);
    TerminateProcess(pi.hProcess, 0);

    std::size_t bad = 0;
    for (u32 i = 0; i < n; ++i) {
        std::int32_t x = pts[2 * i], y = pts[2 * i + 1];
        sim::x87::rotate_point_2d(static_cast<std::int16_t>(angles[i]), x, y);
        if (x != out[2 * i] || y != out[2 * i + 1]) {
            if (++bad <= 20)
                std::printf("angle %d (%d,%d): orig (%d,%d) ours (%d,%d)\n", angles[i], pts[2 * i], pts[2 * i + 1], out[2 * i],
                            out[2 * i + 1], x, y);
        }
    }
    std::printf("rotation: %u calls, %zu mismatches\n", n, bad);
    return (bad || sign_or_exp_bad) ? 1 : 0;
}
