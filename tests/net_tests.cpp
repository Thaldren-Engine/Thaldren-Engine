// Network layer tests: bit streams, the dictionary coder and message envelope, frames,
// the loopback transport (and, in net_play, whole games).
#include <cstdio>
#include <random>

#include "net/codec.h"
#include <algorithm>

#include "net/frames.h"
#include "net/lobby.h"
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

static void test_room() {
    net::LoopbackHub hub;
    net::Transport& th = hub.add_machine();
    net::Transport& t1 = hub.add_machine();
    net::Transport& t2 = hub.add_machine();
    net::BattleRoom host(th, true, 0x100, "Host");
    net::BattleRoom a(t1, false, 0x200, "Alice");
    net::BattleRoom b(t2, false, 0x300, "Bob");
    const net::UnitChecks defs = {{1, 11}, {2, 22}, {3, 33}, {4, 44}, {5, 55}};
    host.set_unit_checks(defs);
    a.set_unit_checks(defs);
    net::UnitChecks other = defs;
    other[2].second = 99;  // Bob's third unit differs
    b.set_unit_checks(other);
    host.set_map("Moon Quartet");
    host.set_map_ready(true);
    host.set_options(1, net::room::kLineOfSight, 200, 10, 10, true);
    b.set_side(1);
    std::vector<std::string> lines;
    a.on_chat = [&](const std::string& l) { lines.push_back(l); };
    const int ai = host.add_computer(1);
    CHECK(ai == 1);
    std::uint32_t clock = 100;
    for (int k = 0; k < 40; ++k) {
        clock += 2;
        host.frame(clock);
        a.frame(clock);
        b.frame(clock);
        if (k == 5) {
            a.set_map_ready(true);
            b.set_map_ready(true);
            a.set_ready(true);
            b.set_ready(true);
            host.say("glhf");
        }
    }
    CHECK(a.local_slot() >= 0 && b.local_slot() >= 0 && a.local_slot() != b.local_slot());
    CHECK(host.players()[static_cast<std::size_t>(b.local_slot())].side == 1);
    CHECK(lines.size() == 1 && lines[0] == "Host: glhf");
    CHECK(host.can_start());
    const auto& dis = b.disabled_units();
    CHECK(std::find(dis.begin(), dis.end(), 3u) != dis.end());
    a.next_ally_team();  // team 0
    b.next_ally_team();
    for (int k = 0; k < 4; ++k) {
        clock += 2;
        host.frame(clock);
        a.frame(clock);
        b.frame(clock);
    }
    CHECK(host.players()[static_cast<std::size_t>(a.local_slot())].ally_team == 0);
    CHECK(host.start());
    sim::Lcg15 rh, ra, rb;
    net::LoopbackHub* unused = nullptr;
    (void)unused;
    for (int k = 0; k < 20 && !(host.loaded() && a.loaded() && b.loaded()); ++k) {
        clock += 2;
        host.frame(clock, &rh);
        a.frame(clock, &ra);
        b.frame(clock, &rb);
    }
    CHECK(a.started() && b.started());
    CHECK(host.loaded() && a.loaded() && b.loaded());
    const sim::BattleStart sa = a.battle_start(), sh = host.battle_start(), sb = b.battle_start();
    CHECK(sa.provider == 3 && sa.unit_limit == 200 && sa.commander_mode == 1 && sa.fog_of_war && sa.fixed_locations);
    CHECK(sa.players[static_cast<std::size_t>(a.local_slot())].kind == sim::PlayerKind::Local);
    CHECK(sa.players[0].kind == sim::PlayerKind::Remote && sa.players[1].kind == sim::PlayerKind::Remote);
    CHECK(sh.players[0].kind == sim::PlayerKind::Local && sh.players[1].kind == sim::PlayerKind::Computer);
    CHECK(sh.players[static_cast<std::size_t>(a.local_slot())].kind == sim::PlayerKind::Remote);
    CHECK(a.map() == "Moon Quartet");
    // Fixed locations: positions in slot order, as the host assigned them (each machine knows its own).
    CHECK(sh.players[0].start_position == 0 && sh.players[1].start_position == 1);
    CHECK(sa.players[static_cast<std::size_t>(a.local_slot())].start_position == a.local_slot());
    CHECK(sb.players[static_cast<std::size_t>(b.local_slot())].start_position == b.local_slot());
    CHECK(sa.players[static_cast<std::size_t>(a.local_slot())].ally_group == 0);
    // Logos: all different; groups: the host's machine 1 (its computer player too), the others their own.
    std::vector<int> logos;
    for (const auto& p : host.players())
        if (p.net_id) logos.push_back(p.logo);
    std::sort(logos.begin(), logos.end());
    CHECK(std::adjacent_find(logos.begin(), logos.end()) == logos.end() && logos.back() < 10);
    CHECK(host.players()[0].group == 1 && host.players()[1].group == 1);
    CHECK(host.players()[static_cast<std::size_t>(a.local_slot())].group > 1 && host.players()[static_cast<std::size_t>(b.local_slot())].group > 1 &&
          host.players()[static_cast<std::size_t>(a.local_slot())].group != host.players()[static_cast<std::size_t>(b.local_slot())].group);
    CHECK(a.players()[static_cast<std::size_t>(b.local_slot())].group == host.players()[static_cast<std::size_t>(b.local_slot())].group);
    // The join gate: a latecomer is refused.
    net::Transport& t3 = hub.add_machine();
    net::BattleRoom late(t3, false, 0x400, "Late");
    for (int k = 0; k < 6; ++k) {
        clock += 2;
        late.frame(clock);
        host.frame(clock, &rh);
    }
    late.frame(clock + 2);
    CHECK(late.refused() && late.local_slot() < 0);
}

static void test_room_shuffle() {
    // Start positions shuffled on the host's C runtime rand() (two players: one draw decides).
    int shuffled = 0;
    for (std::uint32_t seed = 1; seed < 9; ++seed) {
        net::LoopbackHub hub;
        net::Transport& th = hub.add_machine();
        net::Transport& t1 = hub.add_machine();
        net::BattleRoom host(th, true, 0x100, "Host");
        net::BattleRoom a(t1, false, 0x200, "Alice");
        host.set_map_ready(true);
        host.set_options(0, 0, 250, 10, 10, false);
        sim::Lcg15 rh, ra;
        rh.set_state(seed * 7919u);
        const std::uint32_t before = rh.state();
        std::uint32_t clock = 100;
        for (int k = 0; k < 60 && !(host.loaded() && a.loaded()); ++k) {
            clock += 2;
            host.frame(clock, &rh);
            a.frame(clock, &ra);
            if (k == 3) {
                a.set_map_ready(true);
                a.set_ready(true);
            }
            if (k > 5 && !host.started() && host.can_start()) host.start();
        }
        CHECK(host.loaded() && a.loaded());
        const int ph = host.battle_start().players[0].start_position, pa = a.battle_start().players[1].start_position;
        CHECK(ph != pa && ph >= 0 && ph < 2 && pa >= 0 && pa < 2);
        // One draw when not shuffling, two when shuffling (the decision and the swap).
        sim::Lcg15 x;
        x.set_state(before);
        x.next();
        if (ph == 1) {
            ++shuffled;
            x.next();
        }
        CHECK(rh.state() == x.state());
    }
    CHECK(shuffled > 0 && shuffled < 8);
}

int main() {
    test_bitstream();
    test_codec();
    test_frames();
    test_loopback();
    test_room();
    test_room_shuffle();
    std::printf("net_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
