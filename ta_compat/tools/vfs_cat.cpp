// Prints a file from a TA game directory's virtual file system to stdout.
// Usage: vfs_cat <game_dir> <path-inside-vfs>

#include <cstdio>
#include <string>
#include <vector>

#include "ta/hpi.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: vfs_cat <game_dir> <path>\n");
        return 2;
    }
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    std::vector<std::uint8_t> data;
    std::string source;
    if (!vfs.read(argv[2], data, &source)) {
        std::fprintf(stderr, "not found: %s\n", argv[2]);
        return 1;
    }
    std::fprintf(stderr, "[%s, %zu bytes]\n", source.c_str(), data.size());
    std::fwrite(data.data(), 1, data.size(), stdout);
    return 0;
}
