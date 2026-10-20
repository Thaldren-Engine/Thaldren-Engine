// Smacker movie decoding (client/smacker.h; the format: docs/RE_MEMORY.md, "Movies").

#include "client/smacker.h"

#include <algorithm>
#include <cstring>

namespace client {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}

constexpr int kMaxDepth = 64;  // deeper trees are damaged files

int run_length(int code) {
    // Block runs: 1..59, then 128, 256, 512, 1024, 2048.
    return code < 59 ? code + 1 : 128 << (code - 59);
}

}  // namespace

// Bits from the lowest bit of each byte upwards; past the end every bit reads as 0.
class SmackerFile::Bits {
public:
    Bits(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}
    int bit() {
        if (pos_ >= n_ * 8) {
            ++pos_;
            return 0;
        }
        const int b = (p_[pos_ >> 3] >> (pos_ & 7)) & 1;
        ++pos_;
        return b;
    }
    std::uint32_t bits(int count) {
        std::uint32_t v = 0;
        for (int i = 0; i < count; ++i) v |= static_cast<std::uint32_t>(bit()) << i;
        return v;
    }
    bool overrun() const { return pos_ > n_ * 8; }

private:
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t pos_ = 0;
};

bool SmackerFile::read_byte_tree(Bits& b, ByteTree& t) {
    t.nodes.clear();
    if (!b.bit()) return true;  // absent: decodes to 0
    // Nodes in the order they come: an inner node's first child follows it, its second child's
    // index is kept (negated) once that child starts.
    auto read_node = [&](auto&& self, int depth) -> bool {
        if (depth > kMaxDepth || t.nodes.size() > 4096) return false;
        if (b.bit()) {
            const std::size_t at = t.nodes.size();
            t.nodes.push_back(0);
            if (!self(self, depth + 1)) return false;
            t.nodes[at] = -static_cast<int>(t.nodes.size());
            return self(self, depth + 1);
        }
        t.nodes.push_back(static_cast<int>(b.bits(8)));
        return true;
    };
    if (!read_node(read_node, 0)) return false;
    b.bit();  // the spare bit after the tree
    return !b.overrun();
}

int SmackerFile::decode_byte(Bits& b, const ByteTree& t) {
    if (t.nodes.empty()) return 0;
    std::size_t at = 0;
    while (t.nodes[at] < 0) at = b.bit() ? static_cast<std::size_t>(-t.nodes[at]) : at + 1;
    return t.nodes[at];
}

bool SmackerFile::read_big_tree(Bits& b, BigTree& t) {
    t.nodes.clear();
    t.present = b.bit() != 0;
    if (!t.present) return true;
    ByteTree lo, hi;
    if (!read_byte_tree(b, lo) || !read_byte_tree(b, hi)) return false;
    std::array<std::uint32_t, 3> esc{};
    for (auto& e : esc) e = b.bits(16);
    auto read_node = [&](auto&& self, int depth) -> bool {
        if (depth > kMaxDepth || t.nodes.size() > (1u << 20)) return false;
        if (b.bit()) {
            const std::size_t at = t.nodes.size();
            t.nodes.push_back(0);
            if (!self(self, depth + 1)) return false;
            t.nodes[at] = -static_cast<std::int32_t>(t.nodes.size());
            return self(self, depth + 1);
        }
        const std::uint32_t lo_v = static_cast<std::uint32_t>(decode_byte(b, lo));
        const std::uint32_t v = lo_v | static_cast<std::uint32_t>(decode_byte(b, hi)) << 8;
        std::int32_t leaf = static_cast<std::int32_t>(v);
        for (int k = 0; k < 3; ++k)
            if (v == esc[static_cast<std::size_t>(k)]) {
                leaf = 0x10000 + k;
                break;
            }
        t.nodes.push_back(leaf);
        return true;
    };
    if (!read_node(read_node, 0)) return false;
    b.bit();  // the spare bit after the tree
    return !b.overrun();
}

