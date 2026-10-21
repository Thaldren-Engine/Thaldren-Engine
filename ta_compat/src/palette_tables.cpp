// The palette and its colour tables, loaded or built and cached as the original does at
// start-up (0x429330, 0x42E140, 0x42E1D0, 0x42E260; builders 0x4BA750, 0x4BADF0, 0x4BABD0 over
// 0x4BA920 / 0x4BA9D0; cache write 0x4BC290). Spec: docs/RE_MEMORY.md, "Palette tables built
// when missing".
//
// Floating point (docs/parity/NUMERICS_FLOAT.md): the shade and light factors are doubles
// stored with fstp qword, each channel is FILD (exact) times the factor (one PC53 multiply,
// the same rounding as a C++ double multiply) and __ftol truncates it (sim::x87::ftol32).

#include "ta/palette_tables.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

#include "sim/x87.h"

namespace ta {

SortedPalette sort_palette(const PaletteBytes& pal) {
    SortedPalette s;
    for (int i = 0; i < 256; ++i) {
        const std::size_t e = static_cast<std::size_t>(i) * 4;
        s.sum[static_cast<std::size_t>(i)] = pal[e] + pal[e + 1] + pal[e + 2];
        s.order[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    }
    for (std::size_t i = 0; i < 256; ++i)
        for (std::size_t j = i + 1; j < 256; ++j)
            if (s.sum[i] > s.sum[j]) {
                std::swap(s.sum[i], s.sum[j]);
                std::swap(s.order[i], s.order[j]);
            }
    return s;
}

std::uint8_t nearest_entry(const PaletteBytes& pal, const SortedPalette& sorted, std::uint8_t c0, std::uint8_t c1, std::uint8_t c2) {
    const std::int32_t target = c0 + c1 + c2;
    std::int32_t best = 1000000000;
    std::uint32_t pick = 0;
    std::uint32_t k = 0;
    for (; k < 256; ++k) {
        const std::int32_t s = sorted.sum[k];
        if (s < target - 40) continue;
        if (s > target + 40) break;
        const std::size_t e = static_cast<std::size_t>(sorted.order[k]) * 4;
        const std::int32_t d0 = pal[e] - c0, d1 = pal[e + 1] - c1, d2 = pal[e + 2] - c2;
        const std::int32_t d = d0 * d0 + d1 * d1 + d2 * d2;
        if (d < best) {
            pick = k;
            best = d;
        }
    }
    // (No entry within 40: the position where the scan stopped, as a byte.)
    if (best == 1000000000) pick = k;
    return sorted.order[pick & 0xFF];
}

std::vector<std::uint8_t> build_alpha_table(const PaletteBytes& pal) {
    const SortedPalette s = sort_palette(pal);
    std::vector<std::uint8_t> t(65536);
    for (std::size_t a = 0; a < 256; ++a)
        for (std::size_t b = 0; b < 256; ++b) {
            std::uint8_t v;
            if (a == b) {
                v = static_cast<std::uint8_t>(a);
            } else {
                const std::size_t ea = a * 4, eb = b * 4;
                v = nearest_entry(pal, s, static_cast<std::uint8_t>((pal[ea] + pal[eb]) / 2),
                                  static_cast<std::uint8_t>((pal[ea + 1] + pal[eb + 1]) / 2),
                                  static_cast<std::uint8_t>((pal[ea + 2] + pal[eb + 2]) / 2));
            }
            t[a * 256 + b] = v;
        }
    return t;
}

std::vector<std::uint8_t> build_shade_table(const PaletteBytes& pal) {
    const SortedPalette s = sort_palette(pal);
    std::vector<std::uint8_t> t(8192);
    // The factor starts at 0.0 and each level subtracts the constant at 0x4FDBE8 (-0.06875).
    double f = 0.0;
    constexpr double kStep = -0x1.199999999999ap-4;  // 0x4FDBE8: bf b1 99 99 99 99 99 9a
    for (std::size_t level = 0; level < 32; ++level) {
        for (std::size_t e = 0; e < 256; ++e) {
            std::uint8_t c[3];
            for (int k = 0; k < 3; ++k) {
                const std::int32_t v = sim::x87::ftol32(static_cast<double>(pal[e * 4 + static_cast<std::size_t>(k)]) * f);
                // cmp ax,0xFF / jbe: the low 16 bits compared unsigned.
                c[k] = static_cast<std::uint16_t>(v) > 0xFF ? std::uint8_t{0xFF} : static_cast<std::uint8_t>(v);
            }
            t[level * 256 + e] = nearest_entry(pal, s, c[0], c[1], c[2]);
        }
        f = f - kStep;
    }
    return t;
}

std::vector<std::uint8_t> build_light_table(const PaletteBytes& pal) {
    const SortedPalette s = sort_palette(pal);
    std::vector<std::uint8_t> t(8192);
    constexpr double kStep = -0x1.1111111111111p-5;  // 0x4FDBD0: bf a1 11 11 11 11 11 11 (-1/30)
    constexpr double kOne = 1.0;                      // 0x4FDBD8
    for (std::int32_t level = 0; level < 32; ++level) {
        // FILD level, FMUL [0x4FDBD0], FSUBR [0x4FDBD8], FSTP qword.
        const double f = kOne - static_cast<double>(level) * kStep;
        for (std::size_t e = 0; e < 256; ++e) {
            std::int32_t c[3];
            for (int k = 0; k < 3; ++k) {
                c[k] = sim::x87::ftol32(static_cast<double>(pal[e * 4 + static_cast<std::size_t>(k)]) * f);
                if (c[k] > 0xFF) c[k] = 0xFF;  // (signed compare)
            }
            t[static_cast<std::size_t>(level) * 256 + e] =
                nearest_entry(pal, s, static_cast<std::uint8_t>(c[0]), static_cast<std::uint8_t>(c[1]), static_cast<std::uint8_t>(c[2]));
        }
    }
    return t;
}

bool pcx_palette(const std::vector<std::uint8_t>& pcx, PaletteBytes& out) {
    if (pcx.size() < 0x80 || pcx[0] != 0x0A || pcx[1] != 0x05) return false;
    // The map is read from 768 bytes before the end (no 0x0C marker check). A file shorter
    // than that would seek before its start: not modelled (taken as unreadable).
    if (pcx.size() < 0x300) return false;
    const std::uint8_t* m = pcx.data() + (pcx.size() - 0x300);
    for (std::size_t i = 0; i < 256; ++i) {
        out[i * 4] = m[i * 3];
        out[i * 4 + 1] = m[i * 3 + 1];
        out[i * 4 + 2] = m[i * 3 + 2];
        out[i * 4 + 3] = 0;
    }
    return true;
}

namespace {

// 0x4BC290: fopen(path, "wb") relative to the working folder (the game folder), fwrite, fclose.
// No folder is made: without a palettes folder in the game folder nothing is written.
void write_cache(Vfs& vfs, const std::string& path, const std::uint8_t* data, std::size_t n) {
    const std::filesystem::path p = vfs.disk_file(path);
#ifdef _WIN32
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") != 0) f = nullptr;
#else
    std::FILE* f = std::fopen(p.c_str(), "wb");
#endif
    if (!f) return;
    std::fwrite(data, 1, n, f);
    std::fclose(f);
}

// 0x4BBC30 → DeleteFileA(path) relative to the game folder; failures ignored.
void delete_loose(Vfs& vfs, const std::string& path) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(vfs.disk_file(path), ec)) std::filesystem::remove(vfs.disk_file(path), ec);
}

// Hapi_LoadFileToMemory and the copy of `n` bytes out of it (0x4BAAD0 / 0x4BAB00 / 0x4BAB30,
// 0x42A400): false when the read fails.
bool load_exact(Vfs& vfs, const std::string& path, std::size_t n, std::vector<std::uint8_t>& out) {
    std::vector<std::uint8_t> d;
    if (!vfs.read(path, d)) return false;
    out.assign(n, 0);
    for (std::size_t i = 0; i < n && i < d.size(); ++i) out[i] = d[i];
    return true;
}

}  // namespace

