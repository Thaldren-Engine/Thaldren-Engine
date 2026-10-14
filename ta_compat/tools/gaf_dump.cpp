// Lists the sequences of a GAF file from a game directory's virtual file system
// (name, frame count, first frame size and hotspot) and optionally writes every
// frame of the matching sequences as 8-bit BMPs with the game palette.
// Usage: gaf_dump <game_dir> <path-inside-vfs> [<out_prefix> [<sequence name>]]

#include <cstdio>
#include <string>
#include <vector>

#include "render/presenter.h"
#include "ta/gaf.h"
#include "ta/hpi.h"
#include "ta/render_assets.h"
#include "ta/tdf.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: gaf_dump <game_dir> <path> [<out_prefix> [<sequence>]]\n");
        return 2;
    }
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    std::vector<std::uint8_t> data;
    if (!vfs.read(argv[2], data)) {
        std::fprintf(stderr, "not found: %s\n", argv[2]);
        return 1;
    }
    ta::GafDirectory dir;
    if (!ta::gaf_read_directory(data, dir)) {
        std::fprintf(stderr, "not a GAF file\n");
        return 1;
    }
    render::Palette palette{};
    ta::load_palette(vfs, palette);
    const std::string prefix = argc > 3 ? argv[3] : "";
    const std::string only = argc > 4 ? argv[4] : "";
    for (std::size_t s = 0; s < dir.sequence_names.size(); ++s) {
        std::vector<render::Sprite8> frames;
        ta::gaf_sequence_frames(data, static_cast<int>(s), frames);
        std::printf("%3zu %-32s %3zu frames", s, dir.sequence_names[s].c_str(), frames.size());
        if (!frames.empty())
            std::printf("  %dx%d at (%d,%d) key %d", frames[0].width, frames[0].height, frames[0].x_offset, frames[0].y_offset,
                        frames[0].key);
        std::printf("\n");
        if (prefix.empty() || (!only.empty() && ta::stricmp_c(only, dir.sequence_names[s]) != 0)) continue;
        for (std::size_t f = 0; f < frames.size(); ++f) {
            render::Image8 img(frames[f].width, frames[f].height, frames[f].key);
            img.pixels = frames[f].pixels;
            write_bmp(prefix + dir.sequence_names[s] + "_" + std::to_string(f) + ".bmp", img, palette);
        }
    }
    return 0;
}
