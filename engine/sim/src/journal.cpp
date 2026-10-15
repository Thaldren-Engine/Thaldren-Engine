// The input journal of a battle (docs/RE_MEMORY.md, "Saved games").

#include "sim/journal.h"

#include "sim/battle.h"
#include "sim/lockstep.h"

namespace sim {

namespace {

constexpr std::uint32_t kMagic = 0x4C4E524Au;  // "JRNL"
constexpr std::uint32_t kVersion = 1;

void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) o.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}
bool get32(const std::vector<std::uint8_t>& in, std::size_t& at, std::uint32_t& v) {
    if (at + 4 > in.size()) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) v |= static_cast<std::uint32_t>(in[at++]) << (8 * k);
    return true;
}

}  // namespace

void Journal::encode(std::vector<std::uint8_t>& out) const {
    put32(out, kMagic);
    put32(out, kVersion);
    put32(out, static_cast<std::uint32_t>(commands.size()));
    for (const command::Timed& t : commands) {
        put32(out, t.tick);
        command::encode(t.command, out);
    }
    for (std::size_t k = 0; k < commands.size(); ++k) put32(out, k < command_frames.size() ? command_frames[k] : 0);
    put32(out, static_cast<std::uint32_t>(frame_ends.size()));
    for (std::uint32_t f : frame_ends) put32(out, f);
    put32(out, static_cast<std::uint32_t>(clock.size()));
    for (std::uint32_t c : clock) put32(out, c);
    put32(out, static_cast<std::uint32_t>(checksums.size()));
    for (const auto& [tick, sum] : checksums) {
        put32(out, tick);
        put32(out, sum);
    }
}

bool Journal::decode(const std::vector<std::uint8_t>& in) {
    std::size_t at = 0;
    std::uint32_t magic = 0, version = 0, n = 0;
    if (!get32(in, at, magic) || magic != kMagic || !get32(in, at, version) || version != kVersion) return false;
    *this = Journal{};
    if (!get32(in, at, n)) return false;
    for (std::uint32_t k = 0; k < n; ++k) {
        command::Timed t;
        if (!get32(in, at, t.tick) || !command::decode(in, at, t.command)) return false;
        commands.push_back(std::move(t));
    }
    for (std::uint32_t k = 0; k < n; ++k) {
        std::uint32_t f = 0;
        if (!get32(in, at, f)) return false;
        command_frames.push_back(f);
    }
    for (std::vector<std::uint32_t>* list : {&frame_ends, &clock}) {
        if (!get32(in, at, n)) return false;
        for (std::uint32_t k = 0; k < n; ++k) {
            std::uint32_t v = 0;
            if (!get32(in, at, v)) return false;
            list->push_back(v);
        }
    }
    if (!get32(in, at, n)) return false;
    for (std::uint32_t k = 0; k < n; ++k) {
        std::uint32_t tick = 0, sum = 0;
        if (!get32(in, at, tick) || !get32(in, at, sum)) return false;
        checksums[tick] = sum;
    }
    return true;
}

Journal Journal::until(std::uint32_t tick) const {
    // Everything recorded so far belongs to steps up to `tick` when the save is made at it;
    // commands queued for later steps stay with the save (they were given before it).
    Journal j = *this;
    for (auto it = j.checksums.begin(); it != j.checksums.end();)
        it = it->first > tick ? j.checksums.erase(it) : std::next(it);
    return j;
}

bool JournalRecorder::apply(Battle& b, const command::Command& c) {
    j_.commands.push_back(command::Timed{b.game_tick() + 1, c});
    j_.command_frames.push_back(static_cast<std::uint32_t>(j_.frame_ends.size()));
    return command::apply(b, c);
}

void JournalRecorder::step(Battle& b, bool record_checksum) {
    b.step();
    if (record_checksum) j_.checksums[b.game_tick()] = state_checksum(b);
}

void JournalRecorder::end_frame(Battle& b) {
    j_.frame_ends.push_back(b.game_tick());
    b.end_frame();
}

bool JournalPlayer::run_to(Battle& b, std::uint32_t tick) {
    // Commands go in where they were given: due before the next step and after as many frame
    // tails as had run then.
    auto pending = [&](std::uint32_t due) {
        while (next_cmd_ < j_.commands.size() && j_.commands[next_cmd_].tick <= due &&
               (next_cmd_ >= j_.command_frames.size() || j_.command_frames[next_cmd_] <= next_frame_))
            command::apply(b, j_.commands[next_cmd_++].command);
    };
    auto frames_at = [&](std::uint32_t t) {
        while (next_frame_ < j_.frame_ends.size() && j_.frame_ends[next_frame_] == t) {
            pending(t + 1);
            ++next_frame_;
            b.end_frame();
        }
    };
    frames_at(b.game_tick());
    while (b.game_tick() < tick) {
        const std::uint32_t due = b.game_tick() + 1;
        pending(due);
        b.step();
        auto it = j_.checksums.find(b.game_tick());
        if (it != j_.checksums.end() && it->second != state_checksum(b)) {
            mismatch_tick = b.game_tick();
            return false;
        }
        frames_at(b.game_tick());
    }
    return true;
}

void JournalPlayer::apply_rest(Battle& b) {
    while (next_cmd_ < j_.commands.size()) command::apply(b, j_.commands[next_cmd_++].command);
}

std::uint32_t JournalPlayer::clock() { return next_clock_ < j_.clock.size() ? j_.clock[next_clock_++] : 0; }

}  // namespace sim
