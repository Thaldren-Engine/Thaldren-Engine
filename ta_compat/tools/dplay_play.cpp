// Network games over the original's session protocol (net/dplay_session.h): with the original
// program on the other side, or between two of these tools. The room (net::BattleRoom), the
// loading handshake and the battle (net::NetBattle) run as in the client; this tool readies its
// player by itself, and as the host starts the game once enough players are ready.
//
//   dplay_play <game_dir> host [options]
//   dplay_play <game_dir> join <address> [options]
//   dplay_play <game_dir> serial <port|pipe> host|join [options]   (net/dplay_serial.h)
//   dplay_play <game_dir> modem <port> dial <number>|answer [options]
// options:
//   --loopback          sockets on 127.0.0.1 only (tests on one machine)
//   --name <s>          the player's name (default "Engine")
//   --game <s>          host: the game's name
//   --map <s>           host: the map (default "Moon Quartet")
//   --password <s>
//   --players <n>       host: start once n humans are seated and ready (default 2)
//   --computer <n>      host: computer players in the room
//   --steps <n>         battle steps after loading (default 900, 30 per second)
//   --room <seconds>    longest wait in the room (default 180)
//   --log <file>        every session message, sent and received
//   --no-battle         stop when the game would load
//   --say <text>        a chat line in the room once seated
//   --stream-port <n> / --datagram-port <n>   the first ports tried (TCP/IP)

#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net/codec.h"
#include "net/dplay_serial.h"
#include "net/protocol.h"
#include "net/dplay_session.h"
#include "net/dplay_tcpip.h"
#include "net/lobby.h"
#include "net/net_battle.h"
#include "net/session.h"
#include "sim/battle.h"
#include "sim/battle_quiet.h"
#include "sim/commands.h"
#include "ta/battle_loader.h"
#include "ta/net_setup.h"
#include "ta/unitdefs.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string mode, address, serial_port, serial_role, phone;
    bool loopback = false, no_battle = false;
    std::string name = "Engine", game = "Engine game", map = "Moon Quartet", password, log, say;
    int players = 2, computers = 0, steps = 900, room_seconds = 180;
    std::uint16_t stream_port = 2300, datagram_port = 2350;
};

FILE* g_log = nullptr;
Clock::time_point g_t0 = Clock::now();

double secs() { return std::chrono::duration<double>(Clock::now() - g_t0).count(); }

void logline(const std::string& s) {
    if (!g_log) return;
    std::fprintf(g_log, "%9.3f %s\n", secs(), s.c_str());
    std::fflush(g_log);
}

void say(const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    std::printf("%8.2f %s\n", secs(), b);
    logline(std::string("## ") + b);
}

std::uint32_t clock30() { return 1000u + static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_t0).count() * 30 / 1000); }

// The player's data in the session (0x451220): the password typed (17 bytes), then the words 0 and 0x50.
std::vector<std::uint8_t> player_data(const std::string& password) {
    std::vector<std::uint8_t> d(21, 0);
    std::memcpy(d.data(), password.data(), std::min<std::size_t>(password.size(), 16));
    d[0x13] = 0x50;
    return d;
}

// Scripted orders for this machine's player: move, build, attack (as net_play gives them).
void give_orders(sim::Battle& b, const ta::LoadedBattle& lb, int slot, std::mt19937& rng) {
    if (rng() % 40 != 0) return;
    const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(slot)];
    std::vector<std::uint16_t> mine;
    for (int id = p.first_unit; id <= p.last_unit; ++id)
        if (id > 0 && b.ticks[static_cast<std::size_t>(id)].type_index && (b.units[static_cast<std::size_t>(id)].flags & sim::unit_flags::kAlive))
            mine.push_back(static_cast<std::uint16_t>(id));
    if (mine.empty()) return;
    sim::command::Command sel;
    sel.kind = sim::command::Kind::Select;
    sel.player = static_cast<std::uint8_t>(slot);
    sel.units.push_back(mine[rng() % mine.size()]);
    sim::command::apply(b, sel);
    const sim::Unit& u = b.units[sel.units[0]];
    auto around = [&](int r) {
        std::array<std::int32_t, 3> pos = u.pos;
        pos[0] += (static_cast<int>(rng() % static_cast<unsigned>(2 * r + 1)) - r) << 16;
        pos[2] += (static_cast<int>(rng() % static_cast<unsigned>(2 * r + 1)) - r) << 16;
        pos[0] = std::clamp(pos[0], 48 << 16, (b.map.width * 16 - 48) << 16);
        pos[2] = std::clamp(pos[2], 48 << 16, (b.map.height * 16 - 48) << 16);
        return pos;
    };
    sim::command::Command c;
    c.player = static_cast<std::uint8_t>(slot);
    const std::uint16_t type = b.ticks[sel.units[0]].type_index;
    const ta::UnitDefRecord* d = type < lb.type_defs.size() ? lb.type_defs[type] : nullptr;
    if (rng() % 2 == 0 && d && d->build_options_count > 0 && b.ticks[sel.units[0]].has_motion) {
        c.kind = sim::command::Kind::MobileBuild;
        c.units = {sel.units[0]};
        c.unit_type = d->build_options[rng() % static_cast<unsigned>(std::min(d->build_options_count, 12))];
        c.has_pos = true;
        c.pos = around(160);
    } else {
        c.kind = sim::command::Kind::IssueIntent;
        c.intent = sim::intent::kMove;
        c.has_pos = true;
        c.pos = around(300);
    }
    sim::command::apply(b, c);
}

