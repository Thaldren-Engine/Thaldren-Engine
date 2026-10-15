#include "client/net_menu.h"

#include <algorithm>
#include <chrono>
#include <random>

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
    RoomScreen(const NetMenuConfig& cfg, std::unique_ptr<net::Transport> t, bool host)
        : cfg_(cfg), transport_(std::move(t)), session_(std::make_shared<net::NetSession>(*transport_)),
          room_(std::make_unique<net::BattleRoom>(*session_, host, make_net_id(), cfg.player_name)) {
        // This machine's C runtime rand(): the host's start position shuffle draws it (an input
        // from the clock, as the menus leave it).
        crt_rand_.set_state(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (cfg_.unit_checks) room_->set_unit_checks(cfg_.unit_checks());
        room_->set_map_ready(true);
        room_->on_chat = [this](const std::string& l) { push(l); };
        room_->on_player_left = [this](int s) { push("Player " + std::to_string(s + 1) + " left"); };
    }
    void enter(FrontEnd& fe) override {
        if (launched_) fe.pop();  // back from the game
    }
    void handle(FrontEnd& fe, const InputEvent& e) override {
        if (!room_) return;  // handed to the game
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
        if (!room_) return;
        room_->frame(now_clock(now_ms), &crt_rand_);
        if (room_->refused() && !launched_) push(room_->error());
        if (room_->loaded() && !launched_) launch(fe);
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
    void launch(FrontEnd& fe) {
        launched_ = true;
        auto req = std::make_shared<NetGameRequest>();
        req->map = room_->map();
        req->sides = room_->sides();
        req->start = room_->battle_start();
        // The original seeds its generators from its clocks (inputs).
        const auto t = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        req->start.battle_rng_seed = t;
        req->start.loading_rand_seed = t ^ 0x5A5A5A5Au;
        req->start.crt_rand_state = crt_rand_.state();
        req->settings = &fe.settings();
        auto transport = std::make_shared<std::unique_ptr<net::Transport>>(std::move(transport_));
        auto session = session_;
        auto net = std::make_shared<std::unique_ptr<net::NetBattle>>();
        const auto players = room_->players();
        req->wrap_outside = [session, net](sim::BattleOutside& client_outside) -> sim::BattleOutside& {
            *net = std::make_unique<net::NetBattle>(client_outside, *session);
            return **net;
        };
        req->attach = [net, players](sim::Battle& b) {
            (*net)->attach(b);
            (*net)->set_room(players);
        };
        const NetMenuConfig cfg = cfg_;
        room_.reset();  // the room's transport now belongs to the game
        fe.start_battle([cfg, req, transport, session, net](const LoadProgress& progress, std::string& error) -> std::unique_ptr<BattleSession> {
            if (!cfg.load_network_game) {
                error = "network games need the importer's loader";
                return nullptr;
            }
            std::unique_ptr<BattleSession> inner = cfg.load_network_game(*req, progress, error);
            if (!inner || !*net) return nullptr;
            return std::make_unique<NetBattleSession>(std::move(*transport), session, std::move(*net), std::move(inner));
        });
    }

    NetMenuConfig cfg_;
    std::unique_ptr<net::Transport> transport_;
    std::shared_ptr<net::NetSession> session_;
    std::unique_ptr<net::BattleRoom> room_;
    Box side_, colour_, team_, ready_, computer_, start_, back_;
    sim::Lcg15 crt_rand_;
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

void install_multiplayer(FrontEndHooks& hooks, const NetMenuConfig& config) {
    hooks.multiplayer_menu = [config](FrontEnd& fe) { return make_multiplayer_menu(fe, config); };
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
    net_->on_chat = [this](int slot, const std::string& text) {
        inner_->client().message("Player " + std::to_string(slot + 1) + ": " + text, true);
    };
    net_->on_player_left = [this](int slot, int) { inner_->client().message("Player " + std::to_string(slot + 1) + " left", true); };
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

void NetBattleSession::send_chat(const std::string& text) {
    net_->send_chat(text);
    inner_->client().message(text);
}

void NetBattleSession::surrender() {
    inner_->surrender();
}

}  // namespace client
