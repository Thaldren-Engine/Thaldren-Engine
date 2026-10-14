#include "ta/hpi.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <span>

#include "ta/inflate.h"

namespace ta {
namespace {

constexpr char kTrailer[] = "Copyright 0000 Cavedog Entertainment";
constexpr std::uint32_t kBlockSize = 0x10000;
constexpr std::size_t kSqshHeaderSize = 19;

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Archive bytes are obscured with a key mixed with the low byte of the absolute file offset.
void unscramble(std::uint8_t* data, std::size_t size, std::uint32_t file_offset, std::uint8_t key) {
    if (!key) return;
    for (std::size_t i = 0; i < size; ++i) {
        const auto pos = static_cast<std::uint8_t>(file_offset + i);
        data[i] = static_cast<std::uint8_t>(pos ^ key ^ static_cast<std::uint8_t>(~data[i]));
    }
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

bool parse_node(const std::vector<std::uint8_t>& dir, std::uint32_t node_offset, HpiEntry& out, int depth) {
    if (depth > 64 || node_offset + 8 > dir.size()) return false;
    const std::uint32_t count = le32(&dir[node_offset]);
    const std::uint32_t entries = le32(&dir[node_offset + 4]);
    if (static_cast<std::uint64_t>(entries) + static_cast<std::uint64_t>(count) * 9 > dir.size()) return false;
    out.children.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* e = &dir[entries + i * 9];
        const std::uint32_t name_off = le32(e);
        const std::uint32_t data_off = le32(e + 4);
        HpiEntry child;
        if (name_off >= dir.size()) return false;
        const auto* name_begin = reinterpret_cast<const char*>(&dir[name_off]);
        child.name.assign(name_begin, strnlen(name_begin, dir.size() - name_off));
        child.is_dir = (e[8] & 1) != 0;
        if (child.is_dir) {
            if (!parse_node(dir, data_off, child, depth + 1)) return false;
        } else {
            if (data_off + 9 > dir.size()) return false;
            child.data_offset = le32(&dir[data_off]);
            child.size = le32(&dir[data_off + 4]);
            child.compression = dir[data_off + 8];
        }
        out.children.push_back(std::move(child));
    }
    return true;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// Directory enumeration order on NTFS: names compared after upper-casing.
std::string upper(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

} // namespace

const char* to_string(HpiError e) {
    switch (e) {
    case HpiError::None: return "ok";
    case HpiError::OpenFailed: return "open failed";
    case HpiError::BadHeader: return "bad header";
    case HpiError::BadTrailer: return "bad trailer";
    case HpiError::BadDirectory: return "bad directory";
    case HpiError::ReadFailed: return "read failed";
    case HpiError::BadChunk: return "bad chunk";
    case HpiError::BadChunkChecksum: return "bad chunk checksum";
    case HpiError::BadChunkSize: return "bad chunk size";
    case HpiError::BadCompression: return "bad compression";
    }
    return "?";
}

void decompress_lz77(const std::uint8_t* in, std::size_t in_size, std::vector<std::uint8_t>& out) {
    std::uint8_t window[0x1000] = {};
    std::uint32_t wpos = 1;
    std::size_t p = 0;
    if (in_size == 0) return;
    std::uint32_t flags = in[p++];
    std::uint32_t mask = 1;
    while (p < in_size) {
        if ((flags & mask) == 0) {
            const std::uint8_t b = in[p++];
            out.push_back(b);
            window[wpos] = b;
            wpos = (wpos + 1) & 0xFFF;
        } else {
            if (p + 2 > in_size) return;
            const std::uint32_t v = static_cast<std::uint32_t>(in[p]) | (static_cast<std::uint32_t>(in[p + 1]) << 8);
            p += 2;
            const std::uint32_t offset = v >> 4;
            if (offset == 0) return;  // end marker
            const std::uint32_t count = (v & 0xF) + 2;
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint8_t b = window[(offset + i) & 0xFFF];
                out.push_back(b);
                window[wpos] = b;
                wpos = (wpos + 1) & 0xFFF;
            }
        }
        mask <<= 1;
        if (mask & 0x100) {
            mask = 1;
            if (p >= in_size) return;
            flags = in[p++];
        }
    }
}

namespace {
std::uint64_t g_method_counts[4] = {};
}

const std::uint64_t* sqsh_method_counts() { return g_method_counts; }

HpiError unpack_sqsh(std::vector<std::uint8_t>& chunk, std::vector<std::uint8_t>& out) {
    if (chunk.size() < kSqshHeaderSize || std::memcmp(chunk.data(), "SQSH", 4) != 0) return HpiError::BadChunk;
    const std::uint8_t method = chunk[5];
    const bool scrambled = chunk[6] != 0;
    const std::uint32_t packed = le32(&chunk[7]);
    const std::uint32_t unpacked = le32(&chunk[11]);
    const std::uint32_t checksum = le32(&chunk[15]);
    if (method > 3) return HpiError::BadCompression;
    ++g_method_counts[method];
    if (kSqshHeaderSize + static_cast<std::uint64_t>(packed) > chunk.size()) return HpiError::BadChunk;
    std::uint8_t* payload = chunk.data() + kSqshHeaderSize;
    std::uint32_t sum = 0;
    for (std::uint32_t i = 0; i < packed; ++i) sum += payload[i];
    if (sum != checksum) return HpiError::BadChunkChecksum;
    if (scrambled) {
        for (std::uint32_t i = 0; i < packed; ++i) {
            payload[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(payload[i] - static_cast<std::uint8_t>(i)) ^
                                                   static_cast<std::uint8_t>(i));
        }
    }
    const std::size_t before = out.size();
    if (method == 1) {
        decompress_lz77(payload, packed, out);
    } else if (method == 2) {
        if (zlib_decompress(std::span<const std::uint8_t>(payload, packed), out) != InflateResult::Ok) {
            return HpiError::BadCompression;
        }
    } else {
        // Methods 0 and 3 produce no output in the original; the size check below fails.
    }
    return out.size() - before == unpacked ? HpiError::None : HpiError::BadChunkSize;
}

std::unique_ptr<HpiArchive> HpiArchive::open(const std::filesystem::path& path, HpiError& err) {
    std::unique_ptr<HpiArchive> a(new HpiArchive());
    a->path_ = path;
    a->file_.open(path, std::ios::binary);
    if (!a->file_) {
        err = HpiError::OpenFailed;
        return nullptr;
    }
    std::uint8_t hdr[20];
    if (!a->file_.read(reinterpret_cast<char*>(hdr), sizeof(hdr)) || std::memcmp(hdr, "HAPI", 4) != 0 ||
        le32(hdr + 4) != 0x00010000u) {
        err = HpiError::BadHeader;
        return nullptr;
    }
    // Trailer check (the game compares the year as "0000").
    const std::size_t tlen = sizeof(kTrailer) - 1;
    std::string trailer(tlen, '\0');
    a->file_.seekg(-static_cast<std::streamoff>(tlen), std::ios::end);
    if (!a->file_.read(trailer.data(), static_cast<std::streamsize>(tlen))) {
        err = HpiError::BadTrailer;
        return nullptr;
    }
    for (std::size_t i = 10; i < 14; ++i) trailer[i] = '0';
    if (trailer != kTrailer) {
        err = HpiError::BadTrailer;
        return nullptr;
    }

    const std::uint32_t dir_size = le32(hdr + 8);
    const std::uint8_t raw_key = hdr[12];
    a->key_ = raw_key ? static_cast<std::uint8_t>(~((raw_key >> 6) | (raw_key << 2))) : 0;
    if (dir_size < 20) {
        err = HpiError::BadDirectory;
        return nullptr;
    }
    std::vector<std::uint8_t> dir(dir_size);
    a->file_.clear();
    a->file_.seekg(0);
    if (!a->file_.read(reinterpret_cast<char*>(dir.data()), dir_size)) {
        err = HpiError::BadDirectory;
        return nullptr;
    }
    unscramble(dir.data() + 20, dir.size() - 20, 20, a->key_);
    a->root_.is_dir = true;
    if (!parse_node(dir, le32(hdr + 16), a->root_, 0)) {
        err = HpiError::BadDirectory;
        return nullptr;
    }
    err = HpiError::None;
    return a;
}

const HpiEntry* HpiArchive::find(std::string_view path) const {
    const HpiEntry* node = &root_;
    while (true) {
        const auto sep = path.find_first_of("\\/");
        const std::string_view part = path.substr(0, sep);
        const HpiEntry* hit = nullptr;
        for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
            if (iequals(it->name, part)) {
                hit = &*it;
                break;
            }
        }
        if (!hit) return nullptr;
        if (sep == std::string_view::npos) return hit;
        if (!hit->is_dir) return nullptr;
        node = hit;
        path.remove_prefix(sep + 1);
    }
}

bool HpiArchive::read_raw(std::uint32_t offset, std::uint8_t* dst, std::size_t size) {
    file_.clear();
    file_.seekg(offset);
    if (!file_.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size))) return false;
    unscramble(dst, size, offset, key_);
    return true;
}

