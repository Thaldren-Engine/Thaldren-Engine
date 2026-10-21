// The colour tables the game computes from its palette when their files are missing (the
// original's 0x4BA750 mixing, 0x4BADF0 shade, 0x4BABD0 light) against the files the game
// ships (palettes\PALETTE.ALP / .SHD / .LHT, which those routines wrote).
//
// Usage: palette_check <game_dir>

#include <cstdio>
#include <string>
#include <vector>

#include "render/draw_list.h"
#include "ta/hpi.h"
#include "ta/render_assets.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: palette_check <game_dir>\n");
        return 2;
    }
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    render::Palette palette;
    if (!ta::load_palette(vfs, palette)) {
        std::fprintf(stderr, "no palettes\\PALETTE.PAL\n");
        return 1;
    }
    int failures = 0;
    auto compare = [&](const char* file, const std::vector<std::uint8_t>& built) {
        std::vector<std::uint8_t> shipped;
        if (!vfs.read(std::string("palettes\\") + file, shipped)) {
            std::printf("%s: not in the game files\n", file);
            ++failures;
            return;
        }
        std::size_t diff = 0, first = built.size();
        for (std::size_t i = 0; i < built.size() && i < shipped.size(); ++i)
            if (built[i] != shipped[i]) {
                if (!diff) first = i;
                ++diff;
            }
        std::printf("%s: %zu bytes shipped, %zu computed, %zu differ", file, shipped.size(), built.size(), diff);
        if (diff) std::printf(" (first at %zu: shipped %u, computed %u)", first, shipped[first], built[first]);
        std::printf("\n");
        if (diff || shipped.size() < built.size()) ++failures;
    };
    const render::BlendTable alp = render::mixing_table(palette);
    compare("PALETTE.ALP", std::vector<std::uint8_t>(alp.begin(), alp.end()));
    compare("PALETTE.SHD", render::shade_levels(palette));
    compare("PALETTE.LHT", render::light_levels(palette));
    return failures ? 1 : 0;
}
