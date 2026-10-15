#pragma once
// Batching of packets into frames and their delivery on the receiving side, as the original
// does it with frame timing on (its default; see docs/RE_MEMORY.md, "Network and sync").
//
// Sending: packets wait in a channel (one for "everybody" and one per addressee). A channel
// sends at most once per interval (200 ms by default, in 1/30 s clock ticks: 6), or at once
// when forced or when more than 0x429 bytes or 0x400 packets wait. Sending makes one frame
// per sending player: a 4-byte head (the "everybody" channel's running number, starting at -2
// and counting down; -1 for addressed frames) and the packets in order, then the message
// envelope (net/codec.h).
//
// Receiving: a frame's packets are queued per sending player with the game step from which
// each may be delivered: they are spread over the coming steps (one per step for up to 30
// packets, about count/30 per step above that; within the first 30 steps of a battle over as
// many steps as have passed). In a frame of more than 128 packets the oldest unit state
// packets beyond 128 are dropped. A queue holds at most 512 packets. Delivery takes the
// first deliverable head of the players' queues in their order; only when none is ready is
// the next message read, and if its sender's queue head is still waiting nothing more is
// read this time. At step 0 (outside a battle) everything is delivered at once.

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace net {

constexpr std::int32_t kEverybody = 0;  // addressee "all players"

struct NetTiming {
    bool frame_timing = true;    // off: every packet is sent at once, unwrapped (the "-p-" switch)
    bool compression = true;     // the frame compression switch
    std::uint32_t interval_ticks = 6;  // clock ticks (1/30 s) between a channel's sends
    // The original's switch "-p<n>": n packets per second (2..30, 0 = 200 ms); negative turns
    // frame timing off.
    static NetTiming from_rate(int packets_per_second);
};

using SendFn = std::function<void(std::int32_t from, std::int32_t to, std::vector<std::uint8_t> message)>;

class FrameSender {
public:
    explicit FrameSender(NetTiming timing = {}) : timing_(timing) {}
    void set_timing(const NetTiming& t) { timing_ = t; }
    const NetTiming& timing() const { return timing_; }
    // Queues a packet (with frame timing off it goes out at once through `send`).
    void queue(std::int32_t from, std::int32_t to, const std::uint8_t* packet, std::size_t size, std::uint32_t clock,
               const SendFn& send);
    // Sends every channel whose interval has passed (all when forced).
    void flush(std::uint32_t clock, bool force, const SendFn& send);
    std::uint64_t frames_sent() const { return frames_sent_; }
    std::uint64_t bytes_sent() const { return bytes_sent_; }

private:
    struct Pending {
        std::int32_t from;
        std::vector<std::uint8_t> bytes;
    };
    struct Channel {
        std::int32_t to = -1;  // -1 unused
        std::deque<Pending> pending;
        std::size_t bytes = 0;
        std::uint32_t deadline = 0;
        std::int32_t sequence = -2;
    };
    Channel& channel(std::int32_t to);
    void send_channel(Channel& c, std::uint32_t clock, bool force, const SendFn& send);

    NetTiming timing_;
    Channel everybody_{kEverybody};
    std::vector<Channel> direct_;
    std::uint64_t frames_sent_ = 0, bytes_sent_ = 0;
};

struct Delivered {
    std::int32_t from = 0, to = 0;
    std::vector<std::uint8_t> packet;
};

class FrameReceiver {
public:
    // Reads the next message (from, to, bytes); false when none is waiting.
    using ReadFn = std::function<bool(std::int32_t& from, std::int32_t& to, std::vector<std::uint8_t>& message)>;
    // The next deliverable packet at game step `tick`; false when nothing is ready.
    bool next(std::uint32_t tick, bool frame_timing, const ReadFn& read, Delivered& out);
    std::uint64_t dropped() const { return dropped_; }
    std::uint64_t bad_messages() const { return bad_; }
    std::uint64_t order_mismatches() const { return order_mismatches_; }
    // Waiting packets (all senders).
    std::size_t waiting() const;
    // A frame's packet sequence contained a size-0 type: the original loops forever there.
    bool stuck() const { return stuck_; }

    // Frames that waited for an earlier one (out-of-order arrivals) and their replays.
    std::uint64_t restamps() const { return restamps_; }

private:
    struct Item {
        std::uint32_t release;
        std::vector<std::uint8_t> packet;
    };
    struct Frame {
        bool present = false;
        std::int32_t from = 0, to = 0;
        std::vector<std::uint8_t> bytes;
        bool spread = true;
    };
    struct Sender {
        std::int32_t id = -1, to = -1;
        std::deque<Item> queue;
        std::int32_t last_sequence = -1;  // -1: none yet
        std::uint32_t base_tick = 0;      // never set by the original after creation (0)
        Frame held;                       // a frame that came after a gap
    };
    static constexpr std::int32_t kNone = 0x7FFFFFFF;
    Sender* sender(std::int32_t id);
    static bool ready(const Item& it, std::uint32_t tick);
    bool take_head(Sender& s, std::uint32_t tick, Delivered& out);
    void split(Sender& s, const std::vector<std::uint8_t>& frame, std::uint32_t tick, bool spread);

    std::deque<Sender> senders_;
    Frame current_, deferred_;
    std::int32_t replay_ = kNone;
    std::uint64_t dropped_ = 0, bad_ = 0, order_mismatches_ = 0, restamps_ = 0;
    bool stuck_ = false;
};

} // namespace net