std::uint16_t SmackerFile::decode_big(Bits& b, BigTree& t) {
    if (!t.present || t.nodes.empty()) return 0;
    std::size_t at = 0;
    while (t.nodes[at] < 0) at = b.bit() ? static_cast<std::size_t>(-t.nodes[at]) : at + 1;
    const std::int32_t leaf = t.nodes[at];
    const std::uint16_t v = leaf >= 0x10000 ? t.recent[static_cast<std::size_t>(leaf - 0x10000)] : static_cast<std::uint16_t>(leaf);
    if (v != t.recent[0]) {
        t.recent[2] = t.recent[1];
        t.recent[1] = t.recent[0];
        t.recent[0] = v;
    }
    return v;
}

bool SmackerFile::open(std::vector<std::uint8_t> bytes, std::string& error) {
    data_ = std::move(bytes);
    if (data_.size() < 104 || std::memcmp(data_.data(), "SMK2", 4) != 0) {
        error = data_.size() >= 4 && std::memcmp(data_.data(), "SMK4", 4) == 0 ? "version 4 Smacker files are not supported" : "not a Smacker file";
        return false;
    }
    const std::uint8_t* h = data_.data();
    width_ = static_cast<int>(le32(h + 4));
    height_ = static_cast<int>(le32(h + 8));
    frames_ = static_cast<int>(le32(h + 12));
    rate_ = static_cast<std::int32_t>(le32(h + 16));
    flags_ = le32(h + 20);
    const std::uint32_t trees = le32(h + 52);
    for (int k = 0; k < 7; ++k) track_[static_cast<std::size_t>(k)] = le32(h + 72 + 4 * k);
    if (width_ <= 0 || height_ <= 0 || width_ > 4096 || height_ > 4096 || frames_ <= 0 || frames_ > 1000000) {
        error = "bad Smacker header";
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(frames_) + (flags_ & 1);
    std::size_t at = 104;
    if (data_.size() < at + n * 5 + trees) {
        error = "truncated Smacker file";
        return false;
    }
    frame_size_.resize(n);
    frame_type_.resize(n);
    for (std::size_t i = 0; i < n; ++i) frame_size_[i] = le32(h + at + 4 * i);
    at += 4 * n;
    for (std::size_t i = 0; i < n; ++i) frame_type_[i] = h[at + i];
    at += n;
    Bits b(h + at, trees);
    if (!read_big_tree(b, map_) || !read_big_tree(b, colour_) || !read_big_tree(b, full_) || !read_big_tree(b, type_)) {
        error = "damaged Smacker trees";
        return false;
    }
    at += trees;
    frame_offset_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        frame_offset_[i] = at;
        at += frame_size_[i] & ~3u;
    }
    pixels_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0);
    palette_.fill(0);
    frame_ = 0;
    return true;
}

double SmackerFile::frame_ms() const {
    if (rate_ > 0) return rate_;
    if (rate_ < 0) return -rate_ / 100.0;
    return 100.0;
}

bool SmackerFile::audio_present(int track) const { return track >= 0 && track < 7 && (track_[static_cast<std::size_t>(track)] & 0x40000000u) != 0; }
int SmackerFile::audio_rate(int track) const { return static_cast<int>(track_[static_cast<std::size_t>(track)] & 0xFFFFFFu); }
int SmackerFile::audio_channels(int track) const { return (track_[static_cast<std::size_t>(track)] & 0x10000000u) ? 2 : 1; }
int SmackerFile::audio_bits(int track) const { return (track_[static_cast<std::size_t>(track)] & 0x20000000u) ? 16 : 8; }

void SmackerFile::apply_palette(const std::uint8_t* p, std::size_t n) {
    const std::array<std::uint8_t, 768> old = palette_;
    std::size_t i = 0, k = 0;
    auto widen = [](std::uint8_t c) { return static_cast<std::uint8_t>((c & 0x3F) << 2 | (c & 0x3F) >> 4); };
    while (i < 256 && k < n) {
        const std::uint8_t c = p[k++];
        if (c & 0x80) {
            i += static_cast<std::size_t>(c & 0x7F) + 1;  // kept as they are
        } else if (c & 0x40) {
            if (k >= n) break;
            std::size_t src = p[k++];
            for (std::size_t m = static_cast<std::size_t>(c & 0x3F) + 1; m > 0 && i < 256 && src < 256; --m, ++i, ++src)
                std::memcpy(&palette_[i * 3], &old[src * 3], 3);
        } else {
            if (k + 2 > n) break;
            palette_[i * 3] = widen(c);
            palette_[i * 3 + 1] = widen(p[k]);
            palette_[i * 3 + 2] = widen(p[k + 1]);
            k += 2;
            ++i;
        }
    }
}

