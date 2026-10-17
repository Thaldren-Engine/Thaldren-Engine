#include "ta/hpi.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <span>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

// Directory enumeration order on NTFS: names compared after upper-casing.
[[maybe_unused]] std::string upper(std::string_view s) {
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

std::unique_ptr<HpiArchive> HpiArchive::open(const std::filesystem::path& path, HpiError& err, bool keep_open) {
    std::unique_ptr<HpiArchive> a(new HpiArchive());
    a->path_ = path;
    a->keep_open_ = keep_open;
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

namespace {

// The game's tree lookup: '\\'-separated components, each searched from the last
// entry to the first (case-insensitive); the first hit decides.
template <class Entry> Entry* tree_find(Entry* node, std::string_view path) {
    while (true) {
        const auto sep = path.find('\\');
        const std::string_view part = path.substr(0, sep);
        Entry* hit = nullptr;
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

void clear_shadows(HpiEntry& node) {
    for (auto& e : node.children) {
        e.shadowed = false;
        if (e.is_dir) clear_shadows(e);
    }
}

} // namespace

const HpiEntry* HpiArchive::find(std::string_view path) const { return tree_find(&root_, path); }
HpiEntry* HpiArchive::find(std::string_view path) { return tree_find(&root_, path); }

const HpiEntry* HpiArchive::find_dir_of_pattern(std::string_view pattern) const {
    const HpiEntry* node = &root_;
    auto sep = pattern.find('\\');
    while (sep != std::string_view::npos) {
        const std::string_view part = pattern.substr(0, sep);
        const HpiEntry* hit = nullptr;
        for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
            if (iequals(it->name, part)) {
                hit = &*it;
                break;
            }
        }
        if (!hit || !hit->is_dir) return nullptr;
        node = hit;
        pattern.remove_prefix(sep + 1);
        sep = pattern.find('\\');
    }
    return node;
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

HpiError HpiArchive::read_stored(const HpiEntry& f, std::vector<std::uint8_t>& out) {
    out.clear();
    if (f.is_dir) return HpiError::ReadFailed;
    if (f.compression == 0) {
        out.resize(f.size);
        return read_raw(f.data_offset, out.data(), f.size) ? HpiError::None : HpiError::ReadFailed;
    }
    const std::uint32_t blocks = (f.size + kBlockSize - 1) / kBlockSize;
    out.resize(blocks * 4u);
    if (!read_raw(f.data_offset, out.data(), out.size())) return HpiError::ReadFailed;
    std::uint64_t total = 0;
    for (std::uint32_t b = 0; b < blocks; ++b) total += le32(&out[b * 4u]);
    const std::size_t at = out.size();
    out.resize(at + static_cast<std::size_t>(total));
    return read_raw(f.data_offset + blocks * 4u, out.data() + at, static_cast<std::size_t>(total)) ? HpiError::None : HpiError::ReadFailed;
}

// ------------------------------------------------------------------ writer

namespace {

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
void set32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = static_cast<std::uint8_t>(x >> (8 * i));
}

// LZ77 stream for decompress_lz77: greedy matches against the bytes already
// written (window position = output position + 1), then the end marker.
std::vector<std::uint8_t> lz77_encode(const std::uint8_t* in, std::size_t n) {
    std::vector<std::uint8_t> out;
    std::size_t flag_at = 0;
    int bit = 8;
    auto token = [&](bool match) {
        if (bit == 8) {
            flag_at = out.size();
            out.push_back(0);
            bit = 0;
        }
        if (match) out[flag_at] = static_cast<std::uint8_t>(out[flag_at] | (1u << bit));
        ++bit;
    };
    std::size_t p = 0;
    while (p < n) {
        std::size_t best_len = 0, best_q = 0;
        const std::size_t lo = p > 4000 ? p - 4000 : 0;
        for (std::size_t q = lo; q < p; ++q) {
            if (((q + 1) & 0xFFF) == 0) continue;  // window offset 0 is the end marker
            std::size_t len = 0;
            while (len < 17 && p + len < n && in[q + len] == in[p + len]) ++len;
            if (len > best_len) {
                best_len = len;
                best_q = q;
            }
        }
        if (best_len >= 2) {
            token(true);
            const std::uint32_t v = static_cast<std::uint32_t>(((best_q + 1) & 0xFFF) << 4) | static_cast<std::uint32_t>(best_len - 2);
            out.push_back(static_cast<std::uint8_t>(v));
            out.push_back(static_cast<std::uint8_t>(v >> 8));
            p += best_len;
        } else {
            token(false);
            out.push_back(in[p++]);
        }
    }
    token(true);
    out.push_back(0);
    out.push_back(0);
    return out;
}

// zlib stream made of stored DEFLATE blocks.
std::vector<std::uint8_t> zlib_stored(const std::uint8_t* in, std::size_t n) {
    std::vector<std::uint8_t> out{0x78, 0x01};
    std::size_t p = 0;
    do {
        const std::size_t len = std::min<std::size_t>(n - p, 0xFFFF);
        out.push_back(p + len == n ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(len));
        out.push_back(static_cast<std::uint8_t>(len >> 8));
        out.push_back(static_cast<std::uint8_t>(~len));
        out.push_back(static_cast<std::uint8_t>(~len >> 8));
        out.insert(out.end(), in + p, in + p + len);
        p += len;
    } while (p < n);
    std::uint32_t a = 1, b = 0;
    for (std::size_t i = 0; i < n; ++i) {
        a = (a + in[i]) % 65521;
        b = (b + a) % 65521;
    }
    const std::uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(adler >> (8 * i)));
    return out;
}

struct WNode {
    std::string name;
    bool is_dir = false;
    std::vector<WNode> children;
    std::size_t file = 0;  // index into the files
};

void layout_node(const WNode& node, std::vector<std::uint8_t>& dir, std::size_t node_at, std::vector<std::size_t>& record_at) {
    // node: count, entries offset; then the entries, names, children and file records.
    set32(dir, node_at, static_cast<std::uint32_t>(node.children.size()));
    const std::size_t entries_at = dir.size();
    set32(dir, node_at + 4, static_cast<std::uint32_t>(entries_at));
    dir.resize(dir.size() + node.children.size() * 9);
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        const WNode& c = node.children[i];
        const std::size_t e = entries_at + i * 9;
        set32(dir, e, static_cast<std::uint32_t>(dir.size()));
        dir.insert(dir.end(), c.name.begin(), c.name.end());
        dir.push_back(0);
        dir[e + 8] = c.is_dir ? 1 : 0;
        if (c.is_dir) {
            const std::size_t child_at = dir.size();
            set32(dir, e + 4, static_cast<std::uint32_t>(child_at));
            dir.resize(dir.size() + 8);
            layout_node(c, dir, child_at, record_at);
        } else {
            set32(dir, e + 4, static_cast<std::uint32_t>(dir.size()));
            record_at[c.file] = dir.size();
            dir.resize(dir.size() + 9);
        }
    }
}

} // namespace

std::vector<std::uint8_t> hpi_build(const std::vector<HpiWriteFile>& files, std::uint8_t key) {
    WNode root;
    root.is_dir = true;
    for (std::size_t f = 0; f < files.size(); ++f) {
        WNode* node = &root;
        std::string_view p = files[f].path;
        while (true) {
            const auto sep = p.find('\\');
            const std::string part(p.substr(0, sep));
            if (sep == std::string_view::npos) {
                WNode leaf;
                leaf.name = part;
                leaf.file = f;
                node->children.push_back(std::move(leaf));
                break;
            }
            WNode* sub = nullptr;
            for (auto& c : node->children)
                if (c.is_dir && c.name == part) sub = &c;
            if (!sub) {
                WNode d;
                d.name = part;
                d.is_dir = true;
                node->children.push_back(std::move(d));
                sub = &node->children.back();
            }
            node = sub;
            p.remove_prefix(sep + 1);
        }
    }
    std::vector<std::uint8_t> out(20, 0);
    std::vector<std::size_t> record_at(files.size(), 0);
    const std::size_t root_at = out.size();
    out.resize(out.size() + 8);
    layout_node(root, out, root_at, record_at);
    const std::size_t dir_size = out.size();
    // File data.
    for (std::size_t f = 0; f < files.size(); ++f) {
        const HpiWriteFile& w = files[f];
        const std::size_t data_at = out.size();
        std::uint32_t size = 0;
        std::uint8_t compression = 0;
        if (w.copy_stored) {
            out.insert(out.end(), w.stored.begin(), w.stored.end());
            size = w.stored_size;
            compression = w.stored_compression;
        } else if (w.compression == 0) {
            out.insert(out.end(), w.data.begin(), w.data.end());
            size = static_cast<std::uint32_t>(w.data.size());
        } else {
            size = static_cast<std::uint32_t>(w.data.size());
            compression = w.compression;
            const std::size_t blocks = (w.data.size() + kBlockSize - 1) / kBlockSize;
            const std::size_t table_at = out.size();
            out.resize(out.size() + blocks * 4);
            for (std::size_t b = 0; b < blocks; ++b) {
                const std::size_t from = b * kBlockSize;
                const std::size_t n = std::min<std::size_t>(kBlockSize, w.data.size() - from);
                std::vector<std::uint8_t> payload =
                    w.compression == 1 ? lz77_encode(w.data.data() + from, n) : zlib_stored(w.data.data() + from, n);
                if (w.scramble_chunks)
                    for (std::size_t i = 0; i < payload.size(); ++i)
                        payload[i] = static_cast<std::uint8_t>((payload[i] ^ static_cast<std::uint8_t>(i)) + static_cast<std::uint8_t>(i));
                std::uint32_t sum = 0;
                for (std::uint8_t x : payload) sum += x;
                std::vector<std::uint8_t> chunk{'S', 'Q', 'S', 'H', 2, w.compression, static_cast<std::uint8_t>(w.scramble_chunks ? 1 : 0)};
                put32(chunk, static_cast<std::uint32_t>(payload.size()));
                put32(chunk, static_cast<std::uint32_t>(n));
                put32(chunk, sum);
                chunk.insert(chunk.end(), payload.begin(), payload.end());
                set32(out, table_at + b * 4, static_cast<std::uint32_t>(chunk.size()));
                out.insert(out.end(), chunk.begin(), chunk.end());
            }
        }
        set32(out, record_at[f], static_cast<std::uint32_t>(data_at));
        set32(out, record_at[f] + 4, size);
        out[record_at[f] + 8] = compression;
    }
    std::memcpy(out.data(), "HAPI", 4);
    set32(out, 4, 0x00010000u);
    set32(out, 8, static_cast<std::uint32_t>(dir_size));
    out[12] = key;
    set32(out, 16, static_cast<std::uint32_t>(root_at));
    // Obscure everything after the header (the reader's transform is its own inverse
    // up to the complement: stored = ~(plain ^ pos ^ key')).
    if (key) {
        const auto k = static_cast<std::uint8_t>(~((key >> 6) | (key << 2)));
        for (std::size_t i = 20; i < out.size(); ++i)
            out[i] = static_cast<std::uint8_t>(~(out[i] ^ static_cast<std::uint8_t>(i) ^ k));
    }
    const char* trailer = "Copyright 1998 Cavedog Entertainment";
    out.insert(out.end(), trailer, trailer + std::strlen(trailer));
    return out;
}

// ------------------------------------------------------------------ file system

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

namespace {

bool is_absolute_game_path(std::string_view p) {
    return (p.size() >= 2 && p[1] == ':') || (!p.empty() && (p[0] == '\\' || p[0] == '/'));
}

std::string join_game_path(const std::filesystem::path& root, std::string_view rel) {
    if (root.empty() || is_absolute_game_path(rel)) return std::string(rel);
    std::string r = root.string();
    if (!r.empty() && r.back() != '\\' && r.back() != '/') r += '\\';
    r += rel;
    return r;
}

#ifndef _WIN32
// Case-insensitive resolution of a '\\'- or '/'-separated path (the game's disk is case-insensitive).
std::filesystem::path resolve_ci(const std::string& joined) {
    std::string p = joined;
    std::replace(p.begin(), p.end(), '\\', '/');
    std::filesystem::path cur = p.size() && p[0] == '/' ? std::filesystem::path("/") : std::filesystem::path(".");
    std::size_t at = p.size() && p[0] == '/' ? 1 : 0;
    while (at <= p.size()) {
        const std::size_t sep = p.find('/', at);
        const std::string part = p.substr(at, sep == std::string::npos ? std::string::npos : sep - at);
        if (!part.empty()) {
            std::filesystem::path next = cur / part;
            std::error_code ec;
            if (!std::filesystem::exists(next, ec)) {
                for (const auto& de : std::filesystem::directory_iterator(cur, ec))
                    if (iequals(de.path().filename().string(), part)) {
                        next = de.path();
                        break;
                    }
            }
            cur = next;
        }
        if (sep == std::string::npos) break;
        at = sep + 1;
    }
    return cur;
}
#endif

std::filesystem::path disk_path(const std::filesystem::path& root, std::string_view rel) {
#ifdef _WIN32
    return std::filesystem::path(join_game_path(root, rel));
#else
    return resolve_ci(join_game_path(root, rel));
#endif
}

} // namespace

// The game's finder, step by step (a later scan can mount archives while one runs,
// and marking shadows changes what later steps see, so nothing is precomputed past
// the disk listing).
class Vfs::Finder {
public:
    Finder(Vfs& vfs, std::string_view pattern, int source, bool all) : vfs_(vfs), pattern_(pattern), source_(source), all_(all) {
        const auto sep = pattern.rfind('\\');
        file_pat_ = std::string(sep == std::string_view::npos ? pattern : pattern.substr(sep + 1));
        if (file_pat_ == "*.*") file_pat_ = "*";
        if (source_ == -1) {
            disk_ = vfs_.list_disk(pattern);
            if (disk_.empty()) {
                if (!all_) {
                    done_ = true;
                    return;
                }
                source_ = 0;
            }
        }
        index_ = -1;
    }

    bool next(VfsFound& out) {
        if (done_) return false;
        if (source_ < 0) {
            if (disk_pos_ < disk_.size()) {
                out = disk_[disk_pos_++];
                return true;
            }
            source_ = 0;
            index_ = -1;
            if (!all_) return end();
        }
        while (source_ < static_cast<int>(vfs_.archives_.size())) {
            bool have_node = index_ >= 0;
            if (!have_node) {
                node_ = vfs_.archives_[static_cast<std::size_t>(source_)]->find_dir_of_pattern(pattern_);
                have_node = node_ != nullptr;
            }
            if (have_node) {
                ++index_;
                while (index_ < static_cast<int>(node_->children.size())) {
                    const HpiEntry& e = node_->children[static_cast<std::size_t>(index_)];
                    if (wildcard_match(e.name, file_pat_) && !e.shadowed) {
                        out.name = e.name;
                        out.attrib = e.is_dir ? 0x11u : 1u;
                        out.size = e.is_dir ? 0u : e.size;
                        out.source = source_;
                        return true;
                    }
                    ++index_;
                }
                if (!all_) return end();
            }
            // A missing directory moves on to the next archive even without `all`.
            ++source_;
            index_ = -1;
        }
        return end();
    }

private:
    bool end() {
        done_ = true;
        return false;
    }
    Vfs& vfs_;
    std::string pattern_, file_pat_;
    int source_;
    bool all_;
    bool done_ = false;
    std::vector<VfsFound> disk_;
    std::size_t disk_pos_ = 0;
    const HpiEntry* node_ = nullptr;
    int index_ = -1;
};

std::vector<VfsFound> Vfs::list_disk(std::string_view pattern) const {
    std::vector<VfsFound> out;
    const std::string full = join_game_path(root_, pattern);
#ifdef _WIN32
    // The original asks the operating system (FindFirstFile): its matching (8.3
    // names included) and its order (NTFS: upper-cased name order) are used as they are.
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(full.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        VfsFound f;
        f.name = fd.cFileName;
        f.attrib = fd.dwFileAttributes == FILE_ATTRIBUTE_NORMAL ? 0u : static_cast<std::uint32_t>(fd.dwFileAttributes);
        f.size = fd.nFileSizeLow;
        f.source = -1;
        out.push_back(std::move(f));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    // Elsewhere: NTFS-like behaviour ("." and ".." first in a subdirectory, then
    // names in upper-cased order), matched with the game's wildcard rules.
    const auto sep = full.find_last_of("\\/");
    const std::string dir = sep == std::string::npos ? std::string(".") : full.substr(0, sep + 1);
    std::string pat = sep == std::string::npos ? full : full.substr(sep + 1);
    if (pat == "*.*") pat = "*";
    const std::filesystem::path d = resolve_ci(dir);
    std::error_code ec;
    if (!std::filesystem::is_directory(d, ec)) return out;
    std::vector<VfsFound> names;
    for (const auto& de : std::filesystem::directory_iterator(d, ec)) {
        VfsFound f;
        f.name = de.path().filename().string();
        f.attrib = de.is_directory(ec) ? 0x10u : 0x20u;
        f.size = de.is_regular_file(ec) ? static_cast<std::uint32_t>(de.file_size(ec)) : 0u;
        names.push_back(std::move(f));
    }
    std::sort(names.begin(), names.end(), [](const VfsFound& a, const VfsFound& b) { return upper(a.name) < upper(b.name); });
    if (std::filesystem::absolute(d, ec) != std::filesystem::absolute(d, ec).root_path())
        for (const char* dot : {".", ".."})
            if (wildcard_match(dot, pat)) out.push_back(VfsFound{dot, 0x10u, 0u, -1});
    for (auto& f : names)
        if (wildcard_match(f.name, pat)) out.push_back(std::move(f));
#endif
    return out;
}

std::vector<VfsFound> Vfs::find_files(std::string_view pattern, int source, bool all) {
    std::vector<VfsFound> out;
    Finder f(*this, pattern, source, all);
    VfsFound e;
    while (f.next(e)) out.push_back(e);
    return out;
}

bool Vfs::mount(const std::filesystem::path& archive, HpiError& err, bool keep_open) {
    std::error_code ec;
    const auto full = std::filesystem::absolute(archive, ec);
    for (const auto& a : archives_) {
        if (iequals(a->path().string(), full.string())) {
            err = HpiError::None;
            return false;  // already mounted
        }
    }
    auto a = HpiArchive::open(full, err, keep_open);
    if (!a) return false;
    archives_.push_back(std::move(a));
    return true;
}

void Vfs::mount_glob(std::string_view pattern, bool keep_open, int limit, std::vector<std::string>* log) {
    // The prefix of a CD pattern ("D:\") is kept for the archive's path; game
    // directory names are relative to the game directory.
    const auto sep = pattern.rfind('\\');
    const std::string prefix = sep == std::string_view::npos ? std::string() : std::string(pattern.substr(0, sep + 1));
    Finder f(*this, pattern, -1, true);
    VfsFound e;
    if (!f.next(e)) return;
    do {
        HpiError err = HpiError::None;
        const std::string name = prefix + e.name;
        if (mount(std::filesystem::path(join_game_path(root_, name)), err, keep_open)) {
            if (log) log->push_back(name);
            if (limit > 0) --limit;
        }
    } while (f.next(e) && limit != 0);
}

std::vector<std::string> Vfs::cd_drive_roots() const {
    if (cd_drives_set_) return cd_drives_;
    std::vector<std::string> out;
#ifdef _WIN32
    for (char c = 'A'; c <= 'Z'; ++c) {
        const char root[4] = {c, ':', '\\', 0};
        if (GetDriveTypeA(root) == DRIVE_CDROM) out.emplace_back(root);
    }
#endif
    return out;
}

void Vfs::mount_game_dir(const std::filesystem::path& game_dir, std::vector<std::string>* log) {
    root_ = game_dir;
    scan(log);
}

void Vfs::scan(std::vector<std::string>* log) {
    // Archives the game does not keep open are dropped when their file is gone.
    for (std::size_t i = 0; i < archives_.size();) {
        if (!archives_[i]->keep_open()) {
            std::ifstream probe(archives_[i]->path(), std::ios::binary);
            if (!probe) {
                archives_.erase(archives_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        ++i;
    }
    mount_glob("rev31.GP3", true, -1, log);
    mount_glob("*.CCX", true, -1, log);
    mount_glob("*.UFO", false, -1, log);
    mount_glob("*.HPI", false, 10, log);
    for (const std::string& cd : cd_drive_roots()) mount_glob(cd + "*.hpi", false, -1, log);
    refresh_shadows();
}

void Vfs::mark_shadows(const std::string& dir, int source, bool all) {
    Finder f(*this, dir + "*", source, all);
    VfsFound e;
    while (f.next(e)) {
        if (!e.is_dir()) {
            for (std::size_t k = e.source < 0 ? 0 : static_cast<std::size_t>(e.source) + 1; k < archives_.size(); ++k) {
                HpiEntry* hit = archives_[k]->find(dir + e.name);
                if (hit && !hit->is_dir) hit->shadowed = true;
            }
        } else if (e.name != "." && e.name != "..") {
            mark_shadows(dir + e.name + "\\", e.source, false);
        }
    }
}

void Vfs::refresh_shadows() {
    for (auto& a : archives_) clear_shadows(a->root());
    if (!archives_.empty()) mark_shadows("", -1, true);
}

Vfs::Located Vfs::locate(std::string_view path) {
    if (!root_.empty()) {
        // A loose file wins (the game opens the path relative to the game directory first).
        const std::filesystem::path p = disk_path(root_, path);
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) {
            std::ifstream probe(p, std::ios::binary);
            if (probe) return Located{-1, nullptr};
        }
    }
    for (std::size_t k = 0; k < archives_.size(); ++k) {
        const HpiEntry* e = archives_[k]->find(path);
        if (e && !e->is_dir) return Located{static_cast<int>(k), e};
    }
    return Located{};
}

std::uint32_t Vfs::size_or_zero(std::string_view path) {
    const Located l = locate(path);
    if (!l) return 0;
    if (l.source == -1) {
        std::error_code ec;
        const auto n = std::filesystem::file_size(disk_path(root_, path), ec);
        return ec ? 0u : static_cast<std::uint32_t>(n);
    }
    return l.entry->size;
}

bool Vfs::is_loose(std::string_view path) { return locate(path).source == -1; }

std::vector<std::string> Vfs::enumerate(std::string_view pattern, bool dirs_only) {
    std::vector<std::string> out;
    Finder f(*this, pattern, -1, true);
    VfsFound e;
    while (f.next(e)) {
        if (e.name == "." || e.name == "..") continue;
        if (dirs_only && !e.is_dir()) continue;
        out.push_back(e.name);
    }
    return out;
}

void Vfs::tree_walk(const std::string& dir, std::string_view file_pattern, int source, bool all, std::vector<std::string>& out) {
    Finder f(*this, dir + "\\*", source, all);
    VfsFound e;
    while (f.next(e)) {
        if (e.name == "." || e.name == "..") continue;
        if (!e.is_dir()) {
            if (wildcard_match(e.name, file_pattern)) out.push_back(dir + "\\" + e.name);
        } else {
            tree_walk(dir + "\\" + e.name, file_pattern, e.source, false, out);
        }
    }
}

std::vector<std::string> Vfs::enumerate_tree(std::string_view dir, std::string_view file_pattern, int source, bool all) {
    std::vector<std::string> out;
    tree_walk(std::string(dir), file_pattern, source, all, out);
    return out;
}

std::string Vfs::virtual_path(std::string_view dir, std::string_view name, std::string_view ext) {
    auto build = [&](std::string base) {
        std::string p = std::move(base);
        p += '\\';
        p += name;
        if (!ext.empty()) {
            const auto dot = p.rfind('.');
            if (dot != std::string::npos) p.resize(dot);
            p += '.';
            p += ext;
        }
        return p;
    };
    if (!language.empty()) {
        std::string localized = build(std::string(dir) + "-" + language);
        if (locate(localized)) return localized;
    }
    return build(std::string(dir));
}

bool Vfs::read(std::string_view path, std::vector<std::uint8_t>& out, std::string* source) {
    const Located l = locate(path);
    if (!l) return false;
    if (l.source == -1) {
        std::ifstream f(disk_path(root_, path), std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (source) *source = "<disk>";
        return true;
    }
    auto& a = archives_[static_cast<std::size_t>(l.source)];
    if (a->read(*l.entry, out) != HpiError::None) return false;
    if (source) *source = a->path().filename().string();
    return true;
}

} // namespace ta
