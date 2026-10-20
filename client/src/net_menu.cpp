#include "client/net_menu.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace client {
namespace {

std::uint32_t now_clock(std::uint32_t now_ms) { return now_ms * 30u / 1000u; }

}  // namespace

std::string chat_line(const std::string& name, const std::string& to, const std::string& text) {
    return "<" + name + (to.empty() ? std::string() : "->" + to) + "> " + text;
}

void launch_network_battle(FrontEnd& fe, const NetMenuConfig& cfg, NetLaunch L) {
    using clock = std::chrono::steady_clock;
    std::shared_ptr<net::BattleRoom> room = L.room;
    std::shared_ptr<sim::Lcg15> crt = L.crt_rand ? L.crt_rand : std::make_shared<sim::Lcg15>();
    auto req = std::make_shared<NetGameRequest>();
    req->map = room->map();
    req->sides = room->sides();
    req->start = room->battle_start();  // (the start positions come with the handshake)
    // The original seeds its generators from its clocks (inputs).
    const auto t = static_cast<std::uint32_t>(clock::now().time_since_epoch().count());
    req->start.battle_rng_seed = t;
    req->start.loading_rand_seed = t ^ 0x5A5A5A5Au;
    req->start.crt_rand_state = crt->state();
    req->settings = &fe.settings();
    // The unit definitions' rules as the exchange left them (0x46E160 at the battle start).
    for (const auto& [key, sum] : room->unit_checks()) req->unit_rules[key] = {room->unit_enabled(key), room->unit_limit(key)};
    {  // the host's record decides (the original reads the host's option word, bit 13)
        const auto& ps = room->players();
        const net::RoomPlayer* host = &ps[0];
        for (const auto& p : ps)
            if (p.net_id && (p.flags & net::room::kHost)) host = &p;
        req->cheats = (host->options & 0x2000u) != 0;
    }
    auto transport = std::make_shared<std::unique_ptr<net::Transport>>(std::move(L.transport));
    std::shared_ptr<net::NetSession> session = L.session;
    auto net = std::make_shared<std::unique_ptr<net::NetBattle>>();
    req->wrap_outside = [session, net](sim::BattleOutside& client_outside) -> sim::BattleOutside& {
        *net = std::make_unique<net::NetBattle>(client_outside, *session);
        return **net;
    };
    req->attach = [net, room](sim::Battle& b) {
        (*net)->attach(b);
        (*net)->set_room(room->players());
    };
    const bool host = room->is_host();
    const int local = room->local_slot();
    FrontEnd* fep = &fe;
    const auto t0 = clock::now();
    const std::uint32_t base = room->clock();
    auto now_ticks = [t0, base]() {
        return base + 1u + static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count() * 30 / 1000);
    };
    // The loading screen's network part (0x497F40): every player in the game (slot order) with a
    // bar; this machine's players (its human, and the computer players a host runs) show this
    // machine's progress, the others what their packets report; "has loaded" is set at the
    // start for this machine's players, by packets for the others.
    auto shown = std::make_shared<std::string>();
    auto show_players = [room, fep, host, local, shown](bool synchronised) {
        LoadingState* ls = fep->loading();
        if (!ls) return;
        struct Report {  // (the bars as a log line whenever they change)
            std::shared_ptr<std::string> last;
            LoadingState* ls;
            ~Report() {
                std::string line = ls->synchronised ? "synchronised" : "";
                for (const LoadingPlayer& p : ls->players)
                    line += " | " + p.name + " " + std::to_string(p.percent) + "%" + (p.local ? " local" : "") + (p.loaded ? " loaded" : "");
                if (line != *last) std::printf("loading bars:%s\n", line.c_str());
                *last = line;
            }
        } report{shown, ls};
        ls->network = true;
        ls->synchronised = synchronised;
        ls->players.clear();
        int sum = 0;
        for (int r : ls->rows) sum += r;
        const auto& ps = room->players();
        for (std::size_t k = 0; k < ps.size(); ++k) {
            const net::RoomPlayer& p = ps[k];
            if (!p.net_id) continue;
            LoadingPlayer lp;
            lp.name = p.name;
            lp.local = static_cast<int>(k) == local || (p.kind == 2 && host);
            lp.percent = lp.local ? sum / kLoadRows : p.load_percent;
            lp.loaded = lp.local || room->heard_loaded(static_cast<int>(k));
            ls->players.push_back(lp);
        }
    };
    // One frame of the loading screen's network part: this machine's progress goes out (0x2A,
    // with a probe) and the network is read (other machines' progress, start positions, "loaded").
    auto net_frame = [room, crt, fep, now_ticks]() {
        int sum = 0;
        if (LoadingState* ls = fep->loading())
            for (int r : ls->rows) sum += r;
        room->frame(now_ticks(), crt.get());
        room->send_load_progress(static_cast<std::uint8_t>(std::clamp(sum / kLoadRows, 0, 255)));
    };
    std::array<std::string, 10> names{};
    for (std::size_t k = 0; k < names.size(); ++k) names[k] = room->players()[k].name;
    fe.start_battle([cfg, req, transport, session, net, room, crt, show_players, net_frame, now_ticks, names, local](
                        const LoadProgress& progress, std::string& error) -> std::unique_ptr<BattleSession> {
        show_players(false);
        if (!cfg.load_network_game) {
            error = "network games need the importer's loader";
            return nullptr;
        }
        // Each redraw of the loading screen (at most every 200 ms) is a frame of its network part.
        auto last = std::make_shared<clock::time_point>(clock::now() - std::chrono::seconds(1));
        auto frame_due = [last]() {
            if (clock::now() - *last < std::chrono::milliseconds(kLoadRedrawMs)) return false;
            *last = clock::now();
            return true;
        };
        const LoadProgress net_progress = [progress, net_frame, show_players, frame_due](float f, const std::string& stage) {
            if (frame_due()) {
                net_frame();
                show_players(false);
            }
            progress(f, stage);
        };
        // Once the data is loaded: the handshake, each frame until everybody has loaded
        // (0x4568C0; the loading screen goes on meanwhile).
        req->before_start = [room, crt, progress, net_frame, show_players, frame_due, now_ticks](sim::BattleStart& start) {
            room->data_loaded();
            const auto limit = clock::now() + std::chrono::minutes(5);
            while (!room->loaded() && !room->refused() && clock::now() < limit) {
                // One pass a loading-screen frame (the original draws one, then sleeps 200 ms):
                // its "loaded" (0x15) and progress (0x2A) go out once a frame, not more.
                if (frame_due()) {
                    net_frame();
                    show_players(false);
                    progress(1.0f, "network");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            show_players(room->loaded());
            progress(1.0f, "network");
            const sim::BattleStart b = room->battle_start();
            for (std::size_t k = 0; k < start.players.size(); ++k) start.players[k].start_position = b.players[k].start_position;
            start.crt_rand_state = crt->state();  // the host's shuffle drew this machine's rand()
        };
        std::unique_ptr<BattleSession> inner = cfg.load_network_game(*req, net_progress, error);
        if (!inner || !*net) return nullptr;
        auto s = std::make_unique<NetBattleSession>(std::move(*transport), session, std::move(*net), std::move(inner));
        s->set_names(names, local);
        s->set_timeout_seconds(cfg.timeout_seconds);  // (-t)
        return s;
    });
}

NetBattleSession::NetBattleSession(std::unique_ptr<net::Transport> transport, std::shared_ptr<net::NetSession> session,
                                   std::unique_ptr<net::NetBattle> net, std::unique_ptr<BattleSession> inner)
    : transport_(std::move(transport)), session_(std::move(session)), net_(std::move(net)), inner_(std::move(inner)) {
    // The client's step budget waits for the slowest remote player; its pause and speed keys
    // send packet 0x19 and received ones apply here.
    GameClient& c = inner_->client();
    net::NetBattle* n = net_.get();
    c.network.remote_lag = [n] { return n->remote_lag(); };
    c.network.send_pause = [n](bool p) { n->send_pause(p); };
    c.network.send_speed = [n](int s) { n->send_speed(s); };
    // "+compression" (GS+0x4ED) switches the frames' compression; "+drop" the time-out check.
    c.network.set_compression_off = [n](bool off) {
        net::NetTiming t = n->sender().timing();
        t.compression = !off;
        n->set_timing(t);
    };
    c.network.set_timeout_off = [n](bool off) { n->set_timeout(n->timeout_seconds(), off); };
    // The share commands: the local side record's flag word toggled and announced (0x450F90).
    c.network.toggle_share_flag = [this](std::uint8_t bit) {
        if (local_ < 0 || local_ > 9) return;
        net::RoomPlayer r = net_->room()[static_cast<std::size_t>(local_)];
        r.flags = static_cast<std::uint8_t>(r.flags ^ bit);
        net_->set_local_record(local_, r);
    };
    net_->on_pause = [this](bool p) { inner_->client().set_paused(p); };
    net_->on_speed = [this](int s) { inner_->client().set_game_speed(s); };
    // A received line is shown as it came (its sender put its name in front), in colour 8 with
    // the sender's record (0x453D40: the console line beeps MessageArrived).
    net_->on_chat = [this](int slot, const std::string& text) {
        inner_->client().message(text, GameClient::kPlayerNote, static_cast<std::uint8_t>(slot));
    };
    // A silent remote player past the time-out (0x453C20, -1: nobody): the time-out dialog.
    net_->on_timeout = [this](int slot) { timed_out_ = slot; };
    // The original's time-out: 30 seconds (the command line's -t gives 30..300, 0x49EE30).
    if (net_->timeout_seconds() == 0) net_->set_timeout(30);
    // The dropped messages draw the program's one rand(): the battle's while it runs.
    session_->sender.set_rand([this]() { return static_cast<std::int32_t>(inner_->battle().crt_rand.next()); });
    c.network.set_send_error = [this](int percent) { session_->sender.set_send_error(percent); };
    net_->on_player_left = [this](int slot, int) {
        inner_->client().message("Player " + std::to_string(slot + 1) + " left", GameClient::kPlayerNote, static_cast<std::uint8_t>(slot));
    };
}

int NetBattleSession::frame(std::uint32_t now_ms) {
    net_->set_clock(now_clock(now_ms));
    inner_->client().set_net_totals(net_->counters().bytes_sent, net_->counters().bytes_received);
    if (net_->paused()) {
        // The original's frame loop while paused: flush, read the network (to hear the unpause).
        net_->network_flush();
        net_->poll();
        return 0;
    }
    return inner_->frame(now_ms);
}

bool NetBattleSession::end_exchange_done(std::uint32_t now_ms) {
    // 0x499200 once the game ends: each pass asks for the missing final totals (0x4572A0) and
    // then sleeps 250 ms; the statistics follow when nothing was asked.
    if (exchanged_) return true;
    if (exchange_started_ && now_ms - exchange_ms_ < 250) return false;
    exchange_started_ = true;
    exchange_ms_ = now_ms;
    net_->set_clock(now_clock(now_ms));
    net_->poll();
    exchanged_ = net_->end_exchange();
    return exchanged_;
}

void NetBattleSession::send_chat(const std::string& line) {
    // The talk line (0x493BF0): a line whose second character is ',', ':' or ';' names its
    // receivers by its first: a digit one player (an unused record, id -1: nobody gets it and its
    // empty name shows; the original drops the line only for an id of 0, which no record of a
    // network game has), 'a' / 'A' the allies, 'e' / 'E' the enemies; the two characters go. The line is
    // then made "<name->to> text" (0x463E50), sent to those receivers and shown in colour 4
    // without a sender.
    std::string text = line, to;
    // (without a prefix: the recipients chosen on TALK2, GS+0x2BF0 / +0x2BF1)
    int recipients = chat_mode;
    std::array<bool, 10> chosen = chat_chosen;
    chat_panel = false;  // (0x493BF0: GS+0x2BEE bit 0x100 cleared once a line went)
    if (text.size() >= 2 && (text[1] == ',' || text[1] == ':' || text[1] == ';')) {
        const char c = text[0];
        if (c >= '0' && c <= '9') {
            const int d = c - '0';
            recipients = 3;
            chosen[static_cast<std::size_t>(d)] = true;
            to = names_[static_cast<std::size_t>(d)];
            text = text.substr(2);
        } else if (c == 'a' || c == 'A') {
            recipients = 1;
            to = "Allies";
            text = text.substr(2);
        } else if (c == 'e' || c == 'E') {
            recipients = 2;
            to = "Enemies";
            text = text.substr(2);
        }
    }
    const std::string out = chat_line(names_[static_cast<std::size_t>(local_ < 0 || local_ > 9 ? 0 : local_)], to, text);
    // (0x453360 sends a line starting with '+' to everybody; a made line starts with '<'.)
    net_->send_chat(out, recipients, chosen);
    inner_->client().message(out, GameClient::kChat);
}

void NetBattleSession::surrender() {
    inner_->surrender();
}

void NetBattleSession::leave_game() {
    // 0x451B60 at exit to Windows: for every player this machine runs (controller 1 or 2) its
    // whole team is killed (0x452CC0 -> 0x486F10; the deaths go out as usual) and the player
    // leaves the session; what was queued is sent first.
    sim::Battle& b = inner_->battle();
    std::vector<std::int32_t> ids;
    for (int k = 0; k < 10 && static_cast<std::size_t>(k) < b.players.size(); ++k) {
        const sim::PlayerSlot& p = b.players[static_cast<std::size_t>(k)];
        if (!p.active || (p.controller != 1 && p.controller != 2)) continue;
        b.kill_team(k);
        const std::int32_t id = net_->room()[static_cast<std::size_t>(k)].net_id;
        if (id) ids.push_back(id);
    }
    net_->flush_all();
    for (std::int32_t id : ids) transport_->destroy_player(id);
}

}  // namespace client
