#include "sim/bitstream.h"

namespace sim {
namespace {
std::uint32_t mask(int bits) { return bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u); }
}  // namespace

void BitWriter::put(std::uint32_t value, int bits) {
    if (bits + bit_ < 32) {
        words_[word_] |= (value & mask(bits)) << bit_;
        bit_ += bits;
        return;
    }
    if (bit_ != 0) {
        const int low = 32 - bit_;
        words_[word_] |= (value & mask(low)) << bit_;
        ++word_;
        words_.resize(word_ + 1, 0u);
        bit_ = bits - low;
        words_[word_] = (value >> low) & mask(bit_);
        return;
    }
    // A whole word at a word boundary is stored as given.
    words_[word_] = value;
    ++word_;
    words_.resize(word_ + 1, 0u);
    words_[word_] = 0;
}

void BitWriter::put_bit(bool on) { put(on ? 1u : 0u, 1); }

void BitWriter::set_byte(std::size_t at, std::uint8_t v) {
    const std::size_t w = at / 4;
    const int shift = static_cast<int>(at % 4) * 8;
    if (w >= words_.size()) words_.resize(w + 1, 0u);
    words_[w] = (words_[w] & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(v) << shift);
}

std::vector<std::uint8_t> BitWriter::bytes() const {
    std::vector<std::uint8_t> out(size());
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint8_t>(words_[i / 4] >> ((i % 4) * 8));
    return out;
}

BitReader::BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

std::uint32_t BitReader::word(std::size_t i) const {
    std::uint32_t v = 0;
    for (std::size_t b = 0; b < 4; ++b) {
        const std::size_t at = i * 4 + b;
        if (at < size_) v |= static_cast<std::uint32_t>(data_[at]) << (8 * b);
    }
    return v;
}

std::uint32_t BitReader::get(int bits) {
    if (bits + bit_ < 32) {
        const std::uint32_t v = (word(word_) >> bit_) & mask(bits);
        bit_ += bits;
        return v;
    }
    if (bit_ == 0) return word(word_++);
    const int low = 32 - bit_;
    const std::uint32_t lo = (word(word_) >> bit_) & mask(low);
    bit_ = bits - low;
    ++word_;
    return lo | ((word(word_) & mask(bit_)) << low);
}

std::int32_t BitReader::get_signed(int bits) {
    std::uint32_t v = get(bits);
    if (bits < 32 && (v & (1u << (bits - 1)))) v |= ~mask(bits);
    return static_cast<std::int32_t>(v);
}

} // namespace sim
