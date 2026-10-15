// Network layer tests: bit streams, the dictionary coder and message envelope, frames,
// the loopback transport (and, in net_play, whole games).
#include <cstdio>
#include <random>

#include "net/codec.h"
#include "net/frames.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "sim/bitstream.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (c) ++g_pass;                                                  \
        else {                                                            \
            ++g_fail;                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
        }                                                                 \
    } while (0)

static void test_bitstream() {
    std::mt19937 g(7);
    for (int round = 0; round < 50; ++round) {
        sim::BitWriter w;
        std::vector<std::pair<std::uint32_t, int>> vals;
        for (int i = 0; i < 200; ++i) {
            const int bits = 1 + static_cast<int>(g() % 32);
            const std::uint32_t v = g() & (bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1));
            vals.push_back({v, bits});
            w.put(v, bits);
        }
        const auto bytes = w.bytes();
        sim::BitReader r(bytes.data(), bytes.size());
        bool ok = true;
        for (auto& [v, bits] : vals) ok = ok && r.get(bits) == v;
        CHECK(ok);
    }
    sim::BitWriter w;
    w.put(0x2C, 8);
    w.put(0, 16);
    w.put(0x12345678, 32);
    CHECK(w.size() == 7);
    const auto b = w.bytes();
    CHECK(b[0] == 0x2C && b[3] == 0x78 && b[6] == 0x12);
}

static void test_codec() {
    std::mt19937 g(11);
    for (int round = 0; round < 60; ++round) {
        std::vector<std::uint8_t> data(static_cast<std::size_t>(g() % 3000));
        const int alphabet = 1 + static_cast<int>(g() % 255);
        for (auto& c : data) c = static_cast<std::uint8_t>(g() % static_cast<unsigned>(alphabet) + (round % 2 ? 0x70 : 0));
        const auto packed = net::compress(data.data(), data.size());
        CHECK(net::decompress(packed.data.data(), packed.data.size()) == data);
        const auto msg = net::wrap(data.data(), data.size(), true);
        std::vector<std::uint8_t> back;
        CHECK(net::unwrap(msg.data(), msg.size(), back) == net::UnwrapResult::Ok);
        CHECK(back == data);
        auto bad = msg;
        if (bad.size() > 8) {
            bad[4] ^= 0x40;
            CHECK(net::unwrap(bad.data(), bad.size(), back) == net::UnwrapResult::BadChecksum);
        }
    }
    // Short frames are never compressed.
    std::vector<std::uint8_t> s(12, 0);
    CHECK(net::wrap(s.data(), s.size(), true)[0] == net::kPlainMessage);
    std::vector<std::uint8_t> z(400, 0);
    CHECK(net::wrap(z.data(), z.size(), true)[0] == net::kCompressedMessage);
    CHECK(net::wrap(z.data(), z.size(), false)[0] == net::kPlainMessage);
}

static void test_frames() {
    net::FrameSender tx;
    std::vector<net::Message> wire;
    const net::SendFn send = [&](std::int32_t from, std::int32_t to, std::vector<std::uint8_t> m) { wire.push_back({from, to, m}); };
    // 40 small packets in one frame: delivered one per step.
    for (int i = 0; i < 40; ++i) {
        std::uint8_t p[11] = {net::packet::kKilled, static_cast<std::uint8_t>(i)};
        tx.queue(7, net::kEverybody, p, sizeof p, 100, send);
    }
    tx.flush(100, false, send);
    CHECK(wire.size() == 1);
    net::FrameReceiver rx;
    std::size_t next = 0;
    const net::FrameReceiver::ReadFn read = [&](std::int32_t& f, std::int32_t& t, std::vector<std::uint8_t>& m) {
        if (next >= wire.size()) return false;
        f = wire[next].from;
        t = wire[next].to;
        m = wire[next].bytes;
        ++next;
        return true;
    };
    int got = 0;
    net::Delivered d;
    for (std::uint32_t tick = 1000; tick < 1060; ++tick) {
        int this_step = 0;
        while (rx.next(tick, true, read, d)) {
            CHECK(d.from == 7 && d.packet.size() == 11 && d.packet[1] == got);
            ++got;
            ++this_step;
        }
        CHECK(this_step <= 2);
    }
    CHECK(got == 40);
    CHECK(net::packet_size(net::packet::kSideRecord) == 186 && net::packet_size(net::packet::kViewState) == 58);
}

static void test_loopback() {
    net::LoopbackHub hub;
    net::Transport& a = hub.add_machine();
    net::Transport& b = hub.add_machine();
    net::Transport& c = hub.add_machine();
    a.set_local_players({1});
    b.set_local_players({2});
    c.set_local_players({3});
    a.send(1, 0, {1, 2, 3});
    a.send(1, 3, {4});
    net::Message m;
    CHECK(b.receive(m) && m.bytes.size() == 3 && !b.receive(m));
    CHECK(c.receive(m) && c.receive(m) && m.bytes.size() == 1 && m.to == 3);
}

int main() {
    test_bitstream();
    test_codec();
    test_frames();
    test_loopback();
    std::printf("net_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