bool SmackerFile::decode_picture(const std::uint8_t* p, std::size_t n) {
    Bits b(p, n);
    map_.reset();
    colour_.reset();
    full_.reset();
    type_.reset();
    const int bw = width_ / 4, bh = height_ / 4;
    const int blocks = bw * bh;
    const std::size_t stride = static_cast<std::size_t>(width_);
    int blk = 0;
    while (blk < blocks) {
        const std::uint16_t type = decode_big(b, type_);
        int run = run_length((type >> 2) & 0x3F);
        const int kind = type & 3;
        for (; run > 0 && blk < blocks; --run, ++blk) {
            std::uint8_t* out = &pixels_[static_cast<std::size_t>(blk / bw) * 4 * stride + static_cast<std::size_t>(blk % bw) * 4];
            switch (kind) {
            case 0: {  // two colours and a pattern
                const std::uint16_t clr = decode_big(b, colour_);
                std::uint16_t map = decode_big(b, map_);
                const std::uint8_t hi = static_cast<std::uint8_t>(clr >> 8), lo = static_cast<std::uint8_t>(clr);
                for (int y = 0; y < 4; ++y, out += stride)
                    for (int x = 0; x < 4; ++x, map >>= 1) out[x] = (map & 1) ? hi : lo;
                break;
            }
            case 1:  // every pixel: per row the second pair, then the first
                for (int y = 0; y < 4; ++y, out += stride) {
                    std::uint16_t v = decode_big(b, full_);
                    out[2] = static_cast<std::uint8_t>(v);
                    out[3] = static_cast<std::uint8_t>(v >> 8);
                    v = decode_big(b, full_);
                    out[0] = static_cast<std::uint8_t>(v);
                    out[1] = static_cast<std::uint8_t>(v >> 8);
                }
                break;
            case 2:  // unchanged
                break;
            default:  // one colour
                for (int y = 0; y < 4; ++y, out += stride) std::memset(out, type >> 8, 4);
                break;
            }
        }
    }
    return true;
}

