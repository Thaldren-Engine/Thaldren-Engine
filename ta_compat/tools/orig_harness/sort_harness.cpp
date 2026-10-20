// Differential test of the unit table's sort (GameData_LoadAll 0x42D2E0): the original's
// quicksort loop (0x432D40) and insertion sort (0x432FB0) with its unit-name comparator
// (0x42DB60, stricmp of the names at +0x20), run inside a suspended TotalA.exe on random
// 0x249-byte records with many equal names, against ta::sort_like_original. The final
// unguarded linear insertion that 0x42D2E0 does inline is done here on the read-back records.
//
// Usage: sort_harness <TotalA.exe> [cases] [seed]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "ta/stl98_sort.h"

namespace {

using u32 = std::uint32_t;
constexpr u32 kQuickLoop = 0x00432D40;  // (first, last, less)
constexpr u32 kInsertion = 0x00432FB0;  // (first, last, less)
constexpr u32 kLess = 0x0042DB60;       // stdcall (a, b): stricmp(a + 0x20, b + 0x20) < 0
constexpr u32 kRec = 0x249;

// C-locale stricmp (the runtime's 0x4F8A70: only A-Z fold).
int c_stricmp(const std::string& a, const std::string& b) {
    std::size_t i = 0;
    for (;; ++i) {
        unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y + 32);
        if (x != y) return x < y ? -1 : 1;
        if (x == 0) return 0;
    }
}

struct Rec {
    u32 tag = 0;
    std::string name;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: sort_harness <TotalA.exe> [cases] [seed]\n");
        return 2;
    }
    const int cases = argc > 2 ? std::atoi(argv[2]) : 200;
    const unsigned seed = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 1u;
    harness::Remote r;
    if (!r.start(argv[1])) {
        std::fprintf(stderr, "cannot start %s\n", argv[1]);
        return 1;
    }
    constexpr int kMax = 400;
    const u32 base = r.alloc(kRec * (kMax + 2));
    std::mt19937 rng(seed);
    auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    static const char* kStems[] = {"ARMPW", "armpw", "CORAK", "corak", "ARMCOM", "ArmCom", "CORCOM", "ARMSOLAR", "armsolar",
                                   "CORMEX", "ARMMEX", "ARMLAB", "corlab", "ARMVP", "CORVP", "ARM_", "arm[", "Z", "a", ""};
    int same = 0, differ = 0, with_duplicates = 0, stable_differs = 0;
    for (int c = 0; c < cases; ++c) {
        const int n = c < 40 ? rnd(0, 20) : rnd(17, kMax);
        const int stems = rnd(1, 20);
        std::vector<Rec> recs(static_cast<std::size_t>(n));
        std::vector<std::uint8_t> mem(static_cast<std::size_t>(n) * kRec);
        for (int i = 0; i < n; ++i) {
            Rec& rc = recs[static_cast<std::size_t>(i)];
            rc.tag = static_cast<u32>(i + 1);
            rc.name = kStems[rnd(0, stems - 1)];
            if (rnd(0, 3) == 0) rc.name += std::to_string(rnd(0, 3));
            std::uint8_t* p = &mem[static_cast<std::size_t>(i) * kRec];
            for (u32 k = 0; k < kRec; ++k) p[k] = static_cast<std::uint8_t>(rnd(0, 255));
            std::memcpy(p, &rc.tag, 4);
            std::memset(p + 0x20, 0, 0x20);
            std::memcpy(p + 0x20, rc.name.data(), rc.name.size());
        }
        bool dup = false;
        for (int i = 0; i < n && !dup; ++i)
            for (int j = i + 1; j < n; ++j)
                if (c_stricmp(recs[static_cast<std::size_t>(i)].name, recs[static_cast<std::size_t>(j)].name) == 0) {
                    dup = true;
                    break;
                }
        with_duplicates += dup;
        if (n) r.write(base, mem.data(), mem.size());
        const u32 first = base, last = base + static_cast<u32>(n) * kRec;
        bool ok = true;
        if (n < 17) {
            ok = r.call(kInsertion, 0, {first, last, kLess});
        } else {
            ok = r.call(kQuickLoop, 0, {first, last, kLess}) && r.call(kInsertion, 0, {first, first + 16 * kRec, kLess});
        }
        if (!ok) {
            std::printf("case %d: the original's call failed\n", c);
            ++differ;
            continue;
        }
        if (n) r.read(base, mem.data(), mem.size());
        std::vector<Rec> orig(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const std::uint8_t* p = &mem[static_cast<std::size_t>(i) * kRec];
            std::memcpy(&orig[static_cast<std::size_t>(i)].tag, p, 4);
            orig[static_cast<std::size_t>(i)].name = reinterpret_cast<const char*>(p + 0x20);
        }
        if (n >= 17) {
            // 0x42D2E0's own tail: unguarded linear insertion of every record after the 16th.
            for (int i = 16; i < n; ++i) {
                Rec v = orig[static_cast<std::size_t>(i)];
                int j = i;
                while (c_stricmp(v.name, orig[static_cast<std::size_t>(j - 1)].name) < 0) {
                    orig[static_cast<std::size_t>(j)] = orig[static_cast<std::size_t>(j - 1)];
                    --j;
                }
                orig[static_cast<std::size_t>(j)] = v;
            }
        }
        std::vector<Rec> ours = recs;
        ta::sort_like_original(ours, 0, ours.size(), [](const Rec& a, const Rec& b) { return c_stricmp(a.name, b.name) < 0; });
        // How a stable sort would have done (it differs whenever equal names get reordered).
        std::vector<Rec> stable = recs;
        std::stable_sort(stable.begin(), stable.end(), [](const Rec& a, const Rec& b) { return c_stricmp(a.name, b.name) < 0; });
        for (int i = 0; i < n; ++i)
            if (orig[static_cast<std::size_t>(i)].tag != stable[static_cast<std::size_t>(i)].tag) {
                ++stable_differs;
                break;
            }
        bool eq = true;
        for (int i = 0; i < n; ++i)
            if (orig[static_cast<std::size_t>(i)].tag != ours[static_cast<std::size_t>(i)].tag) eq = false;
        if (eq) {
            ++same;
        } else {
            ++differ;
            if (differ <= 3) {
                std::printf("case %d (n %d) differs:\n  original:", c, n);
                for (const Rec& x : orig) std::printf(" %u", x.tag);
                std::printf("\n  ours:    ");
                for (const Rec& x : ours) std::printf(" %u", x.tag);
                std::printf("\n");
            }
        }
    }
    r.stop();
    std::printf("sort_harness: %d cases (%d with equal names): %d identical, %d differ (a stable sort would differ in %d)\n", cases, with_duplicates, same, differ, stable_differs);
    return differ ? 1 : 0;
}
