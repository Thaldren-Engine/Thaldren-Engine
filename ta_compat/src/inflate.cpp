#include "ta/inflate.h"

#include <array>

namespace ta {
namespace {

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> in) : in_(in) {}

    bool bits(int n, std::uint32_t& v) {
        while (count_ < n) {
            if (pos_ >= in_.size()) return false;
            buf_ |= static_cast<std::uint32_t>(in_[pos_++]) << count_;
            count_ += 8;
        }
        v = buf_ & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
        buf_ >>= n;
        count_ -= n;
        return true;
    }

    void align_to_byte() {
        buf_ = 0;
        count_ = 0;
    }

    bool read_byte(std::uint8_t& b) {
        if (pos_ >= in_.size()) return false;
        b = in_[pos_++];
        return true;
    }

    std::size_t pos() const { return pos_; }

private:
    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
    std::uint32_t buf_ = 0;
    int count_ = 0;
};

// Canonical Huffman decoding table (counts per length + sorted symbols).
struct Huffman {
    std::array<std::uint16_t, 16> count{};
    std::array<std::uint16_t, 320> symbol{};

    bool build(const std::uint8_t* lengths, int n) {
        count.fill(0);
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        std::array<std::uint16_t, 16> offs{};
        for (int i = 1; i < 16; ++i) offs[i] = static_cast<std::uint16_t>(offs[i - 1] + count[i - 1]);
        for (int i = 0; i < n; ++i) {
            if (lengths[i]) symbol[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
        }
        return true;
    }

    bool decode(BitReader& br, int& sym) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            std::uint32_t b = 0;
            if (!br.bits(1, b)) return false;
            code |= static_cast<int>(b);
            const int c = count[len];
            if (code - c < first) {
                sym = symbol[index + (code - first)];
                return true;
            }
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }
};

constexpr std::uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

InflateResult inflate_block(BitReader& br, std::vector<std::uint8_t>& out, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int sym = 0;
        if (!lit.decode(br, sym)) return InflateResult::Truncated;
        if (sym < 256) {
            out.push_back(static_cast<std::uint8_t>(sym));
        } else if (sym == 256) {
            return InflateResult::Ok;
        } else {
            sym -= 257;
            if (sym >= 29) return InflateResult::BadData;
            std::uint32_t extra = 0;
            if (!br.bits(kLenExtra[sym], extra)) return InflateResult::Truncated;
            const std::size_t len = kLenBase[sym] + extra;
            int dsym = 0;
            if (!dist.decode(br, dsym)) return InflateResult::Truncated;
            if (dsym >= 30) return InflateResult::BadData;
            if (!br.bits(kDistExtra[dsym], extra)) return InflateResult::Truncated;
            const std::size_t d = kDistBase[dsym] + extra;
            if (d > out.size()) return InflateResult::BadData;
            const std::size_t from = out.size() - d;
            for (std::size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
    }
}

} // namespace

InflateResult inflate_raw(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t& consumed) {
    BitReader br(in);
    std::uint32_t final_block = 0;
    do {
        std::uint32_t type = 0;
        if (!br.bits(1, final_block) || !br.bits(2, type)) return InflateResult::Truncated;
        if (type == 0) {
            br.align_to_byte();
            std::uint8_t b[4];
            for (auto& x : b)
                if (!br.read_byte(x)) return InflateResult::Truncated;
            const std::uint16_t len = static_cast<std::uint16_t>(b[0] | (b[1] << 8));
            const std::uint16_t nlen = static_cast<std::uint16_t>(b[2] | (b[3] << 8));
            if (len != static_cast<std::uint16_t>(~nlen)) return InflateResult::BadData;
            for (std::uint16_t i = 0; i < len; ++i) {
                std::uint8_t x = 0;
                if (!br.read_byte(x)) return InflateResult::Truncated;
                out.push_back(x);
            }
        } else if (type == 1) {
            std::uint8_t lengths[320];
            for (int i = 0; i < 144; ++i) lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lengths[i] = 8;
            Huffman lit, dist;
            lit.build(lengths, 288);
            for (int i = 0; i < 30; ++i) lengths[i] = 5;
            dist.build(lengths, 30);
            const auto r = inflate_block(br, out, lit, dist);
            if (r != InflateResult::Ok) return r;
        } else if (type == 2) {
            std::uint32_t hlit = 0, hdist = 0, hclen = 0;
            if (!br.bits(5, hlit) || !br.bits(5, hdist) || !br.bits(4, hclen)) return InflateResult::Truncated;
            hlit += 257;
            hdist += 1;
            hclen += 4;
            static constexpr std::uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::uint8_t cl[19] = {};
            for (std::uint32_t i = 0; i < hclen; ++i) {
                std::uint32_t v = 0;
                if (!br.bits(3, v)) return InflateResult::Truncated;
                cl[kOrder[i]] = static_cast<std::uint8_t>(v);
            }
            Huffman clh;
            clh.build(cl, 19);
            std::uint8_t lengths[320] = {};
            std::uint32_t n = 0;
            while (n < hlit + hdist) {
                int sym = 0;
                if (!clh.decode(br, sym)) return InflateResult::Truncated;
                if (sym < 16) {
                    lengths[n++] = static_cast<std::uint8_t>(sym);
                } else {
                    std::uint32_t rep = 0, val = 0;
                    if (sym == 16) {
                        if (n == 0) return InflateResult::BadData;
                        val = lengths[n - 1];
                        if (!br.bits(2, rep)) return InflateResult::Truncated;
                        rep += 3;
                    } else if (sym == 17) {
                        if (!br.bits(3, rep)) return InflateResult::Truncated;
                        rep += 3;
                    } else {
                        if (!br.bits(7, rep)) return InflateResult::Truncated;
                        rep += 11;
                    }
                    if (n + rep > hlit + hdist) return InflateResult::BadData;
                    while (rep--) lengths[n++] = static_cast<std::uint8_t>(val);
                }
            }
            Huffman lit, dist;
            lit.build(lengths, static_cast<int>(hlit));
            dist.build(lengths + hlit, static_cast<int>(hdist));
            const auto r = inflate_block(br, out, lit, dist);
            if (r != InflateResult::Ok) return r;
        } else {
            return InflateResult::BadData;
        }
    } while (!final_block);
    consumed = br.pos();
    return InflateResult::Ok;
}

InflateResult zlib_decompress(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out) {
    if (in.size() < 6) return InflateResult::Truncated;
    const std::uint8_t cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return InflateResult::BadHeader;
    const std::size_t start = out.size();
    std::size_t consumed = 0;
    const auto r = inflate_raw(in.subspan(2), out, consumed);
    if (r != InflateResult::Ok) return r;
    const std::size_t at = 2 + consumed;
    if (at + 4 > in.size()) return InflateResult::Truncated;
    std::uint32_t a = 1, b = 0;
    for (std::size_t i = start; i < out.size(); ++i) {
        a = (a + out[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    const std::uint32_t want = (static_cast<std::uint32_t>(in[at]) << 24) | (static_cast<std::uint32_t>(in[at + 1]) << 16) |
                               (static_cast<std::uint32_t>(in[at + 2]) << 8) | in[at + 3];
    return ((b << 16) | a) == want ? InflateResult::Ok : InflateResult::BadChecksum;
}

} // namespace ta
