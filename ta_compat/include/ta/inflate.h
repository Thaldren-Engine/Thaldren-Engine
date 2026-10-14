#pragma once
// Minimal zlib (RFC 1950) / DEFLATE (RFC 1951) decoder.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ta {

enum class InflateResult { Ok, BadHeader, BadData, BadChecksum, Truncated };

// Decodes a complete zlib stream. Output is appended to `out`.
InflateResult zlib_decompress(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out);

// Decodes raw DEFLATE data starting at `in`; `consumed` receives the bytes used.
InflateResult inflate_raw(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t& consumed);

} // namespace ta
