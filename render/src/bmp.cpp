#include <cstdio>
#include <vector>

#include "render/presenter.h"

namespace render {
namespace {

void put16(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    put16(b, v & 0xFFFF);
    put16(b, v >> 16);
}

} // namespace

bool write_bmp(const std::string& path, const Image8& frame, const Palette& palette) {
    if (frame.width <= 0 || frame.height <= 0) return false;
    const std::uint32_t stride = (static_cast<std::uint32_t>(frame.width) + 3u) & ~3u;
    const std::uint32_t data_size = stride * static_cast<std::uint32_t>(frame.height);
    const std::uint32_t offset = 14 + 40 + 256 * 4;
    std::vector<std::uint8_t> b;
    b.reserve(offset + data_size);
    b.push_back('B');
    b.push_back('M');
    put32(b, offset + data_size);
    put32(b, 0);
    put32(b, offset);
    put32(b, 40);
    put32(b, static_cast<std::uint32_t>(frame.width));
    put32(b, static_cast<std::uint32_t>(frame.height));  // bottom-up
    put16(b, 1);
    put16(b, 8);
    put32(b, 0);  // BI_RGB
    put32(b, data_size);
    put32(b, 2835);
    put32(b, 2835);
    put32(b, 256);
    put32(b, 0);
    for (const Rgb& c : palette) {
        b.push_back(c.b);
        b.push_back(c.g);
        b.push_back(c.r);
        b.push_back(0);
    }
    for (int y = frame.height - 1; y >= 0; --y) {
        const std::uint8_t* r = frame.row(y);
        b.insert(b.end(), r, r + frame.width);
        for (std::uint32_t k = static_cast<std::uint32_t>(frame.width); k < stride; ++k) b.push_back(0);
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    return std::fclose(f) == 0 && ok;
}

std::vector<std::uint8_t> encode_pcx(const Image8& frame, const Palette& palette) {
    std::vector<std::uint8_t> b(128, 0);
    std::vector<std::uint8_t> pal(768);
    for (std::size_t i = 0; i < 256; ++i) {
        pal[i * 3] = palette[i].r;
        pal[i * 3 + 1] = palette[i].g;
        pal[i * 3 + 2] = palette[i].b;
    }
    auto at16 = [&](std::size_t off, int v) {
        b[off] = static_cast<std::uint8_t>(v);
        b[off + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    b[0] = 0x0A;  // manufacturer
    b[1] = 5;     // version
    b[2] = 1;     // run-length coding
    b[3] = 8;     // bits per pixel
    at16(8, frame.width - 1);
    at16(10, frame.height - 1);
    at16(12, frame.width);   // (the resolution fields carry the size)
    at16(14, frame.height);
    for (std::size_t i = 0; i < 48; ++i) b[16 + i] = pal[i];
    b[65] = 1;  // planes
    at16(66, frame.width);  // bytes per line
    auto flush = [&](std::uint8_t v, int n) {
        if (n == 1 && (v & 0xC0) != 0xC0) {
            b.push_back(v);
            return;
        }
        while (n > 0) {
            const int k = n < 0x40 ? n : 0x3F;
            b.push_back(static_cast<std::uint8_t>(k | 0xC0));
            b.push_back(v);
            n -= k;
        }
    };
    for (int y = 0; y < frame.height; ++y) {
        const std::uint8_t* r = frame.row(y);
        std::uint8_t v = r[0];
        int n = 1;
        for (int x = 1; x < frame.width; ++x) {
            if (r[x] == v) {
                ++n;
                continue;
            }
            flush(v, n);
            v = r[x];
            n = 1;
        }
        flush(v, n);
    }
    b.push_back(0x0C);
    b.insert(b.end(), pal.begin(), pal.end());
    return b;
}

bool write_pcx(const std::string& path, const Image8& frame, const Palette& palette) {
    if (frame.width <= 0 || frame.height <= 0) return false;
    const std::vector<std::uint8_t> b = encode_pcx(frame, palette);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    return std::fclose(f) == 0 && ok;
}

bool BmpPresenter::present(const Image8& frame, const Palette& palette) {
    char name[32];
    std::snprintf(name, sizeof name, "%04d.bmp", count_++);
    return write_bmp(prefix_ + name, frame, palette);
}

} // namespace render
