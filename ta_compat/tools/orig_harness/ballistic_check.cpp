// Verifies the weapon ballistics emulation against the original:
//  1. the C runtime acos (0x004E67F0, argument and result on the x87 stack,
//     full 80-bit result) against sim::x87::crt_acos;
//  2. the ballistic pitch solver (0x0049A890, stdcall (dx, dy, dz, speed,
//     float min_angle), gravity from the game state) against sim::ballistic_pitch.
//
// Usage: ballistic_check <TotalA.exe> [count]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "sim/weapons.h"
#include "sim/x87_math.h"

namespace {

using u32 = std::uint32_t;
using u8 = std::uint8_t;

// Runs `code` once in the target with `data` as its argument block.
bool run(HANDLE proc, const std::vector<u8>& code, const std::vector<u8>& data, std::vector<u8>& out, std::size_t out_at) {
    auto* c = VirtualAllocEx(proc, nullptr, code.size(), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    auto* d = static_cast<u8*>(VirtualAllocEx(proc, nullptr, data.size() + out.size(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!c || !d) return false;
    std::vector<u8> blk = data;
    // Patch the in/out pointers (words 1 and 2 of the block are offsets; make them absolute).
    const u32 base = static_cast<u32>(reinterpret_cast<std::uintptr_t>(d));
    u32 w[3];
    std::memcpy(w, blk.data(), 12);
    w[1] += base;
    w[2] = base + static_cast<u32>(out_at);
    std::memcpy(blk.data(), w, 12);
    SIZE_T k = 0;
    WriteProcessMemory(proc, c, code.data(), code.size(), &k);
    WriteProcessMemory(proc, d, blk.data(), blk.size(), &k);
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(c), d, 0, nullptr);
    if (!th) return false;
    const DWORD wr = WaitForSingleObject(th, 600000);
    DWORD exit_code = 1;
    GetExitCodeThread(th, &exit_code);
    CloseHandle(th);
    if (wr != WAIT_OBJECT_0 || exit_code != 0) {
        std::fprintf(stderr, "remote thread: wait %lu, exit code %08lx\n", wr, exit_code);
        return false;
    }
    ReadProcessMemory(proc, d + out_at, out.data(), out.size(), &k);
    return true;
}

// Prologue: block = {count, in*, out*} -> esi = count, edi = in, ebx = out.
const u8 kPrologue[] = {0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57, 0x8B, 0x5D, 0x08, 0x8B, 0x33, 0x8B, 0x7B, 0x04, 0x8B, 0x5B, 0x08};
const u8 kEpilogue[] = {0x5F, 0x5E, 0x5B, 0x5D, 0x33, 0xC0, 0xC2, 0x04, 0x00};

std::vector<u8> loop_stub(const std::vector<u8>& body) {
    std::vector<u8> c(kPrologue, kPrologue + sizeof kPrologue);
    const std::size_t loop = c.size();
    c.insert(c.end(), {0x85, 0xF6, 0x74, 0x00});  // test esi,esi; jz done
    const std::size_t jz = c.size() - 1;
    c.insert(c.end(), body.begin(), body.end());
    c.insert(c.end(), {0x4E, 0xEB, 0x00});  // dec esi; jmp loop
    c.back() = static_cast<u8>(static_cast<int>(loop) - static_cast<int>(c.size()));
    c[jz] = static_cast<u8>(c.size() - (jz + 1));
    c.insert(c.end(), kEpilogue, kEpilogue + sizeof kEpilogue);
    return c;
}

void put32(std::vector<u8>& b, u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i))); }

