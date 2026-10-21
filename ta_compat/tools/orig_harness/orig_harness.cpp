// Differential tester: starts the original TotalA.exe suspended (so the OS maps
// its image at 0x400000 but none of the game runs), injects a tiny x86 stub,
// and uses it to call self-contained routines of the original (atof, atoi) on
// large batches of inputs. Results are compared with our reimplementations.
//
// Usage: orig_harness <TotalA.exe> <game_dir> [random_count]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "ta/crt_strtod.h"
#include "ta/hpi.h"
#include "ta/tdf.h"

namespace {

// Thread procedure (stdcall, one argument: Job*):
//   for each string: results_f64[i] = atof(s); results_i32[i] = atoi(s);
//   _atoflt(&results_f32[i], s) (sscanf's "%f" into a float)
// Job layout: u32 count, u32 strings (char**), u32 out_f64, u32 out_i32, u32 out_f32.
const std::uint8_t kStub[] = {
    0x55, 0x8B, 0xEC, 0x56, 0x57, 0x53,  // push ebp; mov ebp,esp; push esi; push edi; push ebx
    0x8B, 0x5D, 0x08,                    // mov ebx,[ebp+8]
    0x8B, 0x33,                          // mov esi,[ebx]
    0x8B, 0x7B, 0x04,                    // mov edi,[ebx+4]
    // loop:
    0x85, 0xF6,                          // test esi,esi
    0x74, 0x43,                          // jz done
    0xFF, 0x37,                          // push dword [edi]
    0xB8, 0x60, 0x45, 0x4E, 0x00,        // mov eax,0x004E4560 (atof)
    0xFF, 0xD0,                          // call eax
    0x83, 0xC4, 0x04,                    // add esp,4
    0x8B, 0x4B, 0x08,                    // mov ecx,[ebx+8]
    0xDD, 0x19,                          // fstp qword [ecx]
    0x83, 0x43, 0x08, 0x08,              // add dword [ebx+8],8
    0xFF, 0x37,                          // push dword [edi]
    0xB8, 0xD0, 0x4E, 0x4E, 0x00,        // mov eax,0x004E4ED0 (atoi)
    0xFF, 0xD0,                          // call eax
    0x83, 0xC4, 0x04,                    // add esp,4
    0x8B, 0x4B, 0x0C,                    // mov ecx,[ebx+0xC]
    0x89, 0x01,                          // mov [ecx],eax
    0x83, 0x43, 0x0C, 0x04,              // add dword [ebx+0xC],4
    0xFF, 0x37,                          // push dword [edi]
    0xFF, 0x73, 0x10,                    // push dword [ebx+0x10]
    0xB8, 0xD0, 0x3A, 0x4F, 0x00,        // mov eax,0x004F3AD0 (_atoflt)
    0xFF, 0xD0,                          // call eax
    0x83, 0xC4, 0x08,                    // add esp,8
    0x83, 0x43, 0x10, 0x04,              // add dword [ebx+0x10],4
    0x83, 0xC7, 0x04,                    // add edi,4
    0x4E,                                // dec esi
    0xEB, 0xB9,                          // jmp loop
    // done:
    0x5B, 0x5F, 0x5E, 0x5D,              // pop ebx; pop edi; pop esi; pop ebp
    0x33, 0xC0,                          // xor eax,eax
    0xC2, 0x04, 0x00,                    // ret 4
};

struct Child {
    PROCESS_INFORMATION pi{};
    LPVOID stub = nullptr;
};

bool start_child(const std::string& exe, const std::string& dir, Child& c) {
    STARTUPINFOA si{};
    si.cb = sizeof si;
    std::string cmd = "\"" + exe + "\"";
    if (!CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &c.pi)) {
        std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return false;
    }
    c.stub = VirtualAllocEx(c.pi.hProcess, nullptr, sizeof kStub, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    SIZE_T n = 0;
    return c.stub && WriteProcessMemory(c.pi.hProcess, c.stub, kStub, sizeof kStub, &n) && n == sizeof kStub;
}

// Runs atof/atoi/_atoflt in the child for every string; fills the result vectors.
bool run_batch(Child& c, const std::vector<std::string>& strs, std::vector<double>& f, std::vector<int>& i,
               std::vector<std::uint32_t>& f32) {
    const std::uint32_t count = static_cast<std::uint32_t>(strs.size());
    std::vector<std::uint8_t> text;
    std::vector<std::uint32_t> offsets;
    for (const auto& s : strs) {
        offsets.push_back(static_cast<std::uint32_t>(text.size()));
        text.insert(text.end(), s.begin(), s.end());
        text.push_back(0);
    }
    const std::size_t total = 20 + count * 4u + count * 8u + count * 4u + count * 4u + text.size();
    auto* remote = static_cast<std::uint8_t*>(
        VirtualAllocEx(c.pi.hProcess, nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!remote) return false;
    const auto base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(remote));
    const std::uint32_t ptrs = base + 20, out_f = ptrs + count * 4u, out_i = out_f + count * 8u, out_s = out_i + count * 4u,
                        txt = out_s + count * 4u;
    std::vector<std::uint8_t> buf(total);
    const std::uint32_t job[5] = {count, ptrs, out_f, out_i, out_s};
    std::memcpy(buf.data(), job, 20);
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::uint32_t p = txt + offsets[k];
        std::memcpy(buf.data() + 20 + k * 4u, &p, 4);
    }
    std::memcpy(buf.data() + (txt - base), text.data(), text.size());
    SIZE_T n = 0;
    if (!WriteProcessMemory(c.pi.hProcess, remote, buf.data(), buf.size(), &n)) return false;
    HANDLE th = CreateRemoteThread(c.pi.hProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(c.stub), remote, 0, nullptr);
    if (!th) return false;
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    f.resize(count);
    i.resize(count);
    f32.resize(count);
    const bool ok = ReadProcessMemory(c.pi.hProcess, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(out_f)), f.data(),
                                      count * 8u, &n) &&
                    ReadProcessMemory(c.pi.hProcess, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(out_i)), i.data(),
                                      count * 4u, &n) &&
                    ReadProcessMemory(c.pi.hProcess, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(out_s)), f32.data(),
                                      count * 4u, &n);
    VirtualFreeEx(c.pi.hProcess, remote, 0, MEM_RELEASE);
    return ok;
}

