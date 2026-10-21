// Sound files as the original's loader reads them (spec: docs/RE_MEMORY.md, "Client",
// "Sound engine"): a RIFF WAVE file by its format chunk's rate, sample size and channel count
// (the encoding field is not looked at) and its data chunk; a "DIGI" file (rate at byte 0x16,
// 11000 meaning 11025, 8-bit mono data from byte 0x28); anything else is raw 8-bit mono at
// 11025 Hz. 8-bit samples are unsigned.

#include <algorithm>
#include <cstring>

#include "client/audio.h"

namespace client {

namespace {

std::uint32_t u32_at(const std::vector<std::uint8_t>& f, std::size_t at) {
    if (at + 4 > f.size()) return 0;
    return static_cast<std::uint32_t>(f[at]) | static_cast<std::uint32_t>(f[at + 1]) << 8 | static_cast<std::uint32_t>(f[at + 2]) << 16 |
           static_cast<std::uint32_t>(f[at + 3]) << 24;
}
std::uint16_t u16_at(const std::vector<std::uint8_t>& f, std::size_t at) {
    if (at + 2 > f.size()) return 0;
    return static_cast<std::uint16_t>(f[at] | f[at + 1] << 8);
}
bool tag_at(const std::vector<std::uint8_t>& f, std::size_t at, const char* tag) { return at + 4 <= f.size() && std::memcmp(&f[at], tag, 4) == 0; }

void fill(Sample& out, const std::vector<std::uint8_t>& f, std::size_t at, std::size_t bytes, int rate, int bits, int channels) {
    out.rate = rate;
    out.channels = channels < 1 ? 1 : channels;
    out.pcm.clear();
    if (at > f.size()) return;
    bytes = std::min(bytes, f.size() - at);
    if (bits == 16) {
        out.pcm.resize(bytes / 2);
        for (std::size_t i = 0; i < out.pcm.size(); ++i) out.pcm[i] = static_cast<std::int16_t>(u16_at(f, at + i * 2));
    } else {
        out.pcm.resize(bytes);
        for (std::size_t i = 0; i < bytes; ++i) out.pcm[i] = static_cast<std::int16_t>((static_cast<int>(f[at + i]) - 128) * 256);
    }
    // Whole frames only.
    out.pcm.resize(out.pcm.size() / static_cast<std::size_t>(out.channels) * static_cast<std::size_t>(out.channels));
}

}  // namespace

bool decode_wav(const std::vector<std::uint8_t>& f, Sample& out) {
    if (f.empty()) return false;
    if (tag_at(f, 0, "RIFF") && tag_at(f, 8, "WAVE")) {
        // The chunks after the 12-byte header, each found by walking from the first.
        const std::size_t end = static_cast<std::size_t>(u32_at(f, 4)) + 8;
        auto find = [&](const char* tag, std::size_t& at, std::uint32_t& size) {
            std::size_t p = 12;
            while (p + 8 <= f.size() && p < end) {
                const std::uint32_t n = u32_at(f, p + 4);
                if (tag_at(f, p, tag)) {
                    at = p + 8;
                    size = n;
                    return true;
                }
                p += 8 + n;
            }
            return false;
        };
        std::size_t fmt = 0, data = 0;
        std::uint32_t fmt_size = 0, data_size = 0;
        if (!find("fmt ", fmt, fmt_size) || fmt_size < 16 || !find("data", data, data_size) || data_size < 1) return false;
        const int channels = u16_at(f, fmt + 2);
        const int rate = static_cast<int>(u32_at(f, fmt + 4));
        const int bits = u16_at(f, fmt + 14);
        fill(out, f, data, data_size, rate, bits, channels);
        out.riff = true;
        return !out.pcm.empty();
    }
    if (tag_at(f, 0, "DIGI") && tag_at(f, 8, "HSHD") && tag_at(f, 0x20, "SDAT")) {
        int rate = u16_at(f, 0x16) | u16_at(f, 0x18) << 16;
        if (rate == 11000) rate = 11025;
        fill(out, f, 0x28, f.size() - 0x28, rate, 8, 1);
        out.riff = false;
        return !out.pcm.empty();
    }
    fill(out, f, 0, f.size(), 11025, 8, 1);
    out.riff = false;
    return !out.pcm.empty();
}

}  // namespace client