bool SmackerFile::unpack_audio(const std::uint8_t* p, std::size_t n, std::vector<std::uint8_t>& out) {
    out.clear();
    if (n < 4) return false;
    const std::uint32_t size = le32(p);
    if (size > (64u << 20)) return false;
    Bits b(p + 4, n - 4);
    if (!b.bit()) return true;  // no data
    const int stereo = b.bit();
    const int wide = b.bit();
    std::array<ByteTree, 4> trees;
    for (int t = 0; t < (1 << (stereo + wide)); ++t)
        if (!read_byte_tree(b, trees[static_cast<std::size_t>(t)])) return false;
    out.resize(size);
    if (wide) {
        std::array<std::int16_t, 2> last{};
        for (int c = stereo; c >= 0; --c) {
            const std::uint32_t v = b.bits(16);
            last[static_cast<std::size_t>(c)] = static_cast<std::int16_t>(((v & 0xFF) << 8) | (v >> 8));
        }
        const std::size_t samples = size / 2;
        std::size_t i = 0;
        for (; i <= static_cast<std::size_t>(stereo) && i < samples; ++i) {
            out[2 * i] = static_cast<std::uint8_t>(last[i]);
            out[2 * i + 1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(last[i]) >> 8);
        }
        for (; i < samples; ++i) {
            const std::size_t c = (i & static_cast<std::size_t>(stereo)) ? 1 : 0;
            const int lo = decode_byte(b, trees[2 * c]);
            const int hi = decode_byte(b, trees[2 * c + 1]);
            last[c] = static_cast<std::int16_t>(static_cast<std::uint16_t>(last[c]) + static_cast<std::uint16_t>(lo | hi << 8));
            out[2 * i] = static_cast<std::uint8_t>(last[c]);
            out[2 * i + 1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(last[c]) >> 8);
        }
    } else {
        std::array<std::uint8_t, 2> last{};
        for (int c = stereo; c >= 0; --c) last[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(b.bits(8));
        std::size_t i = 0;
        for (; i <= static_cast<std::size_t>(stereo) && i < size; ++i) out[i] = last[i];
        for (; i < size; ++i) {
            const std::size_t c = (i & static_cast<std::size_t>(stereo)) ? 1 : 0;
            last[c] = static_cast<std::uint8_t>(last[c] + decode_byte(b, trees[c]));
            out[i] = last[c];
        }
    }
    return true;
}

bool SmackerFile::next_frame() {
    if (frame_ >= frames_) return false;
    const std::size_t f = static_cast<std::size_t>(frame_++);
    const std::size_t start = frame_offset_[f], size = frame_size_[f] & ~3u;
    if (start + size > data_.size()) return false;
    const std::uint8_t* p = data_.data() + start;
    const std::uint8_t* end = p + size;
    palette_changed_ = false;
    for (auto& a : audio_) a.clear();
    if (frame_type_[f] & 1) {
        const std::size_t len = static_cast<std::size_t>(p[0]) * 4;
        if (len == 0 || p + len > end) return false;
        apply_palette(p + 1, len - 1);
        palette_changed_ = true;
        p += len;
    }
    for (int t = 0; t < 7; ++t) {
        if (!(frame_type_[f] & (2 << t))) continue;
        if (p + 4 > end) return false;
        const std::size_t len = le32(p);
        if (len < 4 || p + len > end) return false;
        const std::uint32_t desc = track_[static_cast<std::size_t>(t)];
        if (desc & 0x80000000u) unpack_audio(p + 4, len - 4, audio_[static_cast<std::size_t>(t)]);
        else audio_[static_cast<std::size_t>(t)].assign(p + 4, p + len);
        p += len;
    }
    return decode_picture(p, static_cast<std::size_t>(end - p));
}

void SmackerFile::show(std::uint8_t* dst, int pitch, int rows, int top) const {
    const std::size_t w = static_cast<std::size_t>(std::min(width_, pitch));
    for (int y = 0; y < height_; ++y) {
        const std::uint8_t* src = &pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_)];
        const int r0 = (flags_ & 6) ? top + 2 * y : top + y;
        if (r0 >= 0 && r0 < rows) std::memcpy(dst + static_cast<std::size_t>(r0) * static_cast<std::size_t>(pitch), src, w);
        if ((flags_ & 4) && r0 + 1 >= 0 && r0 + 1 < rows) std::memcpy(dst + static_cast<std::size_t>(r0 + 1) * static_cast<std::size_t>(pitch), src, w);
    }
}

bool SmackerFile::decode_track(int track, std::vector<std::int16_t>& pcm) const {
    pcm.clear();
    if (!audio_present(track)) return false;
    const bool wide = audio_bits(track) == 16;
    const std::uint32_t desc = track_[static_cast<std::size_t>(track)];
    std::vector<std::uint8_t> raw;
    for (int i = 0; i < frames_; ++i) {
        const std::size_t f = static_cast<std::size_t>(i);
        const std::size_t start = frame_offset_[f], size = frame_size_[f] & ~3u;
        if (start + size > data_.size()) break;
        const std::uint8_t* p = data_.data() + start;
        const std::uint8_t* end = p + size;
        if (frame_type_[f] & 1) p += static_cast<std::size_t>(p[0]) * 4;
        for (int t = 0; t < 7 && p + 4 <= end; ++t) {
            if (!(frame_type_[f] & (2 << t))) continue;
            const std::size_t len = le32(p);
            if (len < 4 || p + len > end) break;
            if (t == track) {
                if (desc & 0x80000000u) unpack_audio(p + 4, len - 4, raw);
                else raw.assign(p + 4, p + len);
                if (wide)
                    for (std::size_t k = 0; k + 1 < raw.size(); k += 2)
                        pcm.push_back(static_cast<std::int16_t>(raw[k] | raw[k + 1] << 8));
                else
                    for (std::uint8_t v : raw) pcm.push_back(static_cast<std::int16_t>((v - 128) * 256));
            }
            p += len;
        }
    }
    return true;
}

}  // namespace client
