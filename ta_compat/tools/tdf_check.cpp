// Parses every text definition file (.tdf .fbi .ota .gui .txt-free) in every
// archive of a TA game directory and reports parse errors and statistics.
//
// Usage: tdf_check <game_dir>

#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "ta/hpi.h"
#include "ta/tdf.h"

namespace {

struct Stats {
    std::size_t files = 0, sections = 0, keys = 0, errors = 0;
};

void count(const ta::TdfSection& s, Stats& st) {
    ++st.sections;
    st.keys += s.keys().size();
    for (const auto& c : s.children()) count(*c, st);
}

std::string ext_of(const std::string& name) {
    const auto dot = name.rfind('.');
    std::string e = dot == std::string::npos ? "" : name.substr(dot);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

void walk(ta::HpiArchive& a, const ta::HpiEntry& dir, const std::string& prefix, std::map<std::string, Stats>& by_ext) {
    std::vector<std::uint8_t> data;
    for (const auto& e : dir.children) {
        const std::string path = prefix.empty() ? e.name : prefix + "\\" + e.name;
        if (e.is_dir) {
            walk(a, e, path, by_ext);
            continue;
        }
        const std::string ext = ext_of(e.name);
        if (ext != ".tdf" && ext != ".fbi" && ext != ".ota" && ext != ".gui") continue;
        if (a.read(e, data) != ta::HpiError::None) continue;
        auto r = ta::tdf_parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
        auto& st = by_ext[ext];
        ++st.files;
        if (!r.error.empty()) {
            ++st.errors;
            std::printf("  ERROR %s\\%s: %s\n", a.path().filename().string().c_str(), path.c_str(), r.error.c_str());
        }
        if (r.root) count(*r.root, st);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: tdf_check <game_dir>\n");
        return 2;
    }
    std::map<std::string, Stats> by_ext;
    for (const auto& de : std::filesystem::directory_iterator(argv[1])) {
        if (!de.is_regular_file()) continue;
        const std::string ext = ext_of(de.path().filename().string());
        if (ext != ".hpi" && ext != ".ufo" && ext != ".ccx" && ext != ".gp3") continue;
        ta::HpiError err = ta::HpiError::None;
        auto a = ta::HpiArchive::open(de.path(), err);
        if (a) walk(*a, a->root(), "", by_ext);
    }
    std::size_t errors = 0;
    for (const auto& [ext, st] : by_ext) {
        std::printf("%-5s files %5zu  sections %6zu  keys %7zu  errors %zu\n", ext.c_str(), st.files, st.sections, st.keys,
                    st.errors);
        errors += st.errors;
    }
    return errors == 0 ? 0 : 1;
}
