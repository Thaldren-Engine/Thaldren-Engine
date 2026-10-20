// Network layer tests: bit streams, the dictionary coder and message envelope, frames,
// the loopback transport (and, in net_play, whole games).
#include <cstdio>
#include <random>

#include "net/codec.h"
#include <algorithm>
#include <deque>

#include "net/dplay_serial.h"
#include "net/dplay_session.h"

#include "net/frames.h"
#include "net/lobby.h"
#include "net/net_battle.h"
#include "net/protocol.h"
#include "sim/battle_quiet.h"
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

// Frames out of order: a frame after a gap waits; the held one goes when the next arrives.
static void test_frame_order() {
    net::FrameSender tx;
    std::vector<net::Message> wire;
    const net::SendFn send = [&](std::int32_t f, std::int32_t t, std::vector<std::uint8_t> m) { wire.push_back({f, t, m}); };
    for (int i = 0; i < 4; ++i) {
        std::uint8_t p[11] = {net::packet::kKilled, static_cast<std::uint8_t>(i)};
        tx.queue(7, net::kEverybody, p, sizeof p, 100, send);
        tx.flush(100, true, send);
    }
    CHECK(wire.size() == 4);
    auto run = [&](const std::vector<int>& order) {
        net::FrameReceiver rx;
        std::size_t next = 0;
        const net::FrameReceiver::ReadFn read = [&](std::int32_t& f, std::int32_t& t, std::vector<std::uint8_t>& m) {
            if (next >= order.size()) return false;
            const net::Message& w = wire[static_cast<std::size_t>(order[next++])];
            f = w.from;
            t = w.to;
            m = w.bytes;
            return true;
        };
        std::vector<int> got;
        net::Delivered d;
        for (std::uint32_t tick = 1000; tick < 1020; ++tick)
            while (rx.next(tick, true, read, d)) got.push_back(d.packet[1]);
        return got;
    };
    CHECK((run({0, 1, 2, 3}) == std::vector<int>{0, 1, 2, 3}));
    // 1 is late: 2 waits; when 1 comes the held 2 goes first (the original's order), then 1, 3.
    CHECK((run({0, 2, 1, 3}) == std::vector<int>{0, 2, 1, 3}));
    // 1 and 2 swapped twice over: 3 held, then 2 arrives (older than 3): 2 now, 3 after; 1 last.
    CHECK((run({0, 3, 2, 1}).size() == 4));
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
    // (The host's frames alternate between its two players' sends on one running number, so the
    // clients hold every other frame until the next arrives, as the original's receiver does.)
    bool ready_a = false, ready_b = false;
    for (int k = 0; k < 150; ++k) {
        clock += 2;
        host.frame(clock);
        a.frame(clock);
        b.frame(clock);
        if (k == 5) {
            a.set_map_ready(true);
            b.set_map_ready(true);
            host.say("glhf");
        }
        if (!ready_a && a.local_slot() >= 0) a.set_ready(ready_a = true);
        if (!ready_b && b.local_slot() >= 0) b.set_ready(ready_b = true);
    }
    CHECK(a.local_slot() >= 0 && b.local_slot() >= 0 && a.local_slot() != b.local_slot());
    CHECK(host.players()[static_cast<std::size_t>(b.local_slot())].side == 1);
    CHECK(lines.size() == 1 && lines[0] == "Host: glhf");
    if (!host.can_start()) {
        for (const auto& p : host.players())
            if (p.net_id) std::printf("debug: id %x kind %d ready %d map %d logo %d\n", p.net_id, p.kind, p.ready(), p.map_ready, p.logo);
    }
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
    // Alliances in the room (0x446C70 team rule, 0x452960, 0x446E90): teammates allied both
    // ways on every machine; the host allies with A by hand (A hears it in +0x113 only); B
    // leaves the team: both ends of A-B forget it (forced).
    const std::size_t sla = static_cast<std::size_t>(a.local_slot()), slb = static_cast<std::size_t>(b.local_slot());
    CHECK(a.alliance_rows()[sla][slb] == 1 && b.alliance_rows()[slb][sla] == 1 && host.alliance_rows()[sla][slb] == 1);
    host.set_allied(static_cast<int>(sla), true);
    b.next_ally_team();  // team 1
    for (int k = 0; k < 4; ++k) {
        clock += 2;
        host.frame(clock);
        a.frame(clock);
        b.frame(clock);
    }
    CHECK(host.alliance_rows()[0][sla] == 1 && a.allied_by()[0] == 1 && a.alliance_rows()[sla][0] == 0);
    CHECK(a.alliance_rows()[sla][slb] == 0 && b.alliance_rows()[slb][sla] == 0);
    CHECK(host.start());
    sim::Lcg15 rh, ra, rb;
    net::LoopbackHub* unused = nullptr;
    (void)unused;
    for (int k = 0; k < 40 && !(host.loaded() && a.loaded() && b.loaded()); ++k) {
        clock += 2;
        host.frame(clock, &rh);
        if (host.loaded()) {
            // The host's battle has begun: its players' traffic releases the frames the clients
            // held back (stand-in packets from both of the host's players).
            const net::SendFn send = [&](std::int32_t f, std::int32_t t, std::vector<std::uint8_t> m) { th.send(f, t, std::move(m)); };
            const std::uint8_t probe[1] = {net::packet::kProbe};
            host.sender().queue(0x100, net::kEverybody, probe, 1, clock, send);
            host.sender().queue(host.players()[1].net_id, net::kEverybody, probe, 1, clock, send);
            host.sender().flush(clock, true, send);
        }
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
    // The battle starts with the records' alliance bytes.
    CHECK(sa.alliances_given && sh.alliances[0][sla] == 1 && sa.alliances[sla][0] == 0 && sa.alliances[sla][slb] == 0 &&
          sa.alliances[sla][sla] == 1);
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

// A joining machine adds its own computer player (0x447B10 on a free row): the host seats it,
// every machine sees it, only the joiner runs it (one per machine is the lounge's rule).
static void test_room_joiner_computer() {
    net::LoopbackHub hub;
    net::Transport& th = hub.add_machine();
    net::Transport& t1 = hub.add_machine();
    net::BattleRoom host(th, true, 0x100, "Host");
    net::BattleRoom a(t1, false, 0x200, "Alice");
    host.set_map("Moon Quartet");
    host.set_map_ready(true);
    std::uint32_t clock = 100;
    auto run = [&](int n) {
        for (int k = 0; k < n; ++k) {
            clock += 2;
            host.frame(clock);
            a.frame(clock);
        }
    };
    run(30);
    CHECK(a.local_slot() >= 0);
    // The periodic probes (0x06) were answered (0x07): each machine's record of the other has
    // its byte +0x21 bit 0 (0x453D40 case 5).
    for (int s = 0; s < 10; ++s) {
        if (host.players()[static_cast<std::size_t>(s)].net_id == 0x200) CHECK(host.probe_answered(s));
        if (a.players()[static_cast<std::size_t>(s)].net_id == 0x100) CHECK(a.probe_answered(s));
    }
    CHECK(a.add_own_computer());
    run(30);
    int seen_host = -1, seen_a = -1;
    for (int s = 0; s < 10; ++s) {
        if (host.players()[static_cast<std::size_t>(s)].kind == 2) seen_host = s;
        if (a.players()[static_cast<std::size_t>(s)].kind == 2) seen_a = s;
    }
    CHECK(seen_host >= 0 && seen_host == seen_a);
    if (seen_a >= 0) {
        CHECK(a.runs(seen_a) && !host.runs(seen_a));
        CHECK(a.players()[static_cast<std::size_t>(seen_a)].name == "AI:Alice");
        CHECK(a.remove_computer(seen_a, 1));
        run(30);
        CHECK(host.players()[static_cast<std::size_t>(seen_a)].net_id == 0);
    }
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

// ---- the original's session protocol (net/dplay_*.h): messages the original program sent,
// decoded and made again byte for byte; the serial frame; a session over an in-memory link.
static std::vector<std::uint8_t> unhex(const char* h) {
    std::vector<std::uint8_t> v;
    for (std::size_t i = 0; h[i] && h[i + 1]; i += 2) {
        auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        v.push_back(static_cast<std::uint8_t>(nib(h[i]) * 16 + nib(h[i + 1])));
    }
    return v;
}

// Observed (TCP/IP, message version 14): the payloads from the signature on.
static const char* kObsRequestId = "706c617905000e0009000000";
static const char* kObsAddForward =
    "706c617913000e00000000007a563512000000001c0000006c00000050000000090000007a56351200000000000000002000000000000000000000007a5635123000"
    "00000e00000000000000020008fc0000000000000000000000000200092e000000000000000000000000000078563412";
static const char* kObsCreate =
    "706c617908000e00000000007b563512000000001c00000000000000a1000000080000007b5635121e0000001e0000002000000015000000000000007a5635123000"
    "00000e000000000000005000520049004d0045005f005400680061006c006400720065006e0000005000520049004d0045005f005400680061006c006400720065006e"
    "000000020008fc0000000000000000000000000200092e000000000000000000000000000000000000000000000000000000000000005000000000000000";
static const char* kObsEnumReply =
    "706c617901000e005000000000000000c6473133b5d6f64592b80ff070ee6a7120747999f5f5cf11982700a0241496c80a000000010000000000000000000000"
    "29e9d61200000000000000000000000000000000000003015c0000005000520049004d0045005f005400680061006c006400720065006e0027007300430061006e0061"
    "006c002000430072006f007300730069006e00670020000000";
static const char* kObsSuper =
    "706c617929000e000300000000000000b40000000000000024000000740000000000000050000000000000006231a83206fe3b47898c644e19f6d96520747999f5f5"
    "cf11982700a0241496c80a0000000100000000000000000000004002da12000000000008010e040000000a000a00fa0003015000520049004d0045005f00540068006100"
    "6c006400720065006e0027007300430061006e0061006c002000430072006f007300730069006e0067002000000010000000050000004202db12040000000e00000020"
    "0200092e7f00000100000000000000000200092f7f0000010000000000000000100000000f0000004002db12040000000e00000020020008fc00000000000000000000"
    "00000200092e000000000000000000000000100000000c0000004102db12170000004002db125000520049004d0045005f005400680061006c006400720065006e0000"
    "005000520049004d0045005f005400680061006c006400720065006e0000001500000000000000000000000000000000000000500020020008fc000000000000000000"
    "0000000200092e000000000000000000000000";

// Two in-memory ends of one link (a provider pair for sessions in one process).
struct MemLink {
    std::deque<std::vector<std::uint8_t>> to[2];
};
class MemProvider final : public net::dplay::Provider {
public:
    MemProvider(MemLink& l, int side) : l_(l), side_(side) {}
    const char* name() const override { return "memory"; }
    net::dplay::Bytes local_sp_data() const override { return {}; }
    bool same_machine(const net::dplay::Bytes&, const net::dplay::Bytes&) const override { return true; }
    void send(const net::dplay::Bytes&, const net::dplay::Bytes& p, bool) override { l_.to[1 - side_].push_back(p); }
    bool receive(net::dplay::Inbound& in) override {
        auto& q = l_.to[side_];
        if (q.empty()) return false;
        in = net::dplay::Inbound{};
        in.payload = std::move(q.front());
        q.pop_front();
        return true;
    }
    void pump() override {}
    void enumerate(const net::dplay::Bytes& p) override { send({}, p, true); }
    bool listen(std::string&) override { return true; }

private:
    MemLink& l_;
    int side_;
};

static void test_dplay() {
    using namespace net::dplay;
    // Requests and the full player form, made again from what they carry.
    CHECK(request_player_id(kSystemPlayer | kSenderLocal) == unhex(kObsRequestId));
    {
        const auto obs = unhex(kObsAddForward);
        auto m = decode(obs.data(), obs.size());
        CHECK(m && m->command == cmd::kAddForwardRequest && m->player.flags == 9 && m->key == 0x12345678u && m->player.sp_data.size() == 32);
        if (m) CHECK(add_forward_request(m->to, m->player, m->password, m->key) == obs);
    }
    {
        const auto obs = unhex(kObsCreate);
        auto m = decode(obs.data(), obs.size());
        CHECK(m && m->command == cmd::kCreatePlayer && m->player.player_data.size() == 21 && m->player.player_data[0x13] == 0x50);
        if (m) CHECK(create_player(m->to, m->player) == obs);
    }
    {
        const auto obs = unhex(kObsEnumReply);
        auto m = decode(obs.data(), obs.size());
        CHECK(m && m->desc.max_players == 10 && m->desc.name.size() == 31 && m->desc.application == game_application());
        if (m) CHECK(enum_sessions_reply(m->desc) == obs);
    }
    {
        // The name table: the joiner first, then the host's system player and its game player.
        const auto obs = unhex(kObsSuper);
        auto m = decode(obs.data(), obs.size());
        CHECK(m && m->players.size() == 3);
        if (m && m->players.size() == 3) {
            CHECK(m->players[1].flags == 0xF && m->players[2].flags == 0xC && m->players[2].system_id == m->players[1].id);
            CHECK(super_enum_players_reply(m->desc, m->players) == obs);
            // Player ids: (uniqueness << 16 | index) XOR the session key.
            CHECK(m->players[1].id == make_player_id(m->desc.reserved1, 0) && m->players[0].id == make_player_id(m->desc.reserved1, 2));
        }
    }
    // The serial frame: the check is CRC-16/ARC (check value 0xBB3D); a damaged head resyncs.
    {
        const std::vector<std::uint8_t> nine = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        const auto f = serial_frame(nine);
        CHECK(f.size() == 17 && f[0] == 0xAD && f[1] == 0x2B && f[2] == 17 && f[4] == 0x3D && f[5] == 0xBB);
        std::vector<std::uint8_t> stream = {0x12, 0xAD, 0x2B, 0x99};
        stream.insert(stream.end(), f.begin(), f.end());
        auto bad = f;
        bad[10] ^= 1;  // a damaged message is dropped
        stream.insert(stream.end(), bad.begin(), bad.end());
        stream.insert(stream.end(), f.begin(), f.end());
        std::vector<std::uint8_t> p;
        int got = 0;
        while (serial_unframe(stream, p)) got += p == nine ? 1 : 100;
        CHECK(got == 2 && stream.empty());
    }
    // A reliable packet as the original sent it over serial (its player id request in one piece):
    // acknowledged with the message id and piece index, then handled (the host answers the id).
    {
        MemLink link;
        Session host(std::make_unique<MemProvider>(link, 0));
        HostSettings hs;
        hs.session_name = "Serial";
        std::string err;
        CHECK(host.host(hs, err));
        const char* hex = "706c617930000e00c60ea5aaf6ff00499a731eba429ada6d000000000c00000000000000010000000c00000000000000706c617905000e0009000000";
        std::vector<std::uint8_t> pk;
        for (const char* h = hex; h[0] && h[1]; h += 2) pk.push_back(static_cast<std::uint8_t>(std::stoi(std::string(h, 2), nullptr, 16)));
        link.to[0].push_back(pk);
        host.pump();
        bool acked = false, answered = false;
        for (const auto& m : link.to[1]) {
            if (m.size() == 28 && m[4] == 0x31 && std::equal(m.begin() + 8, m.end(), pk.begin() + 8)) acked = true;
            if (m.size() >= 8 && m[4] == 0x07) answered = true;
        }
        CHECK(acked);
        CHECK(answered);
        // The same piece again (its acknowledgement lost): acknowledged, not handled twice.
        link.to[1].clear();
        link.to[0].push_back(pk);
        host.pump();
        CHECK(link.to[1].size() == 1 && link.to[1][0].size() == 28);
    }
    // A session: host, a machine joins, both make players, game messages both ways, one leaves.
    {
        MemLink link;
        Session host(std::make_unique<MemProvider>(link, 0)), join(std::make_unique<MemProvider>(link, 1));
        HostSettings hs;
        hs.session_name = "Test";
        hs.password = "pw";
        std::string err;
        CHECK(host.host(hs, err));
        const std::int32_t hp = host.create_player("H", {1, 2, 3});
        CHECK(hp > 0 || hp < -1);
        auto settle = [&] {
            for (int i = 0; i < 10; ++i) {
                host.pump();
                join.pump();
            }
        };
        join.find();
        settle();
        CHECK(join.sessions().size() == 1 && join.sessions()[0].desc.name == "Test" && (join.sessions()[0].desc.flags & kPasswordRequired));
        // A wrong password is refused, the right one taken.
        join.join(join.sessions()[0], "no");
        settle();
        CHECK(join.state() == Session::State::Failed && join.result() == kErrInvalidPassword);
        Session join2(std::make_unique<MemProvider>(link, 1));
        join2.find();
        for (int i = 0; i < 10; ++i) {
            host.pump();
            join2.pump();
        }
        join2.join(join2.sessions()[0], "pw");
        for (int i = 0; i < 10; ++i) {
            host.pump();
            join2.pump();
        }
        CHECK(join2.state() == Session::State::Joined);
        CHECK(join2.create_player("J", {4}) == 0);
        for (int i = 0; i < 10; ++i) {
            host.pump();
            join2.pump();
        }
        net::SessionEvent e;
        std::int32_t jp = 0, seen_h = 0;
        while (join2.next_event(e))
            if (e.kind == net::SessionEvent::Kind::PlayerCreated) (e.local ? jp : seen_h) = e.id;
        CHECK(jp != 0 && seen_h == hp && join2.player_name(hp) == "H");
        std::int32_t host_saw = 0;
        while (host.next_event(e))
            if (e.kind == net::SessionEvent::Kind::PlayerCreated && !e.local) host_saw = e.id;
        CHECK(host_saw == jp && host.player_name(jp) == "J");
        host.send(hp, 0, {7, 7});
        join2.send(jp, hp, {8});
        for (int i = 0; i < 10; ++i) {
            host.pump();
            join2.pump();
        }
        net::Message m;
        CHECK(join2.receive(m) && m.from == hp && m.to == 0 && m.bytes == std::vector<std::uint8_t>({7, 7}));
        CHECK(host.receive(m) && m.from == jp && m.to == hp && m.bytes == std::vector<std::uint8_t>({8}));
        join2.destroy_player(jp);
        for (int i = 0; i < 10; ++i) {
            host.pump();
            join2.pump();
        }
        bool gone = false;
        while (host.next_event(e)) gone = gone || (e.kind == net::SessionEvent::Kind::PlayerDestroyed && e.id == jp);
        CHECK(gone && !host.player(static_cast<std::uint32_t>(jp)));
    }
}

// A transport over the loopback hub that can also deliver the session's own messages.
struct EventTransport : net::Transport {
    net::Transport& in;
    std::deque<net::SessionEvent> events;
    explicit EventTransport(net::Transport& t) : in(t) {}
    void send(std::int32_t f, std::int32_t t, std::vector<std::uint8_t> b) override { in.send(f, t, std::move(b)); }
    bool receive(net::Message& m) override { return in.receive(m); }
    void set_local_players(const std::vector<std::int32_t>& ids) override { in.set_local_players(ids); }
    bool next_event(net::SessionEvent& e) override {
        if (events.empty()) return false;
        e = events.front();
        events.pop_front();
        return true;
    }
};

// Two machines in a battle: A runs slot 0 (human, id 101, the host) and slot 1 (computer, 102),
// B runs slot 2 (human, 201) and slot 3 (computer, 202). Packets go out at once (no frames).
struct LeaveRig {
    net::LoopbackHub hub;
    EventTransport ta{hub.add_machine()}, tb{hub.add_machine()};
    sim::HeightMap hm{16, 16};
    sim::QuietBattleOutside qa, qb;
    sim::BattleRng ra, rb;
    sim::Battle a{qa, hm, ra}, b{qb, hm, rb};
    net::NetTiming timing() {
        net::NetTiming t;
        t.frame_timing = false;
        return t;
    }
    net::NetBattle na{qa, ta, timing()}, nb{qb, tb, timing()};
    LeaveRig() {
        std::array<net::RoomPlayer, 10> room{};
        const std::int32_t ids[4] = {101, 102, 201, 202};
        for (int s = 0; s < 4; ++s) {
            room[static_cast<std::size_t>(s)].net_id = ids[s];
            room[static_cast<std::size_t>(s)].kind = (s & 1) ? 2 : 1;
            room[static_cast<std::size_t>(s)].group = s < 2 ? 1 : 2;
        }
        room[0].flags = net::room::kHost;
        for (sim::Battle* x : {&a, &b}) {
            x->alliances.assign(10, {});
            for (int s = 0; s < 4; ++s) {
                sim::PlayerSlot& p = x->players[static_cast<std::size_t>(s)];
                p.active = true;
                p.state = static_cast<std::uint8_t>(s);
                p.net_id = ids[s];
            }
            x->victory.sides[0].host = true;
        }
        a.players[0].controller = 1, a.players[1].controller = 2, a.players[2].controller = 3, a.players[3].controller = 3;
        b.players[0].controller = 3, b.players[1].controller = 3, b.players[2].controller = 1, b.players[3].controller = 2;
        a.settings.local_player = 0;
        b.settings.local_player = 2;
        na.attach(a);
        nb.attach(b);
        na.set_room(room);
        nb.set_room(room);
    }
    void pump() {
        for (int k = 0; k < 3; ++k) {
            na.poll();
            nb.poll();
        }
    }
};

static void test_battle_leave_paths() {
    {
        // 0x07 probe answers set the sender record's +0x21 bit 0 (case 5).
        LeaveRig r;
        r.ta.send(101, 0, {0x06});
        r.pump();
        CHECK(r.na.probe_answered(2) && !r.nb.probe_answered(0));
    }
    {
        // B's time-out Reject sends A's human away (0x453010, remote human): A's whole machine
        // group freed on B, the host flag passed to the highest human id (B's 201).
        LeaveRig r;
        r.b.alliances[2][0] = 1;
        r.b.alliances[3][1] = 1;
        r.a.alliances[0][2] = 1;
        r.nb.reject(0, 6);
        CHECK(!r.b.players[0].active && !r.b.players[1].active && r.b.players[0].controller == 0);
        CHECK(r.nb.leaving(0) == 6 && r.nb.leaving(1) == 6);
        CHECK(r.b.alliances[2][0] == 0 && r.b.alliances[3][1] == 0);
        CHECK(r.b.victory.sides[2].host && (r.nb.room()[2].flags & net::room::kHost));
        r.pump();
        // A hears its own human named (0x1B): a status for each local player from the first, so
        // only the first goes out; 0x452CC0 runs for the human each time: its computer player
        // stays (not freed, controller 2), the human stays in the record with its own alliance
        // row cleared; the host flag goes to the highest id among controllers 1 and 3: on A
        // that is B's computer player (202, a remote record here), the old flag stays.
        CHECK(r.na.counters().packets_sent == 1);  // (by_type counts both ways)
        CHECK(r.na.leaving(0) == 6 && r.na.leaving(1) == 6);
        CHECK(r.a.players[0].active && r.a.players[0].controller == 1);
        CHECK(r.a.players[1].active && r.a.players[1].controller == 2);
        CHECK(r.a.alliances[0][2] == 0);
        CHECK(r.a.victory.sides[3].host && r.a.victory.sides[0].host && !r.a.victory.sides[2].host);
    }
    {
        // A session "player destroyed" (system message 5) for B's human on A: sent away with
        // reason 1 (status 0x1B, B's group freed), controller 0; no 0x1C.
        LeaveRig r;
        r.ta.events.push_back({net::SessionEvent::Kind::PlayerDestroyed, 201});
        r.na.poll();
        CHECK(!r.a.players[2].active && !r.a.players[3].active && r.a.players[2].controller == 0);
        CHECK(r.na.leaving(2) == 1 && r.na.counters().by_type[0x1B] == 1 && r.na.counters().by_type[0x1C] == 0);
    }
    {
        // A received 0x1C (case 0x1A): the named player through 0x452CC0, passed on once from
        // this machine's own player; naming this machine's human sets the disconnected flag.
        LeaveRig r;
        int shown = -1;
        r.nb.on_disconnected = [&](int s, int) { shown = s; };
        r.b.alliances[2][3] = 1;
        r.nb.set_phase(net::Phase::Loading);
        r.ta.send(101, 0, {0x1C, 202, 0, 0, 0});
        r.nb.poll();
        CHECK(shown == 3 && r.b.alliances[2][3] == 1);  // loading: a local player is not removed
        r.nb.set_phase(net::Phase::Battle);
        r.ta.send(101, 0, {0x1C, 202, 0, 0, 0});
        r.nb.poll();
        CHECK(r.b.alliances[2][3] == 0 && r.nb.counters().packets_sent == 2 && !r.nb.local_disconnected());
        r.na.poll();
        CHECK(!r.a.players[3].active && r.na.counters().packets_sent == 1);  // passed on once
        r.ta.send(101, 0, {0x1C, 201, 0, 0, 0});
        r.nb.poll();
        CHECK(r.nb.local_disconnected());
    }
}

int main() {
    test_dplay();
    test_battle_leave_paths();
    test_bitstream();
    test_codec();
    test_frames();
    test_frame_order();
    test_loopback();
    test_room();
    test_room_joiner_computer();
    test_room_shuffle();
    std::printf("net_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
