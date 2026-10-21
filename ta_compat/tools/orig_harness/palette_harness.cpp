// Differential test of the palette table builders: the original's alpha (0x4BA750), shade
// (0x4BADF0) and light (0x4BABD0) builders, run inside a suspended TotalA.exe on the shipped
// palette and on generated ones, against ta::build_alpha_table / build_shade_table /
// build_light_table; and the port on the shipped palette against the shipped PALETTE.ALP /
// .SHD / .LHT files.
//
// The builders read the display record through 0x4B6220 (the global at 0x51FBD0): its flags
// word +0xF0 (bits 5, 6, 7: the three tables wanted) and the tables at +0xC0 / +0xC4 / +0xC8,
// which they fill and return.
//
// Usage: palette_harness <TotalA.exe> <game_dir> [cases] [seed]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "remote.h"
#include "ta/hpi.h"
#include "ta/palette_tables.h"

namespace {

using u32 = std::uint32_t;
constexpr u32 kDisplayGlobal = 0x0051FBD0;
constexpr u32 kAlpha = 0x004BA750, kShade = 0x004BADF0, kLight = 0x004BABD0;

std::uint64_t fnv(const std::vector<std::uint8_t>& v) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint8_t b : v) h = (h ^ b) * 1099511628211ull;
    return h;
}

int differing(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size()) return -1;
    int n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) n += a[i] != b[i];
    return n;
}

struct Remote {
    harness::Remote r;
    u32 ctx = 0, alp = 0, shd = 0, lht = 0, pal = 0;
    bool start(const char* exe) {
        if (!r.start(exe) || !r.install_crash_catcher()) return false;
        ctx = r.alloc(0x1000);
        alp = r.alloc(0x10000);
        shd = r.alloc(0x2000);
        lht = r.alloc(0x2000);
        pal = r.alloc(0x1000);
        std::vector<std::uint8_t> z(0x1000, 0);
        r.write(ctx, z.data(), z.size());
        const std::uint16_t flags = 0x00E0;  // alpha (bit 5), shade (bit 6), light (bit 7)
        r.write(ctx + 0xF0, &flags, 2);
        r.w32(ctx + 0xC0, alp);
        r.w32(ctx + 0xC4, shd);
        r.w32(ctx + 0xC8, lht);
        return r.w32(kDisplayGlobal, ctx);
    }
    bool run(u32 fn, const ta::PaletteBytes& p, u32 table, std::size_t n, std::vector<std::uint8_t>& out) {
        r.write(pal, p.data(), p.size());
        std::vector<std::uint8_t> junk(n, 0xCD);
        r.write(table, junk.data(), n);
        u32 ret = 0;
        if (!r.call(fn, 0, {pal}, &ret, 60000) || ret != table) return false;
        out.assign(n, 0);
        return r.read(table, out.data(), n);
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: palette_harness <TotalA.exe> <game_dir> [cases] [seed]\n");
        return 2;
    }
    const int cases = argc > 3 ? std::atoi(argv[3]) : 40;
    const unsigned seed = argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 1u;
    int bad = 0;