HpiError HpiArchive::read(const HpiEntry& f, std::vector<std::uint8_t>& out) {
    out.clear();
    if (f.is_dir) return HpiError::ReadFailed;
    if (f.compression == 0) {
        out.resize(f.size);
        return read_raw(f.data_offset, out.data(), f.size) ? HpiError::None : HpiError::ReadFailed;
    }
    const std::uint32_t blocks = (f.size + kBlockSize - 1) / kBlockSize;
    std::vector<std::uint8_t> table(blocks * 4u);
    if (!read_raw(f.data_offset, table.data(), table.size())) return HpiError::ReadFailed;
    std::uint32_t pos = f.data_offset + blocks * 4u;
    out.reserve(f.size);
    std::vector<std::uint8_t> chunk;
    for (std::uint32_t b = 0; b < blocks; ++b) {
        const std::uint32_t csize = le32(&table[b * 4u]);
        chunk.resize(csize);
        if (!read_raw(pos, chunk.data(), csize)) return HpiError::ReadFailed;
        pos += csize;
        const HpiError e = unpack_sqsh(chunk, out);
        if (e != HpiError::None) return e;
    }
    return out.size() == f.size ? HpiError::None : HpiError::BadChunkSize;
}

bool Vfs::mount(const std::filesystem::path& archive, HpiError& err) {
    const auto full = std::filesystem::absolute(archive);
    for (const auto& a : archives_) {
        if (iequals(a->path().string(), full.string())) {
            err = HpiError::None;
            return false;  // already mounted
        }
    }
    auto a = HpiArchive::open(full, err);
    if (!a) return false;
    archives_.push_back(std::move(a));
    return true;
}