std::unique_ptr<net::dplay::Session> open_session(const Options& o, std::string& err) {
    auto log = [](const std::string& l) { logline(l); };
    if (o.mode == "serial" || o.mode == "modem") {
        net::dplay::SerialOptions so;
        so.port = o.serial_port;
        if (o.mode == "modem") {
            so.modem = true;
            so.phone = o.phone;
            so.answer = o.serial_role == "answer";
        }
        auto p = net::dplay::serial_provider(so, err);
        if (!p) return nullptr;
        return std::make_unique<net::dplay::Session>(std::move(p), log);
    }
    net::dplay::TcpipOptions to;
    to.bind_ip = o.loopback ? net::dplay::kLoopback : 0;
    to.address = o.address;
    to.stream_port = o.stream_port;
    to.datagram_port = o.datagram_port;
    auto p = net::dplay::tcpip_provider(to, err);
    if (!p) return nullptr;
    return std::make_unique<net::dplay::Session>(std::move(p), log);
}

int run(ta::Vfs& vfs, const Options& o) {
    std::string err;
    std::unique_ptr<net::dplay::Session> session = open_session(o, err);
    if (!session) {
        say("cannot open the session: %s", err.c_str());
        return 1;
    }
    const bool host = o.mode == "host" || (o.mode == "serial" && o.serial_role == "host") || (o.mode == "modem" && o.serial_role == "answer");
    std::int32_t my_id = 0;
    if (host) {
        net::dplay::HostSettings hs;
        hs.session_name = o.game;
        hs.password = o.password;
        if (!session->host(hs, err)) {
            say("cannot host: %s", err.c_str());
            return 1;
        }
        my_id = session->create_player(o.name, player_data(o.password));
        say("hosting \"%s\" on %s; player id %08x", o.game.c_str(), session->provider().name(), static_cast<unsigned>(my_id));
    } else {
        // Find the game, join it, then make this machine's player.
        const auto until = Clock::now() + std::chrono::seconds(o.room_seconds);
        auto last_ask = Clock::now() - std::chrono::seconds(10);
        while (session->sessions().empty() && Clock::now() < until) {
            if (Clock::now() - last_ask > std::chrono::seconds(2)) {
                session->find();
                last_ask = Clock::now();
            }
            session->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (session->sessions().empty()) {
            say("no game found");
            return 1;
        }
        const net::dplay::FoundSession fs = session->sessions().front();
        say("found \"%s\" (%u of %u players, words %08x %08x %08x %08x)", fs.desc.name.c_str(), fs.desc.current_players, fs.desc.max_players,
            fs.desc.user[0], fs.desc.user[1], fs.desc.user[2], fs.desc.user[3]);
        session->join(fs, o.password);
        while (session->state() == net::dplay::Session::State::Joining && Clock::now() < until) {
            session->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (session->state() != net::dplay::Session::State::Joined) {
            say("joining failed: %s", session->error().c_str());
            return 1;
        }
        say("joined; system player %08x; %zu players in the table", static_cast<unsigned>(session->system_id()), session->players().size());
        session->create_player(o.name, player_data(o.password));
        net::SessionEvent e;
        std::vector<net::SessionEvent> others;
        while (!my_id && Clock::now() < until) {
            session->pump();
            while (session->next_event(e)) {
                if (e.kind == net::SessionEvent::Kind::PlayerCreated && e.local) my_id = e.id;
                else others.push_back(e);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!my_id) {
            say("no player id");
            return 1;
        }
        say("player id %08x", static_cast<unsigned>(my_id));
    }
    for (const net::dplay::Player& p : session->players())
        say("  table: %08x flags %x machine %08x \"%s\"", static_cast<unsigned>(p.id), p.flags, static_cast<unsigned>(p.system_id), p.short_name.c_str());

    // The room.
    net::NetSession ns(*session);
    net::BattleRoom room(ns, host, my_id, o.name);
    {
        ta::UnitDefContext ctx;
        ta::load_unit_context(vfs, ctx);
        const ta::UnitDefTable defs = ta::load_unit_defs(vfs, ctx);
        room.set_unit_checks(ta::unit_sync_checks(vfs, defs));
    }
    room.map_checksum = [&vfs](const std::string& map) { return ta::map_sync_checksum(vfs, map); };
    room.on_chat = [](const std::string& l) { say("chat: %s", l.c_str()); };
    room.on_player_left = [](int s) { say("slot %d left", s); };
    if (host) {
        room.set_game_name(o.game);
        room.set_map(o.map, ta::map_sync_checksum(vfs, o.map));
        for (int k = 0; k < o.computers; ++k) room.add_computer(static_cast<std::uint8_t>(k & 1));
    }
    room.set_map_ready(true);
    room.set_screen(640, 480);
    sim::Lcg15 crt;
    crt.set_state(static_cast<std::uint32_t>(Clock::now().time_since_epoch().count()));
    const auto room_until = Clock::now() + std::chrono::seconds(o.room_seconds);
    bool said = false, readied = false;
    std::string roster;
    while (!room.loaded() && Clock::now() < room_until) {
        room.frame(clock30(), &crt);
        if (room.refused()) {
            say("out of the room: reason %d %s", room.out_reason(), room.error().c_str());
            return 1;
        }
        if (!host && room.local_slot() >= 0 && !readied) {
            readied = true;
            room.set_ready(true);
            say("seated in slot %d, ready", room.local_slot());
        }
        if (room.local_slot() >= 0 && !said && !o.say.empty()) {
            said = true;
            room.say(o.say);
        }
        std::string r;
        int humans = 0, ready = 0;
        for (int s = 0; s < 10; ++s) {
            const net::RoomPlayer& p = room.players()[static_cast<std::size_t>(s)];
            if (!p.net_id) continue;
            if (p.kind == 1) {
                ++humans;
                if (p.ready() || (p.flags & net::room::kHost)) ++ready;
            }
            char b[160];
            std::snprintf(b, sizeof b, "[%d %08x %s %s logo %d%s%s%s] ", s, static_cast<unsigned>(p.net_id), p.name.c_str(), p.side ? "CORE" : "ARM",
                          p.logo == 0xFF ? -1 : p.logo, p.kind == 2 ? " computer" : "", p.ready() ? " ready" : "", p.map_ready ? "" : " no-map");
            r += b;
        }
        if (r != roster) {
            roster = r;
            say("room: %s map \"%s\"", r.c_str(), room.map().c_str());
        }
        if (host && !room.started() && humans >= o.players && ready >= o.players && room.can_start()) {
            say("starting");
            room.start();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    if (!room.loaded()) {
        say("the game did not start");
        return 1;
    }
    say("loaded: map \"%s\", this machine slot %d", room.map().c_str(), room.local_slot());
    if (o.no_battle) return 0;

    // The battle.
    sim::QuietBattleOutside quiet;
    net::NetBattle nb(quiet, ns);
    ta::BattleRequest req;
    req.map = room.map();
    req.sides = room.sides();
    req.start = room.battle_start();
    const auto t = static_cast<std::uint32_t>(Clock::now().time_since_epoch().count());
    req.start.battle_rng_seed = t;
    req.start.loading_rand_seed = t ^ 0x5A5A5A5Au;
    req.start.crt_rand_state = crt.state();
    for (int s = 0; s < 10; ++s) {
        const sim::StartPlayer& sp = req.start.players[static_cast<std::size_t>(s)];
        if (sp.net_id) say("  player %d: %s %s position %d %s", s, sp.name.c_str(), req.sides[static_cast<std::size_t>(s)].c_str(), sp.start_position,
                           sp.kind == sim::PlayerKind::Remote ? "remote" : sp.kind == sim::PlayerKind::Computer ? "computer" : "local");
    }
    say("loading the battle");
    auto lb = ta::load_battle(vfs, req, nb, err, [&](sim::Battle& b) { nb.attach(b); });
    if (!lb) {
        say("load: %s", err.c_str());
        return 1;
    }
    say("battle loaded");
    sim::Battle& b = *lb->battle;
    ta::set_network_weapons(b, lb->weapons);
    nb.attach(b);
    nb.set_room(room.players());
    nb.flush_now();
    nb.on_chat = [](int slot, const std::string& text) { say("battle chat from %d: %s", slot, text.c_str()); };
    nb.on_player_left = [](int slot, int reason) { say("battle: slot %d left (%d)", slot, reason); };
    const int slot = room.local_slot();
    std::mt19937 rng(t);
    const std::uint32_t c0 = clock30();
    int step = 0;
    while (step < o.steps) {
        const std::uint32_t now = clock30();
        nb.set_clock(now);
        if (now - c0 <= static_cast<std::uint32_t>(step) || nb.remote_lag() > 900) {
            nb.network_flush();
            nb.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        give_orders(b, *lb, slot, rng);
        if (step == 60) nb.send_chat("hello from the engine");
        b.step();
        b.end_frame();
        ++step;
        if (step % 300 == 0) {
            int own = 0, remote = 0;
            for (std::size_t i = 1; i < b.units.size(); ++i)
                if (b.ticks[i].type_index && (b.units[i].flags & sim::unit_flags::kAlive)) (b.ticks[i].owner == slot ? own : remote)++;
            say("step %d: %d own units, %d units of other players, lag %d", step, own, remote, nb.remote_lag());
        }
        if (!session->connected()) {
            say("session lost at step %d", step);
            break;
        }
    }
    int own = 0, remote = 0;
    for (std::size_t i = 1; i < b.units.size(); ++i)
        if (b.ticks[i].type_index && (b.units[i].flags & sim::unit_flags::kAlive)) (b.ticks[i].owner == slot ? own : remote)++;
    const net::NetCounters& n = nb.counters();
    say("end: %d steps, %d own units, %d units of other players; %llu packets sent, %llu received; session: %llu game messages sent, %llu received",
        step, own, remote, static_cast<unsigned long long>(n.packets_sent), static_cast<unsigned long long>(n.packets_received),
        static_cast<unsigned long long>(session->counters().sent), static_cast<unsigned long long>(session->counters().received));
    std::printf("  packets by type:");
    for (std::size_t k = 0; k < n.by_type.size(); ++k)
        if (n.by_type[k]) std::printf(" %02zx:%llu", k, static_cast<unsigned long long>(n.by_type[k]));
    std::printf("\n");
    session->close();
    return remote > 0 ? 0 : 1;
}

// Prints the game packets inside logged messages: the capture tool's net_send / net_recv
// lines (the original's side) or this tool's "game from" log lines.
int decode_log(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return 1;
    char line[65536];
    while (std::fgets(line, sizeof line, f)) {
        std::string l = line;
        std::string hexs, what;
        std::size_t at = l.find("\"bytes\":\"");
        if (at != std::string::npos) {
            const std::size_t e = l.find('"', at + 9);
            hexs = l.substr(at + 9, e - at - 9);
            what = l.find("net_send") != std::string::npos ? "orig sends" : "orig gets ";
            const std::size_t fr = l.find("\"from\":"), to = l.find("\"to\":");
            what += " from " + l.substr(fr + 7, l.find(',', fr) - fr - 7) + " to " + l.substr(to + 5, l.find(',', to) - to - 5);
        } else if ((at = l.find("game from")) != std::string::npos) {
            const std::size_t b = l.find("bytes ", at);
            if (b == std::string::npos) continue;
            hexs = l.substr(b + 6);
            while (!hexs.empty() && (hexs.back() == '\n' || hexs.back() == '\r')) hexs.pop_back();
            if (hexs.size() < 16) continue;
            hexs = hexs.substr(16);  // the session's from and to
            what = l.substr(0, at + 32);
            while (!what.empty() && what.back() == ' ') what.pop_back();
        } else {
            continue;
        }
        std::vector<std::uint8_t> m;
        for (std::size_t i = 0; i + 1 < hexs.size(); i += 2) m.push_back(static_cast<std::uint8_t>(std::strtoul(hexs.substr(i, 2).c_str(), nullptr, 16)));
        std::vector<std::uint8_t> frame;
        const net::UnwrapResult r = net::unwrap(m.data(), m.size(), frame);
        std::printf("%s:", what.c_str());
        if (r != net::UnwrapResult::Ok && r != net::UnwrapResult::NotWrapped) {
            std::printf(" (bad message %d)\n", static_cast<int>(r));
            continue;
        }
        // A frame: 4-byte running number, then packets.
        std::size_t p = r == net::UnwrapResult::Ok ? 4 : 0;
        if (frame.size() >= 4 && r == net::UnwrapResult::Ok) std::printf(" [frame %d]", static_cast<std::int32_t>(net::get32(frame.data())));
        while (p < frame.size()) {
            const std::uint16_t n = net::packet_size_at(frame.data() + p, frame.size() - p);
            if (!n || p + n > frame.size()) {
                std::printf(" ?%02x(+%zu)", frame[p], frame.size() - p);
                break;
            }
            std::printf(" %02x", frame[p]);
            if (frame[p] == 0x1B || frame[p] == 0x18 || frame[p] == 0x17 || frame[p] == 0x26 || frame[p] == 0x1A || frame[p] == 0x15 || frame[p] == 0x1E ||
                frame[p] == 0x1F || frame[p] == 0x22 || frame[p] == 0x21 || frame[p] == 0x08) {
                std::printf("(");
                for (std::size_t k = 1; k < n && k < 42; ++k) std::printf("%02x", frame[p + k]);
                std::printf(")");
            }
            if (frame[p] == 0x05) std::printf("(\"%.*s\")", 64, reinterpret_cast<const char*>(frame.data() + p + 1));
            if (frame[p] == 0x09) {
                const std::uint8_t* u = frame.data() + p;
                std::printf("(type %u unit %u at %d %d %d)", net::get16(u + 1), net::get16(u + 3), static_cast<std::int32_t>(net::get32(u + 5)) >> 16,
                            static_cast<std::int32_t>(net::get32(u + 9)) >> 16, static_cast<std::int32_t>(net::get32(u + 13)) >> 16);
            }
            if (frame[p] == 0x20) {
                const std::uint8_t* rec = frame.data() + p + 1;
                std::printf("(map \"%.20s\" ready %u id %08x kind %u side %u logo %u flags %02x options %04x version %u.%u map sum %08x)",
                            reinterpret_cast<const char*>(rec), rec[0x8F], net::get32(rec + 0x90), rec[0x94], rec[0x95], rec[0x96], rec[0x97],
                            net::get16(rec + 0x9B), rec[0xA7], rec[0xA8], net::get32(rec + 0xA9));
            }
            p += n;
        }
        std::printf("\n");
    }
    std::fclose(f);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "decode") return decode_log(argv[2]);
    if (argc == 4 && std::string(argv[2]) == "mapsum") {
        ta::Vfs vfs;
        vfs.mount_game_dir(argv[1]);
        std::printf("%08x\n", ta::map_sync_checksum(vfs, argv[3]));
        return 0;
    }
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: dplay_play <game_dir> host [options]\n"
                     "       dplay_play <game_dir> join <address> [options]\n"
                     "       dplay_play <game_dir> serial <port> host|join [options]\n"
                     "       dplay_play <game_dir> modem <port> dial <number>|answer [options]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options o;
    o.mode = argv[2];
    int i = 3;
    if (o.mode == "join" && i < argc) o.address = argv[i++];
    if ((o.mode == "serial" || o.mode == "modem") && i + 1 < argc) {
        o.serial_port = argv[i++];
        o.serial_role = argv[i++];
        if (o.serial_role == "dial" && i < argc) o.phone = argv[i++];
    }
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--loopback") o.loopback = true;
        else if (a == "--name") o.name = next();
        else if (a == "--game") o.game = next();
        else if (a == "--map") o.map = next();
        else if (a == "--password") o.password = next();
        else if (a == "--players") o.players = std::atoi(next().c_str());
        else if (a == "--computer") o.computers = std::atoi(next().c_str());
        else if (a == "--steps") o.steps = std::atoi(next().c_str());
        else if (a == "--room") o.room_seconds = std::atoi(next().c_str());
        else if (a == "--log") o.log = next();
        else if (a == "--no-battle") o.no_battle = true;
        else if (a == "--say") o.say = next();
        else if (a == "--stream-port") o.stream_port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (a == "--datagram-port") o.datagram_port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    if (!o.log.empty()) g_log = std::fopen(o.log.c_str(), "w");
    ta::Vfs vfs;
    vfs.mount_game_dir(argv[1]);
    const int r = run(vfs, o);
    if (g_log) std::fclose(g_log);
    return r;
}
