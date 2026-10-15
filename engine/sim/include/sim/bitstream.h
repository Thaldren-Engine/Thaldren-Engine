#pragma once
// Bit-packed streams of the network unit state packets (see docs/RE_MEMORY.md,
// "Network and sync"). Bits fill 32-bit little-endian words from the lowest bit
// up; a value that does not fit in the current word continues in the next one.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sim {

class BitWriter {
public:
    BitWriter() : words_(1, 0u) {}
    // Appends the low `bits` bits of `value` (1..32).
    void put(std::uint32_t value, int bits);
    void put_bit(bool on);
    // Bytes in use: whole words written plus the started one rounded up to bytes.
    std::size_t size() const { return word_ * 4 + static_cast<std::size_t>((bit_ + 7) >> 3); }
    // Overwrites one byte of what was written (the packet's length field).
    void set_byte(std::size_t at, std::uint8_t v);
    std::vector<std::uint8_t> bytes() const;

private:
    std::vector<std::uint32_t> words_;
    std::size_t word_ = 0;
    int bit_ = 0;
};

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size);
    // Reads `bits` (1..32) bits; reads past the end give zero bits.
    std::uint32_t get(int bits);
    // The same, sign-extended from the top bit read.
    std::int32_t get_signed(int bits);
    bool get_bit() { return get(1) != 0; }

private:
    std::uint32_t word(std::size_t i) const;
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t word_ = 0;
    int bit_ = 0;
};

} // namespace sim
