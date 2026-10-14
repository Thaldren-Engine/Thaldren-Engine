#include "ta/gaf.h"

#include <algorithm>
#include <cstring>

#include "ta/tdf.h"

namespace ta {

// Layout: u32 version, u32 sequence count (the game reads it as int16), u32 0,
// then one u32 file offset per sequence. Sequence entry: u16 frame count,
// u16, u32, char name[32], then frame entries.
bool gaf_read_directory(const std::vector<std::uint8_t>& file, GafDirectory& out) {
    out.sequence_names.clear();
    if (file.size() < 12) return false;
    std::int16_t count = 0;
    std::memcpy(&count, &file[4], 2);
    for (int i = 0; i < count; ++i) {
        const std::size_t at = 12 + static_cast<std::size_t>(i) * 4;
        if (at + 4 > file.size()) return false;
        std::uint32_t off = 0;
        std::memcpy(&off, &file[at], 4);
        if (static_cast<std::size_t>(off) + 8 + 32 > file.size()) return false;
        const char* name = reinterpret_cast<const char*>(&file[off + 8]);
        out.sequence_names.emplace_back(name, std::find(name, name + 32, '\0'));
    }
    return true;
}

int gaf_find_sequence(const GafDirectory& dir, const std::string& name) {
    for (std::size_t i = 0; i < dir.sequence_names.size(); ++i)
        if (stricmp_c(dir.sequence_names[i], name) == 0) return static_cast<int>(i);
    return -1;
}

} // namespace ta
