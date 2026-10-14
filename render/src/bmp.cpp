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

bool BmpPresenter::present(const Image8& frame, const Palette& palette) {
    char name[32];
    std::snprintf(name, sizeof name, "%04d.bmp", count_++);
    return write_bmp(prefix_ + name, frame, palette);
}

} // namespace render
