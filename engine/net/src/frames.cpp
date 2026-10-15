#include "net/frames.h"

#include "net/codec.h"
#include "net/protocol.h"

namespace net {

NetTiming NetTiming::from_rate(int n) {
    NetTiming t;
    if (n < 0) {
        t.frame_timing = false;
        return t;
    }
    int ms = 200;
    if (n != 0) {
        if (n < 2) n = 2;
        if (n > 30) n = 30;
        ms = 1000 / n;
    }
    t.interval_ticks = static_cast<std::uint32_t>(ms * 30 + 999) / 1000u;
    return t;
}

FrameSender::Channel& FrameSender::channel(std::int32_t to) {
    if (to == kEverybody) return everybody_;
    for (Channel& c : direct_)
        if (c.to == to) return c;
    direct_.push_back(Channel{to});
    return direct_.back();
}

void FrameSender::queue(std::int32_t from, std::int32_t to, const std::uint8_t* packet, std::size_t size, std::uint32_t clock,
                        const SendFn& send) {
    if (!timing_.frame_timing) {
        bytes_sent_ += size;
        send(from, to, std::vector<std::uint8_t>(packet, packet + size));
        return;
    }
    Channel& c = channel(to);
    if (c.bytes > 0x429 || c.pending.size() == 0x400) {
        send_channel(c, clock, true, send);
        if (!c.pending.empty()) return;  // the packet is lost
    }
    c.pending.push_back(Pending{from, std::vector<std::uint8_t>(packet, packet + size)});
    c.bytes += size;
}

void FrameSender::send_channel(Channel& c, std::uint32_t clock, bool force, const SendFn& send) {
    if (!(c.deadline <= clock || force)) return;
    c.deadline = clock + timing_.interval_ticks;
    while (!c.pending.empty()) {
        const std::int32_t from = c.pending.front().from;
        std::vector<std::uint8_t> frame(4);
        put32(frame.data(), static_cast<std::uint32_t>(c.to == kEverybody ? c.sequence : -1));
        std::deque<Pending> rest;
        for (Pending& p : c.pending) {
            if (p.from == from) frame.insert(frame.end(), p.bytes.begin(), p.bytes.end());
            else rest.push_back(std::move(p));
        }
        c.pending.swap(rest);
        c.bytes = 0;
        std::vector<std::uint8_t> msg = wrap(frame.data(), frame.size(), timing_.compression);
        ++frames_sent_;
        bytes_sent_ += msg.size();
        send(from, c.to, std::move(msg));
        c.sequence -= 1;
        if (c.sequence > -2) c.sequence = -2;
    }
}

void FrameSender::flush(std::uint32_t clock, bool force, const SendFn& send) {
    if (!timing_.frame_timing) return;
    send_channel(everybody_, clock, force, send);
    for (Channel& c : direct_) send_channel(c, clock, force, send);
}

FrameReceiver::Sender* FrameReceiver::sender(std::int32_t id) {
    for (Sender& s : senders_)
        if (s.id == id) return &s;
    senders_.push_back(Sender{});
    senders_.back().id = id;
    return &senders_.back();
}

bool FrameReceiver::ready(const Item& it, std::uint32_t tick) {
    const std::int32_t ahead = static_cast<std::int32_t>(it.release - tick);
    return !(tick != 0 && ahead > 0 && ahead < 31);
}

std::size_t FrameReceiver::waiting() const {
    std::size_t n = 0;
    for (const Sender& s : senders_) n += s.queue.size();
    return n;
}

void FrameReceiver::split(Sender& s, const std::vector<std::uint8_t>& frame, std::uint32_t tick, bool spread) {
    // 0x463790. Count the packets the frame holds.
    std::int32_t remaining = static_cast<std::int32_t>(frame.size()) - 4;
    std::size_t at = 4;
    int count = 0;
    while (remaining > 0 && at < frame.size() && frame[at] > 1 && frame[at] < 0x2D) {
        const std::uint16_t size = packet_size_at(&frame[at], frame.size() - at);
        if (size == 0) {
            stuck_ = true;  // the original never leaves this loop
            break;
        }
        remaining -= size;
        if (remaining < 0) break;
        at += size;
        ++count;
    }
    if (count <= 0) return;
    int drop_states = count - 128;
    std::int32_t interval = static_cast<std::int32_t>(tick - s.base_tick);
    if (interval >= 31) interval = 30;
    if (interval < 1) interval = 1;
    std::uint32_t rate = 16;
    if (interval < count) rate = static_cast<std::uint32_t>((count << 4) / interval);
    std::uint32_t acc = 0, stamp = tick;
    int queued = 0;
    at = 4;
    for (int k = 0; k < count; ++k) {
        const std::uint16_t size = packet_size_at(&frame[at], frame.size() - at);
        if (frame[at] == packet::kUnitStates && drop_states > 0) {
            --drop_states;
            ++dropped_;
        } else {
            if (s.queue.size() > 0x1FF) {
                dropped_ += static_cast<std::uint64_t>(count - k);
                return;
            }
            s.queue.push_back(Item{stamp, std::vector<std::uint8_t>(frame.begin() + static_cast<std::ptrdiff_t>(at),
                                                                    frame.begin() + static_cast<std::ptrdiff_t>(at + size))});
            ++queued;
            if (spread && acc <= static_cast<std::uint32_t>(queued * 16)) {
                acc += rate;
                ++stamp;
            }
        }
        at += size;
    }
}

bool FrameReceiver::take_head(Sender& s, std::uint32_t tick, Delivered& out) {
    if (s.queue.empty() || !ready(s.queue.front(), tick)) return false;
    out.from = s.id;
    out.to = s.to;
    out.packet = std::move(s.queue.front().packet);
    s.queue.pop_front();
    return true;
}

bool FrameReceiver::next(std::uint32_t tick, bool frame_timing, const ReadFn& read, Delivered& out) {
    std::int32_t from = 0, to = 0;
    std::vector<std::uint8_t> msg;
    if (!frame_timing) {
        if (!read(from, to, msg)) return false;
        out.from = from;
        out.to = to;
        out.packet = std::move(msg);
        return true;
    }
    // 0x462F30: the first due packet of the senders' queues, in their order.
    for (Sender& s : senders_)
        if (take_head(s, tick, out)) return true;
    if (!current_.present) {
        if (replay_ != kNone && sender(replay_)->held.present) {
            // A held frame whose turn came: split without spreading.
            Sender& s = *sender(replay_);
            current_ = s.held;
            current_.spread = false;
            s.held = Frame{};
            replay_ = kNone;
        } else if (deferred_.present) {
            current_ = deferred_;
            current_.spread = true;
            deferred_ = Frame{};
            replay_ = kNone;
        } else {
            replay_ = kNone;
            if (!read(from, to, msg)) return false;
            std::vector<std::uint8_t> frame;
            const UnwrapResult r = unwrap(msg.data(), msg.size(), frame);
            if (r == UnwrapResult::NotWrapped) {
                out.from = from;
                out.to = to;
                out.packet = std::move(frame);
                return true;
            }
            if (r != UnwrapResult::Ok || frame.size() <= 4) {
                ++bad_;
                return false;
            }
            Sender& s = *sender(from);
            s.to = to;
            Frame f{true, from, to, std::move(frame), true};
            const std::int32_t head = static_cast<std::int32_t>(get32(f.bytes.data()));
            if (head != -1) {
                bool swap = s.held.present;
                if (s.last_sequence != -1) {
                    // Running numbers count down; the next one is the last - 1 (back to -2 past the end).
                    std::int32_t expected = s.last_sequence - 1;
                    if (expected >= -1) expected = -2;
                    const bool gap = head != expected && static_cast<std::int32_t>(static_cast<std::uint32_t>(expected) - static_cast<std::uint32_t>(head)) >= 0;
                    if (gap) {
                        ++order_mismatches_;
                        if (!s.held.present) {
                            s.held = std::move(f);  // wait for the missing one
                            return false;
                        }
                        const std::int32_t held_head = static_cast<std::int32_t>(get32(s.held.bytes.data()));
                        if (!(head < held_head)) {
                            // The new frame is older than the held one: it goes now, the held one next.
                            swap = false;
                            f.spread = false;
                            replay_ = s.id;
                        }
                    }
                }
                if (swap && s.held.present) {
                    // The held frame goes first (unspread), the new one after it.
                    deferred_ = std::move(f);
                    current_ = std::move(s.held);
                    current_.spread = false;
                    s.held = Frame{};
                    s.last_sequence = static_cast<std::int32_t>(get32(current_.bytes.data()));
                } else {
                    s.last_sequence = head;
                    current_ = std::move(f);
                }
            } else {
                current_ = std::move(f);
            }
        }
    }
    Sender& s = *sender(current_.from);
    if (!s.queue.empty()) {
        // Packets of an earlier frame still wait: they are all due now; the frame waits.
        for (Item& it : s.queue) it.release = tick;
        ++restamps_;
        return take_head(s, tick, out);
    }
    split(s, current_.bytes, tick, current_.spread);
    current_ = Frame{};
    return take_head(s, tick, out);
}

} // namespace net
