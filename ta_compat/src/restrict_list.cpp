#include "ta/restrict_list.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include "ta/tdf.h"

namespace ta {

namespace {

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}

}  // namespace

// 0x44B230: the table's size less one, then each listed definition's key and count.
void write_restrict_list(const std::vector<std::uint32_t>& def_keys, const std::vector<RestrictListRow>& rows, std::vector<std::uint8_t>& out) {
    out.clear();
    const std::int32_t n = static_cast<std::int32_t>(def_keys.size());
    put32(out, static_cast<std::uint32_t>(n - 1));
    for (std::int32_t i = 1; i < n; ++i)
        for (const RestrictListRow& r : rows)
            if (r.def_index == i) {
                put32(out, def_keys[static_cast<std::size_t>(i)]);
                put32(out, static_cast<std::uint32_t>(r.count));
                break;
            }
}

// 0x44B140: fread of one 4-byte item at a time; at the end of the file the bytes there are
// copied over the word's low bytes and the rest stays.
int read_restrict_list(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint32_t>& def_keys, std::vector<RestrictListRow>& rows) {
    std::size_t at = 0;
    auto read32 = [&](std::uint32_t& v) {
        const std::size_t got = std::min<std::size_t>(4, bytes.size() - at);
        std::uint8_t b[4];
        std::memcpy(b, &v, 4);
        for (std::size_t k = 0; k < got; ++k) b[k] = bytes[at + k];
        std::memcpy(&v, b, 4);
        at += got;
        return got == 4;
    };
    // (The pair's words start as whatever the stack held; taken as 0 here, see RE_MEMORY.)
    std::uint32_t n = 0, key = 0, count = 0;
    if (!read32(n)) return 0;  // (no count: the original loops on a stale word, nothing matches)
    int whole = 0;
    for (std::int32_t p = 0; p < static_cast<std::int32_t>(n); ++p) {
        const bool a = read32(key), b = read32(count);
        if (a && b) ++whole;
        const std::int32_t defs = static_cast<std::int32_t>(def_keys.size());
        for (std::int32_t i = 1; i < defs; ++i)
            if (def_keys[static_cast<std::size_t>(i)] == key) {
                for (RestrictListRow& r : rows)
                    if (r.def_index == i) {
                        r.count = static_cast<std::int32_t>(count);
                        break;
                    }
                break;
            }
        // Past the end every further pair is the same one again: nothing more changes.
        if (at >= bytes.size() && !(a && b)) break;
    }
    return whole;
}

std::vector<std::filesystem::path> list_files_newest_first(const std::string& dir, const std::string& ext) {
    struct Found {
        std::filesystem::path path;
        std::string upper;
        std::int64_t time = 0;
    };
    std::vector<Found> found;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.' || e.is_directory(ec)) continue;
        if (stricmp_c(e.path().extension().string(), "." + ext) != 0) continue;
        Found f;
        f.path = e.path();
        for (char c : name) f.upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        const auto t = std::filesystem::last_write_time(e.path(), ec);
        f.time = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
        found.push_back(f);
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.upper < b.upper; });
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return static_cast<std::int32_t>(b.time - a.time) < 0; });
    std::vector<std::filesystem::path> out;
    for (const Found& f : found) out.push_back(f.path);
    return out;
}

std::string name_without_extension(const std::string& file_name) {
    const auto dot = file_name.rfind('.');
    return dot == std::string::npos ? file_name : file_name.substr(0, dot);
}

}  // namespace ta
