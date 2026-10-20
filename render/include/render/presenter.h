#pragma once
// Output of finished frames, independent of any window system.

#include <cstdint>
#include <string>
#include <vector>

#include "render/draw_list.h"
#include "render/image.h"

namespace render {

// Shows an 8-bit frame with its palette (a window, a file, a test sink).
class Presenter {
public:
    virtual ~Presenter() = default;
    virtual bool present(const Image8& frame, const Palette& palette) = 0;
};

// Executes a draw list and returns the resulting 8-bit frame. The CPU
// reference backend and the GPU backend implement this.
class FrameRenderer {
public:
    virtual ~FrameRenderer() = default;
    virtual bool render(const DrawList& list, Image8& out) = 0;
};

// 8-bit run-length PCX (version 5, one 8-bit plane, the 256-colour palette after a 0x0C),
// laid out as the original's screenshot writer makes it: window 0,0..w-1,h-1, the resolution
// fields holding w and h, the first 16 colours in the header, each row coded on its own (a run
// of equal bytes, or one byte of 0xC0 and up, as (0xC0 | n, byte) pieces of at most 63).
std::vector<std::uint8_t> encode_pcx(const Image8& frame, const Palette& palette);
bool write_pcx(const std::string& path, const Image8& frame, const Palette& palette);

// 8-bit BMP with the palette (no compression).
bool write_bmp(const std::string& path, const Image8& frame, const Palette& palette);

// Writes each presented frame to <prefix>NNNN.bmp.
class BmpPresenter : public Presenter {
public:
    explicit BmpPresenter(std::string prefix) : prefix_(std::move(prefix)) {}
    bool present(const Image8& frame, const Palette& palette) override;
    int frames_written() const { return count_; }

private:
    std::string prefix_;
    int count_ = 0;
};

} // namespace render
