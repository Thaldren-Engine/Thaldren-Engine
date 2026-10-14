// Mounts a TA game directory the way the game does and decodes every file in
// every archive found there (mounted or not), reporting any failure.
//
// Usage: hpi_check <game_dir> [--list]

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "ta/hpi.h"

namespace {

struct Stats {
    std::size_t files = 0;
    std::size_t bytes = 0;
    std::size_t failures = 0;
    std::size_t compressed = 0;
};

void walk(ta::HpiArchive& a, const ta::HpiEntry& dir, const std::string& prefix, Stats& s, bool list) {
    std::vector<std::uint8_t> data;
    for (const auto& e : dir.children) {
        const std::string path = prefix.empty() ? e.name : prefix + "\\" + e.name;
        if (e.is_dir) {
            walk(a, e, path, s, list);
            continue;
        }
        const ta::HpiError err = a.read(e, data);
        ++s.files;
        if (e.compression) ++s.compressed;
        if (err != ta::HpiError::None) {
            ++s.failures;
            std::printf("  FAIL %s: %s (size %u, compression %u)\n", path.c_str(), ta::to_string(err), e.size, e.compression);
        } else {
            s.bytes += data.size();
            if (list) std::printf("  %s %u\n", path.c_str(), e.size);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: hpi_check <game_dir> [--list]\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    const bool list = argc > 2 && std::string(argv[2]) == "--list";

    ta::Vfs vfs;
    std::vector<std::string> mounted;
    vfs.mount_game_dir(dir, &mounted);
    std::printf("mount order (%zu):\n", mounted.size());
    for (const auto& m : mounted) std::printf("  %s\n", m.c_str());

    std::size_t total_fail = 0;
    for (const auto& de : std::filesystem::directory_iterator(dir)) {
        if (!de.is_regular_file()) continue;
        auto ext = de.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".hpi" && ext != ".ufo" && ext != ".ccx" && ext != ".gp3") continue;
        ta::HpiError err = ta::HpiError::None;
        auto a = ta::HpiArchive::open(de.path(), err);
        if (!a) {
            std::printf("%s: OPEN FAILED (%s)\n", de.path().filename().string().c_str(), ta::to_string(err));
            ++total_fail;
            continue;
        }
        Stats s;
        walk(*a, a->root(), "", s, list);
        std::printf("%-16s files %6zu  compressed %6zu  bytes %11zu  failures %zu\n",
                    de.path().filename().string().c_str(), s.files, s.compressed, s.bytes, s.failures);
        total_fail += s.failures;
    }
    const auto* m = ta::sqsh_method_counts();
    std::printf("chunks by method: 0=%llu lz77=%llu zlib=%llu 3=%llu\n", static_cast<unsigned long long>(m[0]),
                static_cast<unsigned long long>(m[1]), static_cast<unsigned long long>(m[2]),
                static_cast<unsigned long long>(m[3]));
    std::printf("total failures: %zu\n", total_fail);
    return total_fail == 0 ? 0 : 1;
}
