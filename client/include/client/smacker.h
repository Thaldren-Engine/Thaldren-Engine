#pragma once
// Smacker video files (RAD Game Tools' 8-bit movie format, version 2 files): a decoder written
// from the format's description (docs/RE_MEMORY.md, "Movies"), not from any other decoder.
//
// The format in short:
//   * a 104-byte header: "SMK2", width, height, frame count, frame rate (> 0 milliseconds per
//     frame, < 0 hundredths of a millisecond, 0 means 10 frames a second), flags (1 an extra
//     "ring" frame, 2 lines shown on every other row, 4 lines shown twice), seven audio tracks'
//     largest unpacked sizes, the size of the tree block, four tree allocation sizes, seven
//     audio track descriptions (bit 31 compressed, 30 present, 29 16-bit, 28 stereo, low 24 bits
//     the sample rate), one spare word;
//   * a table of frame sizes (the low two bits are flags), a table of frame type bytes (bit 0
//     a palette record, bits 1..7 audio for tracks 0..6), the tree block, then the frames;
//   * every bit stream reads bits from the lowest bit of each byte upwards;
//   * the tree block holds four "big" code trees (block map, block colours, full blocks,
//     block types). A big tree is a flag bit; when set, two byte trees (low and high byte),
//     three 16-bit escape values and the tree itself, each node a bit (1 inner, 0 leaf), each
//     leaf the 16-bit value made of one code of each byte tree; a leaf equal to escape k stands
//     for the k-th most recent value. Each tree keeps three recent values, cleared at every
//     frame, updated whenever a decoded value differs from the most recent one. A byte tree is
//     a flag bit and, when set, nodes (1 inner, 0 leaf followed by 8 value bits) and one spare
//     bit; an absent tree decodes to 0 without reading;
//   * a frame: the palette record (when flagged: a length byte in units of four bytes, then
//     runs over the 256 entries — 1xxxxxxx keeps x + 1 entries, 01xxxxxx copies x + 1 entries
//     of the previous palette from the index in the next byte, else three 6-bit components,
//     widened to 8 bits as (c << 2) | (c >> 4)), each flagged audio track (a 32-bit length
//     counting itself, then its data), then the picture: 4x4 blocks in rows, each block-type
//     code giving a kind (low 2 bits), a run length (bits 2..7 through the table 1..59, 128,
//     256, 512, 1024, 2048) and, for a fill run, the colour (high byte). Kinds: 0 two-colour
//     blocks (a colour code — high byte for set bits, low byte for clear bits — and a 16-bit
//     pattern, one bit per pixel in row order), 1 full blocks (per row two codes: the second
//     pixel pair, then the first, each low byte first), 2 blocks unchanged, 3 filled blocks;
//   * compressed audio: the unpacked byte count, then a stream: a bit "data", a bit "stereo",
//     a bit "16-bit", one byte tree per channel (two for 16-bit: low byte, high byte), the
//     first sample of each channel stored whole (the last channel first; a 16-bit value's high
//     byte first), then per sample the code added to the channel's last value, wrapping.
// Presentation only.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace client {

class SmackerFile {
public:
    // Reads the header and the trees; false with `error` for a file it cannot play.
    bool open(std::vector<std::uint8_t> bytes, std::string& error);

    int width() const { return width_; }
    int height() const { return height_; }  // rows stored per frame
    // Rows shown: twice the stored rows when the lines are interlaced or doubled.
    int shown_height() const { return (flags_ & 6) ? height_ * 2 : height_; }
    bool interlaced() const { return (flags_ & 2) != 0; }
    bool doubled() const { return (flags_ & 4) != 0; }
    int frames() const { return frames_; }
    // Milliseconds per frame.
    double frame_ms() const;

    // Decodes the next frame; false after the last one (or on a damaged frame).
    bool next_frame();
    int frame_index() const { return frame_ - 1; }  // the frame just decoded
    const std::vector<std::uint8_t>& pixels() const { return pixels_; }  // width x height
    const std::array<std::uint8_t, 768>& palette() const { return palette_; }  // 8-bit RGB
    bool palette_changed() const { return palette_changed_; }  // the frame carried a palette

    // Copies the frame as a player shows it: at (0, top) of a buffer of `rows` rows of `pitch`
    // bytes; interlaced files fill every other row (the rows between are left as they are),
    // doubled files fill each row twice.
    void show(std::uint8_t* dst, int pitch, int rows, int top) const;

    // An audio track: present, its rate, channels and sample size.
    bool audio_present(int track) const;
    int audio_rate(int track) const;
    int audio_channels(int track) const;
    int audio_bits(int track) const;
    // The unpacked audio of the frame just decoded for a track (raw: 8-bit unsigned or 16-bit
    // little-endian signed, channels interleaved).
    const std::vector<std::uint8_t>& frame_audio(int track) const { return audio_[static_cast<std::size_t>(track)]; }
    // The whole track as 16-bit samples, channels interleaved (reads every frame's audio; the
    // picture is not decoded).
    bool decode_track(int track, std::vector<std::int16_t>& pcm) const;

private:
    struct ByteTree {
        // nodes: value >= 0 a leaf, else -(index of the second child); the first child follows.
        std::vector<int> nodes;
    };
    struct BigTree {
        bool present = false;
        std::vector<std::int32_t> nodes;  // leaf: value 0..0xFFFF, or 0x10000 + k for escape k
        std::array<std::uint16_t, 3> recent{};
        void reset() { recent = {0, 0, 0}; }
    };
    class Bits;
    static bool read_byte_tree(Bits& b, ByteTree& t);
    static int decode_byte(Bits& b, const ByteTree& t);
    static bool read_big_tree(Bits& b, BigTree& t);
    static std::uint16_t decode_big(Bits& b, BigTree& t);
    static bool unpack_audio(const std::uint8_t* p, std::size_t n, std::vector<std::uint8_t>& out);
    void apply_palette(const std::uint8_t* p, std::size_t n);
    bool decode_picture(const std::uint8_t* p, std::size_t n);

    std::vector<std::uint8_t> data_;
    int width_ = 0, height_ = 0, frames_ = 0, rate_ = 0;
    std::uint32_t flags_ = 0;
    std::array<std::uint32_t, 7> track_{};
    std::vector<std::uint32_t> frame_size_;
    std::vector<std::uint8_t> frame_type_;
    std::vector<std::size_t> frame_offset_;
    BigTree map_, colour_, full_, type_;
    int frame_ = 0;
    std::vector<std::uint8_t> pixels_;
    std::array<std::uint8_t, 768> palette_{};
    bool palette_changed_ = false;
    std::array<std::vector<std::uint8_t>, 7> audio_;
};

}  // namespace client