void collect_values(const ta::TdfSection& s, std::set<std::string>& out) {
    for (const auto& k : s.keys()) out.insert(k.value);
    for (const auto& ch : s.children()) collect_values(*ch, out);
}

void collect_archive(ta::HpiArchive& a, const ta::HpiEntry& dir, std::set<std::string>& out) {
    std::vector<std::uint8_t> data;
    for (const auto& e : dir.children) {
        if (e.is_dir) {
            collect_archive(a, e, out);
            continue;
        }
        std::string n = e.name;
        for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const auto dot = n.rfind('.');
        const std::string ext = dot == std::string::npos ? "" : n.substr(dot);
        if (ext != ".tdf" && ext != ".fbi" && ext != ".ota" && ext != ".gui") continue;
        if (a.read(e, data) != ta::HpiError::None) continue;
        auto r = ta::tdf_parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
        if (r.root) collect_values(*r.root, out);
    }
}

std::string random_number(std::mt19937& rng) {
    static const char* kWs[] = {"", "", "", " ", "\t", "  "};
    std::string s = kWs[rng() % 6];
    const int sign = static_cast<int>(rng() % 5);
    if (sign == 1) s += '-';
    if (sign == 2) s += '+';
    const int int_digits = static_cast<int>(rng() % 30);
    const int lead_zeros = (rng() % 4 == 0) ? static_cast<int>(rng() % 5) : 0;
    s.append(static_cast<std::size_t>(lead_zeros), '0');
    for (int k = 0; k < int_digits; ++k) s += static_cast<char>('0' + rng() % 10);
    if (rng() % 2) {
        s += '.';
        const int frac_digits = static_cast<int>(rng() % 30);
        const int frac_zeros = (rng() % 3 == 0) ? static_cast<int>(rng() % 12) : 0;
        s.append(static_cast<std::size_t>(frac_zeros), '0');
        for (int k = 0; k < frac_digits; ++k) s += static_cast<char>('0' + rng() % 10);
    }
    if (rng() % 3 == 0) {
        static const char kE[] = {'e', 'E', 'd', 'D'};
        s += kE[rng() % 4];
        const int es = static_cast<int>(rng() % 3);
        if (es == 1) s += '-';
        if (es == 2) s += '+';
        const int mag = static_cast<int>(rng() % 4);
        const int exp = mag == 0 ? static_cast<int>(rng() % 10)
                      : mag == 1 ? static_cast<int>(rng() % 40)
                      : mag == 2 ? static_cast<int>(rng() % 400)
                                 : static_cast<int>(rng() % 6000);
        if (rng() % 5 == 0) s += "00";
        s += std::to_string(exp);
    }
    if (rng() % 6 == 0) s += "xyz";
    return s;
}