void Vfs::mount_game_dir(const std::filesystem::path& game_dir, std::vector<std::string>* log) {
    root_ = game_dir;
    std::vector<std::filesystem::path> files;
    for (const auto& de : std::filesystem::directory_iterator(game_dir)) {
        if (de.is_regular_file()) files.push_back(de.path());
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return upper(a.filename().string()) < upper(b.filename().string()); });
    auto mount_matching = [&](auto pred, int limit) {
        for (const auto& f : files) {
            if (limit == 0) break;
            if (!pred(lower(f.filename().string()))) continue;
            HpiError err = HpiError::None;
            if (mount(f, err)) {
                if (log) log->push_back(f.filename().string());
                if (limit > 0) --limit;
            }
        }
    };
    auto ext_is = [](const char* ext) {
        return [ext](const std::string& name) {
            const std::string e = ext;
            return name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0;
        };
    };
    mount_matching([](const std::string& name) { return name == "rev31.gp3"; }, -1);
    mount_matching(ext_is(".ccx"), -1);
    mount_matching(ext_is(".ufo"), -1);
    mount_matching(ext_is(".hpi"), 10);
}

bool wildcard_match(std::string_view name, std::string_view pattern) {
    // Set of live pattern positions, advanced one name character at a time
    // (the game keeps at most 100 states).
    auto up = [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); };
    auto pat = [&](std::size_t i) { return i < pattern.size() ? up(pattern[i]) : '\0'; };
    std::vector<std::size_t> states{0};
    for (char raw : name) {
        const char c = up(raw);
        // States appended by '*' are processed for this same character.
        std::size_t k = 0;
        while (k < states.size()) {
            const char p = pat(states[k]);
            if (p == '?' || p == c) {
                ++states[k];
            } else if (p == '*') {
                if (states.size() < 100) states.push_back(states[k] + 1);
            } else {
                // Drop this state: move the last one into its slot and re-examine the slot.
                states[k] = states.back();
                states.pop_back();
                if (states.empty()) return false;
                continue;
            }
            ++k;
        }
    }
    for (std::size_t s : states) {
        const char p = pat(s);
        if (p == '\0' || (p == '*' && pat(s + 1) == '\0')) return true;
    }
    return false;
}

