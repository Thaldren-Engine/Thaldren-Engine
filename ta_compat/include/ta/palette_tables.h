// The game palette and its three colour tables as the original gets them at start-up
// (spec: docs/RE_MEMORY.md, "Palette tables built when missing"):
//   palettes\PALETTE.PAL (256 x {r, g, b, x}) — loaded, or built from palettes\PALETTE.PCX's
//     colour map and cached (0x429330, from 0x42A400);
//   palettes\PALETTE.ALP (256 x 256 mixing table)  — loaded or built by 0x4BA750 (0x42E140);
//   palettes\PALETTE.SHD (32 x 256 shade levels)   — loaded or built by 0x4BADF0 (0x42E1D0);
//   palettes\PALETTE.LHT (32 x 256 light levels)   — loaded or built by 0x4BABD0 (0x42E260);
// a built table is written to its path in the game folder (0x4BC290: fopen "wb", so only when
// the folder exists).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ta/hpi.h"

namespace ta {

// A palette as the builders read it: 256 entries of 4 bytes, channels 0..2 used.
using PaletteBytes = std::array<std::uint8_t, 1024>;

// 0x4BA920: every entry's channel sum (bytes 0 + 1 + 2) and the entries' order, sorted by
// that sum with the original's exchange sort (for i, for j > i: sum[i] > sum[j] swaps both).
struct SortedPalette {
    std::array<std::int32_t, 256> sum{};
    std::array<std::uint8_t, 256> order{};
};
SortedPalette sort_palette(const PaletteBytes& pal);

// 0x4BA9D0: the palette entry nearest to colour (c0, c1, c2): among the sorted entries whose
// sum is within 40 of the colour's (the scan stops at the first one above), the first with the
// least squared distance. None within 40: the entry at the sorted position where the scan
// stopped (position 256 = 0).
std::uint8_t nearest_entry(const PaletteBytes& pal, const SortedPalette& sorted, std::uint8_t c0, std::uint8_t c1, std::uint8_t c2);

// 0x4BA750: entry [a * 256 + b] = a when a == b, else the entry nearest to the channel-wise
// halves ((pal[a] + pal[b]) / 2, truncated). 65536 bytes.
std::vector<std::uint8_t> build_alpha_table(const PaletteBytes& pal);
// 0x4BADF0: level n (0..31) scales every entry by the factor f_n (f_0 = 0, f_n+1 = f_n -
// (-0.06875), doubles), each channel __ftol(channel * f_n), above 255 (as a 16-bit value)
// 255; entry [n * 256 + e] = the nearest entry. 8192 bytes.
std::vector<std::uint8_t> build_shade_table(const PaletteBytes& pal);
// 0x4BABD0: level n (0..31) scales by 1.0 - n * (-1/30) (doubles), channels __ftol'd, above
// 255 (signed) 255; entry [n * 256 + e] = the nearest entry. 8192 bytes.
std::vector<std::uint8_t> build_light_table(const PaletteBytes& pal);

// 0x4CB080: the colour map of a PCX file (manufacturer 0x0A, version 5 in its 128-byte
// header; the map is the file's last 768 bytes) as a palette, byte 3 of each entry 0.
bool pcx_palette(const std::vector<std::uint8_t>& pcx, PaletteBytes& out);

struct PaletteTables {
    PaletteBytes palette{};
    std::vector<std::uint8_t> alpha;  // 65536
    std::vector<std::uint8_t> shade;  // 8192
    std::vector<std::uint8_t> light;  // 8192
    // Which ones were built (not loaded).
    bool built_palette = false, built_alpha = false, built_shade = false, built_light = false;
};

// The start-up sequence (0x42A400 → 0x429330, then 0x42E140, 0x42E1D0, 0x42E260): paths by the
// localized path builder (Vfs::virtual_path), a file of size 0 counts as missing. With
// `side_effects` the built files are written into the game folder and a PAL built from the PCX
// is written and then deletes the loose PALETTE.ALP, .LHT and .SHD files (so they are built
// again from the new palette); without,
// nothing on disk changes (later loads in the same run, e.g. a battle's). False on the
// original's fatal errors (0x4B6290: message box with the path, exit(1)): `fatal` = the path.
// A file shorter than its table is zero-filled past its end (the original reads past its
// buffer there).
bool load_palette_tables(Vfs& vfs, bool side_effects, PaletteTables& out, std::string& fatal);

}  // namespace ta