std::uint64_t bits_of(double d) {
    std::uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: orig_harness <TotalA.exe> <game_dir> [random_count]\n");
        return 2;
    }
    const unsigned long random_count = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 2000000ul;

    std::set<std::string> corpus_set;
    for (const auto& de : std::filesystem::directory_iterator(argv[2])) {
        std::string ext = de.path().extension().string();
        for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ext != ".hpi" && ext != ".ufo" && ext != ".ccx" && ext != ".gp3") continue;
        ta::HpiError err = ta::HpiError::None;
        auto a = ta::HpiArchive::open(de.path(), err);
        if (a) collect_archive(*a, a->root(), corpus_set);
    }
    for (const char* edge : {"0", "-0", "-.0", ".", "-", "+", "", "e5", "1e", "1e+", "1e-0", "9007199254740993",
                             "9007199254740992.5", "0.5", "1.7976931348623157e308", "1.8e308", "4.9e-324", "2.5e-324",
                             "2.2250738585072011e-308", "1e5200", "1e5201", "1e-5200", "1e-5201",
                             "123456789012345678901234567890", "0.000000000000000000000000000001", "1d10", "1D-10",
                             "  \t\n\v\f\r7", "2147483648", "-2147483649", "99999999999"}) {
        corpus_set.insert(edge);
    }
    std::vector<std::string> all(corpus_set.begin(), corpus_set.end());
    const std::size_t corpus_size = all.size();
    std::mt19937 rng(12345);
    for (unsigned long k = 0; k < random_count; ++k) all.push_back(random_number(rng));

    Child child;
    if (!start_child(argv[1], argv[2], child)) return 1;

    unsigned long atof_bad = 0, atoi_bad = 0, modern_differs = 0, modern_differs_game = 0;
    unsigned long scanf_bad = 0, scanf_via_double_differs = 0, scanf_via_double_differs_game = 0;
    constexpr std::size_t kBatch = 100000;
    std::vector<double> f;
    std::vector<int> iv;
    std::vector<std::uint32_t> fs;
    for (std::size_t start = 0; start < all.size(); start += kBatch) {
        const std::size_t end = (std::min)(all.size(), start + kBatch);
        std::vector<std::string> batch(all.begin() + static_cast<std::ptrdiff_t>(start), all.begin() + static_cast<std::ptrdiff_t>(end));
        if (!run_batch(child, batch, f, iv, fs)) {
            std::fprintf(stderr, "remote batch failed (%lu)\n", GetLastError());
            TerminateProcess(child.pi.hProcess, 1);
            return 1;
        }
        for (std::size_t k = 0; k < batch.size(); ++k) {
            const double got = ta::crt_atof(batch[k]);
            if (bits_of(f[k]) != bits_of(got) && ++atof_bad <= 25) {
                std::printf("ATOF MISMATCH \"%s\": orig %016llx ours %016llx\n", batch[k].c_str(),
                            static_cast<unsigned long long>(bits_of(f[k])), static_cast<unsigned long long>(bits_of(got)));
            }
            // How often would a modern correctly rounded strtod disagree with the original?
            if (bits_of(std::strtod(batch[k].c_str(), nullptr)) != bits_of(f[k])) {
                ++modern_differs;
                if (start + k < corpus_size) {
                    if (++modern_differs_game <= 10)
                        std::printf("  game value where modern strtod differs: \"%s\"\n", batch[k].c_str());
                }
            }
            // sscanf "%f" into a float: straight from the 80-bit intermediate.
            const float sf = ta::crt_scanf_float(batch[k]);
            std::uint32_t sb;
            std::memcpy(&sb, &sf, 4);
            if (fs[k] != sb && ++scanf_bad <= 25)
                std::printf("SCANF %%f MISMATCH \"%s\": orig %08x ours %08x\n", batch[k].c_str(), fs[k], sb);
            const float via_double = static_cast<float>(got);
            std::uint32_t vb;
            std::memcpy(&vb, &via_double, 4);
            if (fs[k] != vb && start + k < corpus_size) ++scanf_via_double_differs_game;
            if (fs[k] != vb && ++scanf_via_double_differs <= 10)
                std::printf("  (float(atof) differs from %%f: \"%s\" orig %08x via double %08x)\n", batch[k].c_str(), fs[k], vb);
            const int gi = ta::msvc_atoi(batch[k]);
            if (iv[k] != gi && ++atoi_bad <= 25) std::printf("ATOI MISMATCH \"%s\": orig %d ours %d\n", batch[k].c_str(), iv[k], gi);
        }
    }
    TerminateProcess(child.pi.hProcess, 0);
    CloseHandle(child.pi.hThread);
    CloseHandle(child.pi.hProcess);

    std::printf("checked %zu strings (%zu distinct values from game data + edge cases, %lu random)\n", all.size(), corpus_size,
                random_count);
    std::printf("atof mismatches: %lu\natoi mismatches: %lu\nscanf %%f mismatches: %lu\n", atof_bad, atoi_bad, scanf_bad);
    std::printf("(a modern strtod would differ from the original on %lu strings, %lu of them game data values)\n",
                modern_differs, modern_differs_game);
    std::printf("(rounding atof's double to a float would differ from %%f on %lu strings, %lu of them game data values)\n",
                scanf_via_double_differs, scanf_via_double_differs_game);
    return (atof_bad || atoi_bad || scanf_bad) ? 1 : 0;
}
