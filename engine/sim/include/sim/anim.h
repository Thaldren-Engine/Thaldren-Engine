#pragma once
// Frame-sequence animations as the simulation sees them: how many frames a
// sequence has, how long each one shows, whether it loops, and the size and
// hotspot of each frame (burning features place their smoke from it). The
// pixels belong to the presentation.
//
// The original keeps a sequence's address in its frame cursors, and those
// cursors share memory with other state (see sim/features.h), so a sequence
// is identified by an address-like number here too: whatever the loader
// assigns (a stand-in, or the real address in a differential harness).

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sim {

struct AnimFrame {
    std::uint16_t duration = 0;  // steps (low 16 bits of the stored value)
    std::uint16_t width = 0, height = 0;
    std::int16_t x_offset = 0, y_offset = 0;
};

struct AnimSequence {
    std::uint32_t address = 0;  // identity (nonzero)
    std::uint8_t loop = 0;      // nonzero: starts over after the last frame
    std::vector<AnimFrame> frames;
};

class AnimLibrary {
public:
    // Adds a sequence; returns its address (assigned when `seq.address` is 0).
    std::uint32_t add(AnimSequence seq);
    const AnimSequence* find(std::uint32_t address) const;
    AnimSequence* find(std::uint32_t address);
    const std::vector<AnimSequence>& sequences() const { return seqs_; }

    static constexpr std::uint32_t kFirstAddress = 0x00C00000u;  // stand-in addresses
private:
    std::vector<AnimSequence> seqs_;
    std::unordered_map<std::uint32_t, std::size_t> by_address_;
    std::uint32_t next_ = kFirstAddress;
};

// A frame cursor (12 bytes in the original): current frame, steps left on it,
// the loop byte copied from the sequence (plus three bytes nobody writes) and
// the sequence (0 = none / finished).
struct AnimCursor {
    std::uint16_t frame = 0;
    std::uint16_t countdown = 0;
    std::uint8_t loop = 0;
    std::uint8_t pad[3] = {0, 0, 0};
    std::uint32_t seq = 0;
};

// Starts `c` on frame `start` of `seq` (frame 0 when `start` is past the end).
// `seq` must exist (the original reads through a null sequence).
void anim_start(AnimCursor& c, const AnimLibrary& lib, std::uint32_t seq, int start = 0);
// One step: counts down the current frame, then moves to the next; at the end a
// looping sequence starts over and any other clears the sequence. False when
// the cursor had no sequence.
bool anim_advance(AnimCursor& c, const AnimLibrary& lib);
// The frame the cursor shows (nullptr without a sequence).
const AnimFrame* anim_frame(const AnimCursor& c, const AnimLibrary& lib);

} // namespace sim
