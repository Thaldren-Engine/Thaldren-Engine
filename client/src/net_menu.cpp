#include "client/net_menu.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

#include "client/canvas.h"
#include "client/menu.h"

namespace client {
namespace {

std::uint32_t now_clock(std::uint32_t now_ms) { return now_ms * 30u / 1000u; }

// A clickable box of a plain screen.
struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    std::string label;
    bool hit(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

void draw_box(FrontEnd& fe, render::Image8& out, const Box& b, bool on = false) {
    const MenuContent& mc = fe.menu_content();
    const render::ClipRect clip{0, 0, out.width - 1, out.height - 1};
    fill_rect(out, clip, b.x, b.y, b.x + b.w - 1, b.y + b.h - 1, on ? mc.text_highlight : mc.box_colour);
    outline_rect(out, clip, b.x, b.y, b.x + b.w - 1, b.y + b.h - 1, mc.frame_colour);
    if (mc.font) draw_text(out, clip, *mc.font, b.label, b.x + 6, b.y + 4, mc.text_colour, -1, b.w - 8);
}

void draw_line(FrontEnd& fe, render::Image8& out, const std::string& text, int x, int y, bool dim = false) {
    const MenuContent& mc = fe.menu_content();
    if (!mc.font) return;
    draw_text(out, render::ClipRect{0, 0, out.width - 1, out.height - 1}, *mc.font, text, x, y, dim ? mc.text_dim : mc.text_colour);
}

void background(FrontEnd& fe, render::Image8& out, const char* layout) {
    const MenuContent& mc = fe.menu_content();
    const ScreenLayout* l = mc.layout ? mc.layout(layout) : nullptr;
    std::fill(out.pixels.begin(), out.pixels.end(), static_cast<std::uint8_t>(0));
    if (l && l->background) {
        const render::Sprite8& s = *l->background;
        for (int y = 0; y < s.height && y < out.height; ++y)
            for (int x = 0; x < s.width && x < out.width; ++x) out.row(y)[x] = s.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(s.width) + static_cast<std::size_t>(x)];
    }
}

std::int32_t make_net_id() {
    // The session gives every player an id; ours are random nonzero values.
    std::random_device rd;
    std::int32_t id = 0;
    while (id == 0 || id == -1) id = static_cast<std::int32_t>(rd() & 0x7FFFFFFF);
    return id;
}

class RoomScreen final : public Screen {
public:
    // `net_id`: the player's id in the transport's session (0: one of our own, chosen here).
    RoomScreen(const NetMenuConfig& cfg, std::unique_ptr<net::Transport> t, bool host, std::int32_t net_id = 0, const std::string& game_name = {})
        : cfg_(cfg), transport_(std::move(t)), session_(std::make_shared<net::NetSession>(*transport_)),
          room_(std::make_shared<net::BattleRoom>(*session_, host, net_id ? net_id : make_net_id(), cfg.player_name)),
          crt_rand_(std::make_shared<sim::Lcg15>()) {
        if (host && !game_name.empty()) room_->set_game_name(game_name);
        // This machine's C runtime rand(): the host's start position shuffle draws it (an input
        // from the clock, as the menus leave it).
        crt_rand_->set_state(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (cfg_.unit_checks) room_->set_unit_checks(cfg_.unit_checks());
        room_->set_map_ready(true);
        room_->defer_loading_handshake(true);  // the handshake runs once the data is loaded
        room_->on_chat = [this](const std::string& l) { push(l); };
        room_->on_player_left = [this](int s) { push("Player " + std::to_string(s + 1) + " left"); };
    }
    void enter(FrontEnd& fe) override {
        if (launched_) fe.pop();  // back from the game
    }
    void handle(FrontEnd& fe, const InputEvent& e) override {
        if (!room_ || launched_) return;  // handed to the game
        if (e.type == InputEvent::Type::KeyDown) {
            if (e.key == keys::kEscape) {
                fe.pop();
            } else if (e.key == keys::kEnter) {
                if (typed_.rfind("/map ", 0) == 0) room_->set_map(typed_.substr(5));
                else if (!typed_.empty()) {
                    room_->say(typed_);
                    push(cfg_.player_name + ": " + typed_);
                }
                typed_.clear();
            } else if (e.key == 8) {
                if (!typed_.empty()) typed_.pop_back();
            } else if (e.key >= 32 && e.key < 127 && typed_.size() < 60) {
                typed_.push_back(static_cast<char>(e.key));
            }
            return;
        }
        if (e.type != InputEvent::Type::MouseDown || e.button != 1) return;
        fe.play_sound(fe.menu_content().click_sound);
        if (side_.hit(e.x, e.y)) room_->set_side(static_cast<std::uint8_t>(++side_value_ & 1));
        else if (colour_.hit(e.x, e.y)) room_->next_logo();
        else if (team_.hit(e.x, e.y)) room_->next_ally_team();
        else if (ready_.hit(e.x, e.y)) room_->set_ready(ready_value_ = !ready_value_);
        else if (computer_.hit(e.x, e.y) && room_->is_host()) room_->add_computer(1);
        else if (start_.hit(e.x, e.y) && room_->is_host()) {
            if (!room_->start()) push(room_->error());
        } else if (back_.hit(e.x, e.y)) {
            fe.pop();
        }
    }
    void frame(FrontEnd& fe, std::uint32_t now_ms) override {
        if (!room_ || launched_) return;
        room_->frame(now_clock(now_ms), crt_rand_.get());
        if (room_->refused() && !launched_) push(room_->error());
        if (room_->started() && !launched_) {
            launched_ = true;
            launch_network_battle(fe, cfg_, NetLaunch{std::move(transport_), session_, room_, crt_rand_});
        }
    }
    void draw(FrontEnd& fe, render::Image8& out) override {
        background(fe, out, "BATTLROOM");
        if (!room_) return;
        draw_line(fe, out, std::string(room_->is_host() ? "Battle room (host)" : "Battle room") + "  -  map " + room_->map(), 20, 16);
        int y = 44;
        for (int s = 0; s < 10; ++s) {
            const net::RoomPlayer& p = room_->players()[static_cast<std::size_t>(s)];
            if (!p.net_id) continue;
            std::string line = std::to_string(s + 1) + ". " + p.name + "  " + (p.side ? "CORE" : "ARM") + "  logo " +
                               std::to_string(p.logo) + (p.ally_team < 5 ? "  team " + std::to_string(p.ally_team + 1) : "") + (p.kind == 2 ? "  computer" : "") + (p.ready() || p.kind == 2 || (p.flags & net::room::kHost) ? "  ready" : "") +
                               (p.latency_ms ? "  " + std::to_string(p.latency_ms) + " ms" : "");
            draw_line(fe, out, line, 20, y, s != room_->local_slot());
            y += 16;
        }
        const int by = out.height - 120;
        side_ = {20, by, 80, 20, "Side"};
        colour_ = {105, by, 80, 20, "Colour"};
        team_ = {190, by, 80, 20, "Team"};
        ready_ = {275, by, 80, 20, ready_value_ ? "Ready!" : "Ready"};
        computer_ = {360, by, 95, 20, "Computer"};
        start_ = {460, by, 80, 20, "Start"};
        back_ = {545, by, 75, 20, "Back"};
        for (Box* b : {&side_, &colour_, &team_, &ready_, &back_}) draw_box(fe, out, *b, b == &ready_ && ready_value_);
        if (room_->is_host()) {
            draw_box(fe, out, computer_);
            draw_box(fe, out, start_, room_->can_start());
        }
        int cy = out.height - 90;
        for (const std::string& l : lines_) {
            draw_line(fe, out, l, 20, cy, true);
            cy += 14;
        }
        draw_line(fe, out, "> " + typed_, 20, out.height - 22);
    }

private:
    void push(const std::string& l) {
        lines_.push_back(l);
        if (lines_.size() > 4) lines_.erase(lines_.begin());
    }

    NetMenuConfig cfg_;
    std::unique_ptr<net::Transport> transport_;
    std::shared_ptr<net::NetSession> session_;
    std::shared_ptr<net::BattleRoom> room_;
    std::shared_ptr<sim::Lcg15> crt_rand_;
    Box side_, colour_, team_, ready_, computer_, start_, back_;
    int side_value_ = 0, colour_value_ = 0;
    bool ready_value_ = false, launched_ = false;
    std::string typed_;
    std::vector<std::string> lines_;
};

class ConnectScreen final : public Screen {
public:
    explicit ConnectScreen(const NetMenuConfig& cfg) : cfg_(cfg) {}
    // The player's name from the user's settings (the options' nickname).
    NetMenuConfig with_name(FrontEnd& fe) const {
        NetMenuConfig c = cfg_;
        c.player_name = fe.settings().get_text("Nickname", cfg_.player_name);
        return c;
    }
    void handle(FrontEnd& fe, const InputEvent& e) override {
        if (e.type == InputEvent::Type::KeyDown) {
            if (e.key == keys::kEscape) fe.pop();
            else if (e.key == keys::kEnter) join(fe);
            else if (e.key == 8) {
                if (!address_.empty()) address_.pop_back();
            } else if (e.key >= 32 && e.key < 127 && address_.size() < 60) address_.push_back(static_cast<char>(e.key));
            return;
        }
        if (e.type != InputEvent::Type::MouseDown || e.button != 1) return;
        fe.play_sound(fe.menu_content().click_sound);
        if (host_.hit(e.x, e.y)) {
            std::string err;
            auto t = net::tcp_host(cfg_.port, err);
            if (!t) message_ = "cannot host: " + err;
            else fe.push(std::make_unique<RoomScreen>(with_name(fe), std::move(t), true));
        } else if (join_.hit(e.x, e.y)) {
            join(fe);
        } else if (back_.hit(e.x, e.y)) {
            fe.pop();
        }
    }
    void draw(FrontEnd& fe, render::Image8& out) override {
        background(fe, out, "SELPROV");
        draw_line(fe, out, "Multiplayer (TCP/IP)", 20, 16);
        draw_line(fe, out, "Address of the host: " + address_ + "_", 20, 60);
        draw_line(fe, out, "Port " + std::to_string(cfg_.port), 20, 80, true);
        if (!message_.empty()) draw_line(fe, out, message_, 20, 110);
        host_ = {20, 140, 120, 22, "Host a game"};
        join_ = {160, 140, 120, 22, "Join"};
        back_ = {300, 140, 80, 22, "Back"};
        for (const Box* b : {&host_, &join_, &back_}) draw_box(fe, out, *b);
    }

private:
    void join(FrontEnd& fe) {
        std::string err;
        auto t = net::tcp_join(address_.empty() ? "127.0.0.1" : address_, cfg_.port, err);
        if (!t) message_ = "cannot join: " + err;
        else fe.push(std::make_unique<RoomScreen>(with_name(fe), std::move(t), false));
    }
    NetMenuConfig cfg_;
    std::string address_, message_;
    Box host_, join_, back_;
};

}  // namespace

std::unique_ptr<Screen> make_multiplayer_menu(FrontEnd&, const NetMenuConfig& config) { return std::make_unique<ConnectScreen>(config); }

std::unique_ptr<Screen> make_room_screen(const NetMenuConfig& config, std::unique_ptr<net::Transport> transport, bool host, std::int32_t net_id,
                                         const std::string& game_name) {
    return std::make_unique<RoomScreen>(config, std::move(transport), host, net_id, game_name);
}

void install_multiplayer(FrontEndHooks& hooks, const NetMenuConfig& config) {
    hooks.multiplayer_menu = [config](FrontEnd& fe) { return make_multiplayer_menu(fe, config); };
}

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
    net_->on_pause = [this](bool p) { inner_->client().set_paused(p); };
    net_->on_speed = [this](int s) { inner_->client().set_game_speed(s); };
    // A received line is shown as it came (its sender put its name in front), in colour 8 with
    // the sender's record (0x453D40: the console line beeps MessageArrived).
    net_->on_chat = [this](int slot, const std::string& text) {
        inner_->client().message(text, GameClient::kPlayerNote, static_cast<std::uint8_t>(slot));
    };
    net_->on_player_left = [this](int slot, int) {
        inner_->client().message("Player " + std::to_string(slot + 1) + " left", GameClient::kPlayerNote, static_cast<std::uint8_t>(slot));
    };
}

int NetBattleSession::frame(std::uint32_t now_ms) {
    net_->set_clock(now_clock(now_ms));
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
    int recipients = 0;
    std::array<bool, 10> chosen{};
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

}  // namespace client
