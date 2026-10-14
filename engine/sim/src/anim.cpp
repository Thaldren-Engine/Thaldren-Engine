#include "sim/anim.h"

namespace sim {

std::uint32_t AnimLibrary::add(AnimSequence seq) {
    if (seq.address == 0) {
        seq.address = next_;
        next_ += 0x100;
    }
    const std::uint32_t a = seq.address;
    by_address_[a] = seqs_.size();
    seqs_.push_back(std::move(seq));
    return a;
}

const AnimSequence* AnimLibrary::find(std::uint32_t address) const {
    auto it = by_address_.find(address);
    return it == by_address_.end() ? nullptr : &seqs_[it->second];
}

AnimSequence* AnimLibrary::find(std::uint32_t address) {
    auto it = by_address_.find(address);
    return it == by_address_.end() ? nullptr : &seqs_[it->second];
}

namespace {
std::uint16_t duration_of(const AnimSequence* s, std::uint16_t frame) {
    // A frame past the table reads whatever follows it in the original; 0 here.
    return s && frame < s->frames.size() ? s->frames[frame].duration : 0;
}
}  // namespace

void anim_start(AnimCursor& c, const AnimLibrary& lib, std::uint32_t seq, int start) {
    const AnimSequence* s = lib.find(seq);
    const int frames = s ? static_cast<int>(s->frames.size()) : 0;
    c.frame = static_cast<std::uint16_t>(frames <= start ? 0 : start);
    c.seq = seq;
    c.countdown = duration_of(s, c.frame);
    c.loop = s ? s->loop : 0;
}

bool anim_advance(AnimCursor& c, const AnimLibrary& lib) {
    if (c.seq == 0) return false;
    if (c.countdown < 2) {
        const AnimSequence* s = lib.find(c.seq);
        c.frame = static_cast<std::uint16_t>(c.frame + 1);
        if (!s || c.frame >= s->frames.size()) {
            if (c.loop == 0) {
                c.seq = 0;
                return true;
            }
            c.frame = 0;
        }
        c.countdown = duration_of(s, c.frame);
        return true;
    }
    c.countdown = static_cast<std::uint16_t>(c.countdown - 1);
    return true;
}

const AnimFrame* anim_frame(const AnimCursor& c, const AnimLibrary& lib) {
    const AnimSequence* s = lib.find(c.seq);
    if (!s || c.frame >= s->frames.size()) return nullptr;
    return &s->frames[c.frame];
}

} // namespace sim
