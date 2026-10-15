#pragma once
// Wire format of the game's network messages (see docs/RE_MEMORY.md, "Network and sync"):
//
//   message  = kind byte (3 plain, 4 compressed), 16-bit checksum, body
//   body     = the frame, or the frame compressed with the dictionary coder below
//
// The checksum is the 16-bit sum of the body bytes from offset 3 up to (not including) the
// last three bytes of the whole message, taken after each of those bytes was XORed with the
// low byte of its offset. The last three bytes are neither scrambled nor summed.
//
// The dictionary coder: a 4096-byte window (zeroed, writing starts at position 1), at most 17
// bytes per match, matches of 2 bytes or more. A flag byte announces eight items, lowest bit
// first: 0 = one literal byte, 1 = a 16-bit little-endian word (window position << 4 |
// length - 2). A word with position 0 ends the data. The encoder finds matches with a binary
// search tree over the window whose comparisons treat bytes as signed.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace net {

// Encoder. After the end marker the original writes the item slot that follows it from a
// buffer on its stack: either a literal left there by an earlier group of the same message,
// or memory nothing wrote (`stale_byte` stands in for it, and `trailing_stale` tells which).
// The receiver never reads that byte.
struct CompressResult {
    std::vector<std::uint8_t> data;
    bool trailing_stale = false;
};
CompressResult compress(const std::uint8_t* data, std::size_t size, std::uint8_t stale_byte = 0);
// Decoder; stops at the end marker or when the input runs out.
std::vector<std::uint8_t> decompress(const std::uint8_t* data, std::size_t size);

constexpr std::uint8_t kPlainMessage = 3, kCompressedMessage = 4;

// Builds a message from a frame. Bodies shorter than 13 bytes are never compressed; longer
// ones are when compression is on and the result (with its 3-byte head) is shorter than the
// frame alone.
std::vector<std::uint8_t> wrap(const std::uint8_t* frame, std::size_t size, bool compression, std::uint8_t stale_byte = 0);

enum class UnwrapResult { Ok, NotWrapped, TooShort, BadChecksum };
// Checks and opens a message. Data whose first byte is neither 3 nor 4 is passed on as it is
// (NotWrapped, `out` = the data).
UnwrapResult unwrap(const std::uint8_t* message, std::size_t size, std::vector<std::uint8_t>& out);

} // namespace net