bool same80(const u8* hw, const std::optional<sim::x87::Ext80>& e) {
    std::uint64_t m;
    std::uint16_t se;
    std::memcpy(&m, hw, 8);
    std::memcpy(&se, hw + 8, 2);
    const bool nan = (se & 0x7FFF) == 0x7FFF;
    if (!e) return nan;
    if (nan) return false;
    if (m == 0 || e->mantissa == 0) return m == e->mantissa;
    const int exp = static_cast<int>(se & 0x7FFF) - 16383 - 63;
    return m == e->mantissa && exp == e->exponent && ((se & 0x8000) != 0) == e->negative;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ballistic_check <TotalA.exe> [count]\n");
        return 2;
    }
    const int count = argc > 2 ? std::atoi(argv[2]) : 2000000;
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
    std::mt19937 g(777);
    std::uniform_real_distribution<double> ur(-1.0, 1.0);

    // 1. acos.
    std::vector<double> cs;
    for (int i = 0; i < count / 2; ++i) {
        const u32 k = g() % 8;
        double c;
        if (k < 4) c = ur(g);
        else if (k < 6) c = std::sqrt(std::fabs(ur(g))) * (g() % 2 ? 1 : -1);
        else if (k == 6) c = 1.0 - std::ldexp(static_cast<double>(g() % 1000), -static_cast<int>(g() % 43) - 10);  // near 1, |c| <= 1
        else c = std::ldexp(ur(g), -static_cast<int>(g() % 40));
        cs.push_back(c);
    }
    for (double c : {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1e-300, std::nextafter(1.0, 0.0), std::nextafter(-1.0, 0.0)}) cs.push_back(c);
    {
        // fld qword [edi]; mov eax,acos; call eax; fstp tbyte [ebx]; add edi,8; add ebx,10
        std::vector<u8> body = {0xDD, 0x07, 0xB8};
        put32(body, 0x004E67F0);
        body.insert(body.end(), {0xFF, 0xD0, 0xDB, 0x3B, 0x83, 0xC7, 0x08, 0x83, 0xC3, 0x0A});
        std::vector<u8> data;
        put32(data, static_cast<u32>(cs.size()));
        put32(data, 16);
        put32(data, 0);
        put32(data, 0);
        for (double c : cs) {
            u32 w[2];
            std::memcpy(w, &c, 8);
            put32(data, w[0]);
            put32(data, w[1]);
        }
        std::vector<u8> out(cs.size() * 10);
        if (!run(pi.hProcess, loop_stub(body), data, out, data.size())) {
            std::fprintf(stderr, "remote acos batch failed\n");
            TerminateProcess(pi.hProcess, 1);
            return 1;
        }
        // A mismatch is "knife-edge" when the exact value lies within 1/8 unit of a 64-bit
        // halfway point (1/8 unit) that also decides the 53-bit rounding: there the CPU's own FPATAN
        // rounding (not correct rounding) decides, as with FSINCOS.
        auto knife_edge = [](double c) {
            if (!(std::fabs(c) < 1.0)) return false;
            const double sq = std::sqrt((1.0 + c) * (1.0 - c));
            std::uint64_t hi, lo;
            int top;
            sim::x87::fpatan_debug(sq, c, hi, lo, top);
            const std::uint64_t low11 = hi & 0x7FF;
            const std::uint64_t dist = lo > 0x8000000000000000ull ? lo - 0x8000000000000000ull : 0x8000000000000000ull - lo;
            return (low11 == 0x3FF || low11 == 0x400) && dist < (1ull << 61);
        };
        std::size_t bad = 0, edge = 0;
        for (std::size_t i = 0; i < cs.size(); ++i) {
            if (same80(&out[i * 10], sim::x87::crt_acos(cs[i]))) continue;
            if (knife_edge(cs[i])) {
                ++edge;
                continue;
            }
            if (++bad <= 20) {
                std::uint64_t m;
                std::uint16_t se;
                std::memcpy(&m, &out[i * 10], 8);
                std::memcpy(&se, &out[i * 10 + 8], 2);
                const auto e = sim::x87::crt_acos(cs[i]);
                std::printf("acos(%.17g): orig %04x:%016llx ours %s%d:%016llx\n", cs[i], se, static_cast<unsigned long long>(m),
                            e && e->negative ? "-" : "", e ? e->exponent : 99999, e ? static_cast<unsigned long long>(e->mantissa) : 0ull);
                if (std::fabs(cs[i]) < 1.0) {
                    const double c = cs[i], sq = std::sqrt((1.0 + c) * (1.0 - c));
                    std::uint64_t hi, lo;
                    int top;
                    sim::x87::fpatan_debug(sq, c, hi, lo, top);
                    std::printf("    exact %016llx %016llx (low 11 of hi %03llx)\n", static_cast<unsigned long long>(hi),
                                static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi & 0x7FF));
                }
            }
        }
        std::printf("crt_acos: %zu calls, %zu knife-edge (CPU FPATAN rounding), %zu other mismatches\n", cs.size(), edge, bad);
        if (bad) {
            TerminateProcess(pi.hProcess, 1);
            return 1;
        }
    }

    // 2. Ballistic pitch, for several gravities (the game state holds it at +0x14263).
    auto* gs = static_cast<u8*>(VirtualAllocEx(pi.hProcess, nullptr, 0x40000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const u32 gs_addr = static_cast<u32>(reinterpret_cast<std::uintptr_t>(gs));
    SIZE_T k = 0;
    WriteProcessMemory(pi.hProcess, reinterpret_cast<LPVOID>(0x00511DE8), &gs_addr, 4, &k);
    std::size_t total = 0, total_bad = 0, solvable = 0;
    for (std::int32_t gravity : {112, 1, 50, 300, 4000}) {
        WriteProcessMemory(pi.hProcess, gs + 0x14263, &gravity, 4, &k);
        struct Case {
            std::int32_t dx, dy, dz, speed;
            float min_angle;
        };
        std::vector<Case> cases;
        for (int i = 0; i < count / 7; ++i) {
            Case c;
            const u32 r = g() % 10;
            const std::int32_t span = r < 7 ? 3000 : (r < 9 ? 300 : 70000);
            c.dx = static_cast<std::int32_t>(g() % (2 * span + 1)) - span;
            c.dz = static_cast<std::int32_t>(g() % (2 * span + 1)) - span;
            c.dy = static_cast<std::int32_t>(g() % (span + 1)) - span / 2;
            if (g() % 20 == 0) c.dx = c.dz = 0;
            c.speed = g() % 30 == 0 ? 0 : static_cast<std::int32_t>(g() % 2000);
            const float mins[] = {-0.3f, 0.0f, 0.05f, 0.2f, 0.5f, 0.7853981f, -10.0f};
            c.min_angle = g() % 3 == 0 ? static_cast<float>(ur(g)) : mins[g() % 7];
            cases.push_back(c);
        }
        // push [edi+16..0]; mov eax,fn; call eax (stdcall); mov [ebx],eax; add ebx,4; add edi,20
        std::vector<u8> body;
        for (int a = 4; a >= 0; --a) body.insert(body.end(), {0xFF, 0x77, static_cast<u8>(4 * a)});
        body.push_back(0xB8);
        put32(body, 0x0049A890);
        body.insert(body.end(), {0xFF, 0xD0, 0x89, 0x03, 0x83, 0xC3, 0x04, 0x83, 0xC7, 0x14});
        std::vector<u8> data;
        put32(data, static_cast<u32>(cases.size()));
        put32(data, 16);
        put32(data, 0);
        put32(data, 0);
        for (const Case& c : cases) {
            u32 mb;
            std::memcpy(&mb, &c.min_angle, 4);
            put32(data, static_cast<u32>(c.dx));
            put32(data, static_cast<u32>(c.dy));
            put32(data, static_cast<u32>(c.dz));
            put32(data, static_cast<u32>(c.speed));
            put32(data, mb);
        }
        std::vector<u8> out(cases.size() * 4);
        if (!run(pi.hProcess, loop_stub(body), data, out, data.size())) {
            std::fprintf(stderr, "remote ballistic batch failed (gravity %d)\n", gravity);
            TerminateProcess(pi.hProcess, 1);
            return 1;
        }
        std::size_t bad = 0;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            u32 o;
            std::memcpy(&o, &out[i * 4], 4);
            const auto orig = static_cast<std::int16_t>(o);
            const Case& c = cases[i];
            const std::int16_t ours = sim::ballistic_pitch(c.dx, c.dy, c.dz, c.speed, c.min_angle, gravity);
            if (orig != -32768) ++solvable;
            if (orig != ours && ++bad <= 20)
                std::printf("g %d (%d,%d,%d) v %d min %.9g: orig %d ours %d\n", gravity, c.dx, c.dy, c.dz, c.speed, c.min_angle, orig, ours);
        }
        total += cases.size();
        total_bad += bad;
    }
    std::printf("ballistic_pitch: %zu calls (%zu with a solution), %zu mismatches\n", total, solvable, total_bad);
    TerminateProcess(pi.hProcess, 0);
    return total_bad ? 1 : 0;
}