    // The shipped palette and tables.
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[2]);
    std::vector<std::uint8_t> pal_file, alp_file, shd_file, lht_file, pcx_file;
    if (!vfs.read("palettes\\PALETTE.PAL", pal_file) || pal_file.size() < 1024) {
        std::fprintf(stderr, "no palettes\\PALETTE.PAL in %s\n", argv[2]);
        return 1;
    }
    vfs.read("palettes\\PALETTE.ALP", alp_file);
    vfs.read("palettes\\PALETTE.SHD", shd_file);
    vfs.read("palettes\\PALETTE.LHT", lht_file);
    ta::PaletteBytes shipped{};
    std::copy(pal_file.begin(), pal_file.begin() + 1024, shipped.begin());
    {
        int nonzero = 0;
        for (int i = 0; i < 256; ++i) nonzero += shipped[static_cast<std::size_t>(i) * 4 + 3] != 0;
        std::printf("shipped PALETTE.PAL: %zu bytes, byte 3 nonzero in %d entries\n", pal_file.size(), nonzero);
    }
    if (vfs.read("palettes\\PALETTE.PCX", pcx_file)) {
        ta::PaletteBytes from_pcx{};
        const bool ok = ta::pcx_palette(pcx_file, from_pcx);
        int diff = 0;
        for (std::size_t i = 0; i < 1024; ++i) diff += from_pcx[i] != shipped[i];
        std::printf("shipped PALETTE.PCX: %zu bytes, colour map read %d, %d bytes differ from PALETTE.PAL\n", pcx_file.size(), ok, diff);
    } else {
        std::printf("no palettes\\PALETTE.PCX shipped\n");
    }

    const std::vector<std::uint8_t> port_alp = ta::build_alpha_table(shipped);
    const std::vector<std::uint8_t> port_shd = ta::build_shade_table(shipped);
    const std::vector<std::uint8_t> port_lht = ta::build_light_table(shipped);
    std::printf("port vs shipped files: ALP %zu bytes, %d differ; SHD %zu bytes, %d differ; LHT %zu bytes, %d differ\n", alp_file.size(),
                differing(port_alp, std::vector<std::uint8_t>(alp_file.begin(), alp_file.begin() + std::min<std::size_t>(alp_file.size(), 65536))),
                shd_file.size(), differing(port_shd, shd_file), lht_file.size(), differing(port_lht, lht_file));

    Remote o;
    if (!o.start(argv[1])) {
        std::fprintf(stderr, "cannot start %s\n", argv[1]);
        return 1;
    }
    auto compare = [&](const char* name, const ta::PaletteBytes& p, bool print_hash) {
        std::vector<std::uint8_t> a, s, l;
        if (!o.run(kAlpha, p, o.alp, 65536, a) || !o.run(kShade, p, o.shd, 8192, s) || !o.run(kLight, p, o.lht, 8192, l)) {
            const harness::Remote::Crash c = o.r.crash_info();
            std::printf("%s: original call failed (exception %08X at %08X)\n", name, c.code, c.address);
            ++bad;
            return;
        }
        const auto pa = ta::build_alpha_table(p), ps = ta::build_shade_table(p), pl = ta::build_light_table(p);
        const int da = differing(a, pa), ds = differing(s, ps), dl = differing(l, pl);
        if (da || ds || dl) ++bad;
        std::printf("%-12s original vs port: ALP %d, SHD %d, LHT %d bytes differ", name, da, ds, dl);
        if (print_hash)
            std::printf("  (fnv64 ALP %016llX SHD %016llX LHT %016llX)", static_cast<unsigned long long>(fnv(pa)),
                        static_cast<unsigned long long>(fnv(ps)), static_cast<unsigned long long>(fnv(pl)));
        std::printf("\n");
    };
    compare("shipped", shipped, true);
    {
        // The original on the shipped palette against the shipped files too.
        std::vector<std::uint8_t> a, s, l;
        o.run(kAlpha, shipped, o.alp, 65536, a);
        o.run(kShade, shipped, o.shd, 8192, s);
        o.run(kLight, shipped, o.lht, 8192, l);
        std::printf("original vs shipped files: ALP %d, SHD %d, LHT %d bytes differ\n",
                    differing(a, std::vector<std::uint8_t>(alp_file.begin(), alp_file.begin() + std::min<std::size_t>(alp_file.size(), 65536))),
                    differing(s, shd_file), differing(l, lht_file));
    }
    // Generated palettes: the synthetic one the unit test pins (entry i = {i, 255 - i, i * 7, 0}),
    // all black, black and white only (nothing within 40 of the halves), few distinct colours
    // (ties), and random ones (some with every byte 3 set).
    {
        ta::PaletteBytes p{};
        for (int i = 0; i < 256; ++i) {
            p[static_cast<std::size_t>(i) * 4] = static_cast<std::uint8_t>(i);
            p[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(255 - i);
            p[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>(i * 7);
        }
        compare("synthetic", p, true);
    }
    {
        ta::PaletteBytes p{};
        compare("black", p, true);
        for (int i = 0; i < 256; ++i)
            for (int k = 0; k < 3; ++k) p[static_cast<std::size_t>(i) * 4 + static_cast<std::size_t>(k)] = (i & 1) ? 255 : 0;
        compare("black-white", p, true);
        for (int i = 0; i < 256; ++i)
            for (int k = 0; k < 3; ++k) p[static_cast<std::size_t>(i) * 4 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>((i % 5) * 60 + k);
        compare("five", p, true);
    }
    std::mt19937 rng(seed);
    for (int c = 0; c < cases; ++c) {
        ta::PaletteBytes p{};
        const int mode = c % 3;
        for (std::size_t i = 0; i < 1024; ++i) {
            if ((i & 3) == 3) p[i] = mode == 2 ? static_cast<std::uint8_t>(rng()) : 0;
            else p[i] = static_cast<std::uint8_t>(mode == 1 ? (rng() % 8) * 36 : rng());
        }
        char name[32];
        std::snprintf(name, sizeof name, "random %d", c);
        compare(name, p, false);
    }
    std::printf("%s\n", bad ? "DIFFERENCES" : "all identical");
    return bad ? 1 : 0;
}
