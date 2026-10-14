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

#include "sim/x87_math.h"
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

// Calls a cdecl routine `fn` with `nargs` dword arguments for each argument
// tuple in `args`; collects EAX (int_result) or ST0 stored as a double.
bool remote_batch(HANDLE proc, u32 fn, int nargs, bool int_result, const std::vector<u32>& args, std::vector<u32>& out) {
    std::vector<std::uint8_t> c = {0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57, 0x8B, 0x5D, 0x08, 0x8B, 0x33, 0x8B, 0x7B, 0x04, 0x8B, 0x5B, 0x08};
    const std::size_t loop = c.size();
    c.insert(c.end(), {0x85, 0xF6, 0x74, 0x00});  // test esi,esi; jz done (patched)
    const std::size_t jz_at = c.size() - 1;
    for (int k = nargs - 1; k >= 0; --k) c.insert(c.end(), {0xFF, 0x77, static_cast<std::uint8_t>(4 * k)});  // push [edi+4k]
    c.push_back(0xB8);
    for (int i = 0; i < 4; ++i) c.push_back(static_cast<std::uint8_t>(fn >> (8 * i)));
    c.insert(c.end(), {0xFF, 0xD0, 0x83, 0xC4, static_cast<std::uint8_t>(4 * nargs)});  // call eax; add esp,4n
    if (int_result) c.insert(c.end(), {0x89, 0x03, 0x83, 0xC3, 0x04});                // mov [ebx],eax; add ebx,4
    else c.insert(c.end(), {0xDD, 0x1B, 0x83, 0xC3, 0x08});                           // fstp qword [ebx]; add ebx,8
    c.insert(c.end(), {0x83, 0xC7, static_cast<std::uint8_t>(4 * nargs), 0x4E, 0xEB, 0x00});  // add edi; dec esi; jmp loop
    c.back() = static_cast<std::uint8_t>(static_cast<int>(loop) - static_cast<int>(c.size()));
    c[jz_at] = static_cast<std::uint8_t>(c.size() - (jz_at + 1));
    c.insert(c.end(), {0x5F, 0x5E, 0x5B, 0x5D, 0x33, 0xC0, 0xC2, 0x04, 0x00});
    const u32 n = static_cast<u32>(args.size() / static_cast<std::size_t>(nargs));
    const std::size_t out_words = static_cast<std::size_t>(n) * (int_result ? 1 : 2);
    const SIZE_T bytes = 16 + args.size() * 4 + out_words * 4;
    auto* code = VirtualAllocEx(proc, nullptr, c.size(), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    auto* data = static_cast<std::uint8_t*>(VirtualAllocEx(proc, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!code || !data) return false;
    const u32 base = static_cast<u32>(reinterpret_cast<std::uintptr_t>(data));
    const u32 block[3] = {n, base + 16, base + 16 + static_cast<u32>(args.size() * 4)};
    SIZE_T k = 0;
    WriteProcessMemory(proc, code, c.data(), c.size(), &k);
    WriteProcessMemory(proc, data, block, sizeof block, &k);
    WriteProcessMemory(proc, data + 16, args.data(), args.size() * 4, &k);
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(code), data, 0, nullptr);
    if (!th || WaitForSingleObject(th, 600000) != WAIT_OBJECT_0) return false;
    CloseHandle(th);
    out.resize(out_words);
    ReadProcessMemory(proc, data + 16 + args.size() * 4, out.data(), out_words * 4, &k);
    VirtualFreeEx(proc, code, 0, MEM_RELEASE);
    VirtualFreeEx(proc, data, 0, MEM_RELEASE);
    return true;
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

    // 3. Scaled atan2 (Math_Atan2Scaled 0x004B715A, cdecl (y, x) -> int).
    auto rnd_int = [&]() -> std::int32_t {
        const u32 r = g() % 10;
        if (r < 1) return edge[g() % (sizeof edge / sizeof edge[0])];
        if (r < 3) return static_cast<std::int32_t>(g() % 2001) - 1000;
        if (r < 6) return static_cast<std::int32_t>(g() % 0x2000000) - 0x1000000;
        return static_cast<std::int32_t>(g());
    };
    std::vector<u32> aargs;
    std::size_t near_ties = 0;
    double k_atan;
    std::memcpy(&k_atan, &sim::x87::kAtanScaleBits, 8);
    for (int i = 0; i < 4000000; ++i) {
        const std::int32_t y = rnd_int(), x = rnd_int();
        aargs.push_back(static_cast<u32>(y));
        aargs.push_back(static_cast<u32>(x));
    }
    // Inputs whose scaled angle lies very close to a rounding tie (exercise the exact path).
    for (int tries = 0; near_ties < 20000 && tries < 400000000; ++tries) {
        const std::int32_t y = static_cast<std::int32_t>(g() % 200001) - 100000, x = static_cast<std::int32_t>(g() % 200001) - 100000;
        const double v = std::atan2(static_cast<double>(y), static_cast<double>(x)) * k_atan;
        if (std::fabs(v - std::floor(v) - 0.5) < 2e-7) {
            aargs.push_back(static_cast<u32>(y));
            aargs.push_back(static_cast<u32>(x));
            ++near_ties;
        }
    }
    std::vector<u32> aout;
    if (!remote_batch(pi.hProcess, 0x004B715A, 2, true, aargs, aout)) {
        std::fprintf(stderr, "remote atan2 batch failed\n");
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    std::size_t abad = 0;
    for (std::size_t i = 0; i < aout.size(); ++i) {
        const std::int32_t y = static_cast<std::int32_t>(aargs[2 * i]), x = static_cast<std::int32_t>(aargs[2 * i + 1]);
        const std::int32_t ours = sim::x87::atan2_scaled(y, x);
        if (ours != static_cast<std::int32_t>(aout[i]) && ++abad <= 20)
            std::printf("atan2_scaled(%d, %d): orig %d ours %d\n", y, x, static_cast<std::int32_t>(aout[i]), ours);
    }
    std::printf("atan2_scaled: %zu calls (%zu near rounding ties), %zu mismatches\n", aout.size(), near_ties, abad);

    // 4. C runtime hypot (Math_Hypot 0x004FB440, cdecl (double x, double y) -> ST0).
    std::vector<u32> hargs;
    auto push_d = [&](double d) {
        u32 w[2];
        std::memcpy(w, &d, 8);
        hargs.push_back(w[0]);
        hargs.push_back(w[1]);
    };
    std::uniform_real_distribution<double> ur(-1.0, 1.0);
    for (int i = 0; i < 1000000; ++i) {
        double x, y;
        const u32 kind = g() % 5;
        if (kind == 0) {  // Pythagorean-style integer pairs
            const std::int32_t a = static_cast<std::int32_t>(g() % 5000), b = static_cast<std::int32_t>(g() % 5000);
            const std::int32_t s = static_cast<std::int32_t>(g() % 80);
            x = static_cast<double>((a * a - b * b)) * s;
            y = static_cast<double>(2 * a * b) * s;
        } else if (kind == 1) {
            x = static_cast<double>(rnd_int());
            y = static_cast<double>(rnd_int());
        } else if (kind == 2) {
            x = std::ldexp(ur(g), static_cast<int>(g() % 60) - 20);
            y = std::ldexp(ur(g), static_cast<int>(g() % 60) - 20);
        } else if (kind == 3) {
            x = static_cast<double>(static_cast<float>(ur(g) * 1e6));
            y = static_cast<double>(static_cast<float>(ur(g) * 1e6));
        } else {
            x = static_cast<double>(rnd_int());
            y = 0.0;
        }
        push_d(x);
        push_d(y);
    }
    std::vector<u32> hout;
    if (!remote_batch(pi.hProcess, 0x004FB440, 4, false, hargs, hout)) {
        std::fprintf(stderr, "remote hypot batch failed\n");
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    std::size_t hbad = 0;
    for (std::size_t i = 0; i < hout.size() / 2; ++i) {
        double x, y, orig;
        std::memcpy(&x, &hargs[4 * i], 8);
        std::memcpy(&y, &hargs[4 * i + 2], 8);
        std::memcpy(&orig, &hout[2 * i], 8);
        const double ours = sim::x87::crt_hypot(x, y);
        if (std::memcmp(&ours, &orig, 8) != 0 && ++hbad <= 20)
            std::printf("hypot(%.17g, %.17g): orig %.17g ours %.17g\n", x, y, orig, ours);
    }
    std::printf("crt_hypot: %zu calls, %zu mismatches\n", hout.size() / 2, hbad);
    TerminateProcess(pi.hProcess, 0);
    return (bad || sign_or_exp_bad || abad || hbad) ? 1 : 0;
}