bool load_palette_tables(Vfs& vfs, bool side_effects, PaletteTables& out, std::string& fatal) {
    out = PaletteTables{};
    // 0x429330("PALETTE").
    {
        const std::string pal_path = vfs.virtual_path("palettes", "PALETTE", "PAL");
        if (vfs.size_or_zero(pal_path) == 0) {
            const std::string pcx_path = vfs.virtual_path("palettes", "PALETTE", "PCX");
            std::vector<std::uint8_t> pcx;
            if (!vfs.read(pcx_path, pcx) || !pcx_palette(pcx, out.palette)) {
                fatal = pcx_path;
                return false;
            }
            out.built_palette = true;
            if (side_effects) {
                write_cache(vfs, pal_path, out.palette.data(), out.palette.size());
                delete_loose(vfs, vfs.virtual_path("palettes", "PALETTE", "ALP"));
                delete_loose(vfs, vfs.virtual_path("palettes", "PALETTE", "LHT"));
                delete_loose(vfs, vfs.virtual_path("palettes", "PALETTE", "SHD"));
            }
        } else {
            std::vector<std::uint8_t> d;
            if (!load_exact(vfs, pal_path, 1024, d)) {
                fatal = pal_path;
                return false;
            }
            std::copy(d.begin(), d.end(), out.palette.begin());
        }
    }
    // 0x42E140 / 0x42E1D0 / 0x42E260 (in that order).
    struct Table {
        const char* ext;
        std::size_t size;
        std::vector<std::uint8_t> (*build)(const PaletteBytes&);
        std::vector<std::uint8_t>* dest;
        bool* built;
    };
    const Table tables[] = {
        {"ALP", 65536, &build_alpha_table, &out.alpha, &out.built_alpha},
        {"SHD", 8192, &build_shade_table, &out.shade, &out.built_shade},
        {"LHT", 8192, &build_light_table, &out.light, &out.built_light},
    };
    for (const Table& t : tables) {
        const std::string path = vfs.virtual_path("palettes", "PALETTE", t.ext);
        if (vfs.size_or_zero(path) != 0) {
            if (!load_exact(vfs, path, t.size, *t.dest)) {
                fatal = path;
                return false;
            }
            continue;
        }
        *t.dest = t.build(out.palette);
        *t.built = true;
        if (side_effects) write_cache(vfs, path, t.dest->data(), t.dest->size());
    }
    return true;
}

}  // namespace ta