std::uint32_t Vfs::size_or_zero(std::string_view path) {
    std::string native(path);
    std::replace(native.begin(), native.end(), '\\', '/');
    const auto loose = root_ / native;
    if (!root_.empty() && std::filesystem::is_regular_file(loose)) {
        return static_cast<std::uint32_t>(std::filesystem::file_size(loose));
    }
    for (auto& a : archives_) {
        const HpiEntry* e = a->find(path);
        if (e && !e->is_dir) return e->size;
    }
    return 0;
}

bool Vfs::is_loose(std::string_view path) {
    std::string native(path);
    std::replace(native.begin(), native.end(), '\\', '/');
    return !root_.empty() && std::filesystem::is_regular_file(root_ / native);
}

std::vector<std::string> Vfs::enumerate(std::string_view pattern) {
    std::string dir, file_pat(pattern);
    const auto sep = pattern.find_last_of("\\/");
    if (sep != std::string_view::npos) {
        dir = std::string(pattern.substr(0, sep));
        file_pat = std::string(pattern.substr(sep + 1));
    }
    if (file_pat == "*.*") file_pat = "*";
    std::vector<std::string> out;
    std::vector<std::string> seen_lower;
    auto seen = [&](const std::string& name) {
        const std::string l = lower(name);
        for (const auto& s : seen_lower)
            if (s == l) return true;
        return false;
    };
    // Loose files (NTFS order: upper-cased name order).
    std::string native = dir;
    std::replace(native.begin(), native.end(), '\\', '/');
    if (!root_.empty() && std::filesystem::is_directory(root_ / native)) {
        std::vector<std::string> names;
        for (const auto& de : std::filesystem::directory_iterator(root_ / native)) names.push_back(de.path().filename().string());
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return upper(a) < upper(b); });
        for (const auto& n : names) {
            if (wildcard_match(n, file_pat)) {
                out.push_back(n);
                seen_lower.push_back(lower(n));
            }
        }
    }
    for (auto& a : archives_) {
        const HpiEntry* node = dir.empty() ? &a->root() : a->find(dir);
        if (!node || !node->is_dir) continue;
        for (const auto& e : node->children) {
            if (!wildcard_match(e.name, file_pat)) continue;
            if (!e.is_dir && seen(e.name)) continue;  // shadowed by an earlier source
            out.push_back(e.name);
            if (!e.is_dir) seen_lower.push_back(lower(e.name));
        }
    }
    return out;
}

bool Vfs::read(std::string_view path, std::vector<std::uint8_t>& out, std::string* source) {
    std::string native(path);
    std::replace(native.begin(), native.end(), '\\', '/');
    const auto loose = root_ / native;
    if (!root_.empty() && std::filesystem::is_regular_file(loose)) {
        std::ifstream f(loose, std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (source) *source = "<disk>";
        return true;
    }
    for (auto& a : archives_) {
        const HpiEntry* e = a->find(path);
        if (e && !e->is_dir) {
            if (a->read(*e, out) != HpiError::None) return false;
            if (source) *source = a->path().filename().string();
            return true;
        }
    }
    return false;
}

} // namespace ta
