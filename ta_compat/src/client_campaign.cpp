// The campaign and saved games in the front end (ta/client_campaign.h). Built when the front
// end's screens (ta/client_menus.h) are present.

#include "ta/client_campaign.h"

std::string ta::campaign_progress_key(const std::string& campaign) { return "Campaign " + campaign; }

#if __has_include("ta/client_menus.h")

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "client/audio.h"
#include "client/canvas.h"
#include "client/front_end.h"
#include "client/menu.h"
#include "sim/journal.h"
#include "sim/lockstep.h"
#include "ta/campaign.h"
#include "ta/client_menus.h"

namespace ta {

namespace {

std::uint32_t clock_value() {
    // The original seeds a battle from the clock; any fresh value will do.
    return static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

// --- Drawing helpers on the menu content's font and colours.
struct Pen {
    client::FrontEnd& fe;
    render::Image8& out;
    render::ClipRect clip;
    Pen(client::FrontEnd& f, render::Image8& o) : fe(f), out(o), clip{0, 0, o.width - 1, o.height - 1} {}
    const client::Font* font() const { return fe.menu_content().font; }
    int line() const { return font() ? font()->height + 3 : 12; }
    void clear() { client::fill_rect(out, clip, 0, 0, out.width - 1, out.height - 1, fe.menu_content().box_colour); }
    void text(const std::string& s, int x, int y, std::uint8_t colour, int max_width = -1) {
        if (font()) client::draw_text(out, clip, *font(), s, x, y, colour, -1, max_width);
    }
    void row(const std::string& s, int x, int y, int w, bool selected) {
        const client::MenuContent& m = fe.menu_content();
        if (selected) client::fill_rect(out, clip, x - 2, y - 1, x + w, y + line() - 3, m.frame_colour);
        text(s, x, y, selected ? m.text_highlight : m.text_colour, w);
    }
};

std::vector<std::string> wrap(const std::string& text, const client::Font* font, int width) {
    std::vector<std::string> lines;
    std::string cur, word;
    auto w = [&](const std::string& s) { return font ? font->text_width(s) : static_cast<int>(s.size()) * 8; };
    auto flush_word = [&] {
        if (word.empty()) return;
        const std::string tryline = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && w(tryline) > width) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = tryline;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') {
            flush_word();
            lines.push_back(cur);
            cur.clear();
        } else if (c == ' ' || c == '\t') {
            flush_word();
        } else {
            word.push_back(c);
        }
    }
    flush_word();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

std::string format_time(std::uint32_t steps) {
    const std::uint32_t s = steps / 30;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    return buf;
}

// Replays a save's journal onto a freshly started session's battle and checks the state.
bool replay_save(sim::Battle& b, const SavedGame& save, std::string& error) {
    sim::JournalPlayer player(save.journal);
    if (!player.run_to(b, save.tick)) {
        error = "the replay differs from the recording at step " + std::to_string(player.mismatch_tick);
        return false;
    }
    player.apply_rest(b);
    if (sim::state_checksum(b) != save.checksum || b.battle_rng().state() != save.battle_rng || b.crt_rand.state() != save.crt_rand) {
        error = "the replayed state differs from the saved one";
        return false;
    }
    return true;
}

class BriefingScreen;

// A mission battle: the front end's battle session with the campaign's progression.
class MissionSession : public client::BattleSession, public JournaledSession {
public:
    MissionSession(std::unique_ptr<SkirmishSession> inner, MenuArt& art, std::string save_dir, std::string campaign, int index,
                   MissionRequest rq)
        : inner_(std::move(inner)), art_(art), save_dir_(std::move(save_dir)), campaign_(std::move(campaign)), index_(index), rq_(rq) {}
    sim::Battle& battle() override { return inner_->battle(); }
    client::Content& content() override { return inner_->content(); }
    client::GameClient& client() override { return inner_->client(); }
    int frame(std::uint32_t now_ms) override { return inner_->frame(now_ms); }
    client::BattleOutcome outcome() override { return inner_->outcome(); }
    void send_chat(const std::string& t) override { inner_->send_chat(t); }
    void surrender() override { inner_->surrender(); }
    std::vector<client::PlayerStats> stats() override { return inner_->stats(); }
    bool after_battle(client::FrontEnd& fe, client::BattleOutcome outcome) override;
    bool make_save(SavedGame&, std::string& why) override {
        why = "this battle's inputs are not journaled (the client gives its orders directly)";
        return false;
    }

private:
    std::unique_ptr<SkirmishSession> inner_;
    MenuArt& art_;
    std::string save_dir_, campaign_;
    int index_;
    MissionRequest rq_;
};

client::BattleLauncher mission_launcher(MenuArt& art, const std::string& save_dir, const std::string& campaign, int index, const MissionInfo& info,
                                        MissionRequest rq, client::FrontEnd& fe, const SavedGame* save) {
    const client::ClientOptions options = client_options(fe.settings(), fe.width(), fe.height());
    std::shared_ptr<SavedGame> saved = save ? std::make_shared<SavedGame>(*save) : nullptr;
    return [&art, save_dir, campaign, index, info, rq, options, saved](const client::LoadProgress& progress,
                                                                        std::string& error) -> std::unique_ptr<client::BattleSession> {
        const BattleRequest req = make_mission_request(art.vfs(), info, rq);
        std::unique_ptr<SkirmishSession> s = SkirmishSession::start(art.game_dir(), req, 0, options, progress, error);
        if (!s) return nullptr;
        if (saved && !replay_save(s->battle(), *saved, error)) return nullptr;
        return std::make_unique<MissionSession>(std::move(s), art, save_dir, campaign, index, rq);
    };
}

// --- The briefing of one mission: text pages, narration, then the battle.
class BriefingScreen : public client::Screen {
public:
    BriefingScreen(MenuArt& art, std::string save_dir, std::string campaign, int index, int difficulty)
        : art_(art), save_dir_(std::move(save_dir)), campaign_(std::move(campaign)), index_(index), difficulty_(difficulty) {}

    void enter(client::FrontEnd& fe) override {
        if (loaded_) return;
        loaded_ = true;
        Campaign c;
        if (!load_campaign(art_.vfs(), campaign_, c, error_) || index_ < 0 || index_ >= static_cast<int>(c.missions.size())) {
            if (error_.empty()) error_ = "no such mission";
            return;
        }
        side_ = c.side;
        title_ = c.missions[static_cast<std::size_t>(index_)].name;
        if (!load_mission(art_.vfs(), c.missions[static_cast<std::size_t>(index_)].file, difficulty_, info_, error_)) return;
        ok_ = true;
        lines_ = wrap(info_.brief_text.empty() ? info_.description : info_.brief_text, fe.menu_content().font, fe.width() - 80);
        // The narration (camps\briefs\<narration>.wav).
        std::vector<std::uint8_t> wav;
        client::Sample s;
        if (!info_.narration.empty() && art_.vfs().read("camps\\briefs\\" + info_.narration + ".wav", wav) && client::decode_wav(wav, s)) {
            art_.content().samples.push_back(std::move(s));
            fe.play_sound(static_cast<int>(art_.content().samples.size()) - 1);
        }
    }
    void handle(client::FrontEnd& fe, const client::InputEvent& e) override {
        if (e.type != client::InputEvent::Type::KeyDown && e.type != client::InputEvent::Type::MouseDown) return;
        if (e.type == client::InputEvent::Type::KeyDown && e.key == client::keys::kEscape) {
            fe.pop();
            return;
        }
        const int per_page = std::max(1, (fe.height() - 120) / 14);
        if (e.type == client::InputEvent::Type::KeyDown && (e.key == client::keys::kRight || e.key == client::keys::kDown)) {
            if ((page_ + 1) * per_page < static_cast<int>(lines_.size())) ++page_;
            return;
        }
        if (e.type == client::InputEvent::Type::KeyDown && (e.key == client::keys::kLeft || e.key == client::keys::kUp)) {
            if (page_ > 0) --page_;
            return;
        }
        if (!ok_) return;
        // Start: Enter, space or a click.
        MissionRequest rq;
        rq.campaign_side = side_;
        rq.difficulty = difficulty_;
        rq.battle_rng_seed = clock_value();
        rq.loading_rand_seed = static_cast<std::uint32_t>(std::time(nullptr));
        rq.crt_rand_state = clock_value() * 2654435761u;
        fe.start_battle(mission_launcher(art_, save_dir_, campaign_, index_, info_, rq, fe, nullptr));
    }
    void draw(client::FrontEnd& fe, render::Image8& out) override {
        Pen p(fe, out);
        p.clear();
        const client::MenuContent& m = fe.menu_content();
        p.text(campaign_, 40, 20, m.text_dim);
        p.text(ok_ ? title_ : error_, 40, 36, m.text_highlight);
        if (ok_) {
            p.text(info_.description, 40, 54, m.text_colour, fe.width() - 80);
            const int per_page = std::max(1, (fe.height() - 120) / 14);
            for (int k = 0; k < per_page; ++k) {
                const std::size_t at = static_cast<std::size_t>(page_ * per_page + k);
                if (at >= lines_.size()) break;
                p.text(lines_[at], 40, 80 + k * 14, m.text_colour);
            }
            p.text("Enter: start   Arrows: pages   Esc: back", 40, fe.height() - 30, m.text_dim);
        }
    }

private:
    MenuArt& art_;
    std::string save_dir_, campaign_, side_, title_, error_;
    int index_, difficulty_;
    MissionInfo info_;
    std::vector<std::string> lines_;
    int page_ = 0;
    bool loaded_ = false, ok_ = false;
};

bool MissionSession::after_battle(client::FrontEnd& fe, client::BattleOutcome outcome) {
    if (outcome != client::BattleOutcome::Victory) return false;
    // A won mission opens the next one (the original's progress string).
    client::Settings& s = fe.settings();
    const std::string key = campaign_progress_key(campaign_);
    if (s.get_int(key, 0) < index_ + 1) s.set_int(key, index_ + 1);
    Campaign c;
    std::string err;
    if (!load_campaign(art_.vfs(), campaign_, c, err) || index_ + 1 >= static_cast<int>(c.missions.size())) return false;
    fe.push(std::make_unique<BriefingScreen>(art_, save_dir_, campaign_, index_ + 1, rq_.difficulty));
    return true;
}

// --- The campaign menu: campaigns, the missions reached, the difficulty.
class CampaignScreen : public client::Screen {
public:
    CampaignScreen(MenuArt& art, std::string save_dir) : art_(art), save_dir_(std::move(save_dir)) {
        names_ = list_campaigns(art_.vfs());
    }
    void enter(client::FrontEnd& fe) override {
        difficulty_ = std::clamp(fe.settings().get_int("Difficulty", 1), 0, 2);
        load_missions(fe);
    }
    void handle(client::FrontEnd& fe, const client::InputEvent& e) override {
        if (e.type != client::InputEvent::Type::KeyDown) return;
        switch (e.key) {
        case client::keys::kEscape: fe.pop(); return;
        case client::keys::kTab: focus_ = 1 - focus_; return;
        case client::keys::kUp:
            if (focus_ == 0 && camp_ > 0) --camp_, load_missions(fe);
            else if (focus_ == 1 && mission_ > 0) --mission_;
            return;
        case client::keys::kDown:
            if (focus_ == 0 && camp_ + 1 < static_cast<int>(names_.size())) ++camp_, load_missions(fe);
            else if (focus_ == 1 && mission_ + 1 < reachable()) ++mission_;
            return;
        case 'd':
            difficulty_ = (difficulty_ + 1) % 3;
            fe.settings().set_int("Difficulty", difficulty_);
            return;
        case client::keys::kEnter:
            if (focus_ == 0) {
                focus_ = 1;
                return;
            }
            if (camp_ < static_cast<int>(names_.size()) && mission_ < reachable())
                fe.push(std::make_unique<BriefingScreen>(art_, save_dir_, names_[static_cast<std::size_t>(camp_)], mission_, difficulty_));
            return;
        default: return;
        }
    }
    void draw(client::FrontEnd& fe, render::Image8& out) override {
        static const char* const kDifficulty[3] = {"Easy", "Medium", "Hard"};
        Pen p(fe, out);
        p.clear();
        const client::MenuContent& m = fe.menu_content();
        p.text("Campaign", 40, 20, m.text_highlight);
        p.text(std::string("Difficulty: ") + kDifficulty[difficulty_] + "   (D)", fe.width() / 2, 20, m.text_colour);
        const int col = fe.width() / 2 - 60;
        for (std::size_t k = 0; k < names_.size(); ++k) p.row(names_[k], 40, 50 + static_cast<int>(k) * p.line(), col, focus_ == 0 && static_cast<int>(k) == camp_);
        for (int k = 0; k < reachable(); ++k)
            p.row(campaign_.missions[static_cast<std::size_t>(k)].name, fe.width() / 2, 50 + k * p.line(), col, focus_ == 1 && k == mission_);
        p.text("Arrows: choose   Tab: list   Enter: briefing   Esc: back", 40, fe.height() - 30, m.text_dim);
    }

private:
    int reachable() const {
        const int n = static_cast<int>(campaign_.missions.size());
        return std::min(n, reached_ + 1);
    }
    void load_missions(client::FrontEnd& fe) {
        campaign_ = Campaign{};
        std::string err;
        if (camp_ < static_cast<int>(names_.size())) load_campaign(art_.vfs(), names_[static_cast<std::size_t>(camp_)], campaign_, err);
        reached_ = camp_ < static_cast<int>(names_.size()) ? fe.settings().get_int(campaign_progress_key(names_[static_cast<std::size_t>(camp_)]), 0) : 0;
        mission_ = std::min(mission_, std::max(0, reachable() - 1));
    }
    MenuArt& art_;
    std::string save_dir_;
    std::vector<std::string> names_;
    Campaign campaign_;
    int camp_ = 0, mission_ = 0, focus_ = 0, difficulty_ = 1, reached_ = 0;
};

// --- Saved games: the list of <dir>\*.sav with their summaries.
struct SaveEntry {
    std::filesystem::path path;
    SaveSummary summary;
    bool readable = false;
};

std::vector<SaveEntry> list_saves(const std::string& dir) {
    std::vector<SaveEntry> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || stricmp_c(e.path().extension().string(), ".sav") != 0) continue;
        SaveEntry s;
        s.path = e.path();
        std::ifstream in(e.path(), std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string err;
        s.readable = read_save_summary(bytes, s.summary, err);
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const SaveEntry& a, const SaveEntry& b) { return a.path.filename() < b.path.filename(); });
    return out;
}

class LoadScreen : public client::Screen {
public:
    LoadScreen(MenuArt& art, std::string save_dir) : art_(art), dir_(std::move(save_dir)) { saves_ = list_saves(dir_); }
    void handle(client::FrontEnd& fe, const client::InputEvent& e) override {
        if (e.type != client::InputEvent::Type::KeyDown) return;
        if (e.key == client::keys::kEscape) {
            fe.pop();
        } else if (e.key == client::keys::kUp && sel_ > 0) {
            --sel_;
        } else if (e.key == client::keys::kDown && sel_ + 1 < static_cast<int>(saves_.size())) {
            ++sel_;
        } else if (e.key == client::keys::kEnter && sel_ < static_cast<int>(saves_.size())) {
            load(fe, saves_[static_cast<std::size_t>(sel_)]);
        }
    }
    void draw(client::FrontEnd& fe, render::Image8& out) override {
        Pen p(fe, out);
        p.clear();
        const client::MenuContent& m = fe.menu_content();
        p.text("Load game", 40, 20, m.text_highlight);
        for (std::size_t k = 0; k < saves_.size(); ++k) {
            const SaveEntry& s = saves_[k];
            std::string line = s.path.stem().string();
            if (s.readable) {
                line += "  " + (s.summary.description.empty() ? s.summary.map : s.summary.description) + "  " + format_time(s.summary.game_time);
                if (!s.summary.engine) line += "  (the original's)";
            } else {
                line += "  (unreadable)";
            }
            p.row(line, 40, 50 + static_cast<int>(k) * p.line(), fe.width() - 80, static_cast<int>(k) == sel_);
        }
        if (!message_.empty()) p.text(message_, 40, fe.height() - 50, m.text_colour, fe.width() - 80);
        p.text("Arrows: choose   Enter: load   Esc: back", 40, fe.height() - 30, m.text_dim);
    }

private:
    void load(client::FrontEnd& fe, const SaveEntry& e) {
        std::ifstream in(e.path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        SavedGame save;
        if (!read_saved_game(bytes, save, message_)) return;
        if (fe.session()) fe.end_battle(client::BattleOutcome::Quit);
        if (save.start.kind == GameStart::Kind::Mission) {
            MissionInfo info;
            if (!load_mission(art_.vfs(), save.start.mission_file, save.start.mission.difficulty, info, message_)) return;
            fe.start_battle(mission_launcher(art_, dir_, save.start.campaign, save.start.mission_index, info, save.start.mission, fe, &save));
            return;
        }
        const client::ClientOptions options = client_options(fe.settings(), fe.width(), fe.height());
        auto saved = std::make_shared<SavedGame>(save);
        MenuArt& art = art_;
        fe.start_battle([&art, saved, options](const client::LoadProgress& progress, std::string& error) -> std::unique_ptr<client::BattleSession> {
            std::unique_ptr<SkirmishSession> s =
                SkirmishSession::start(art.game_dir(), saved->start.battle, saved->start.battle.start.local_player, options, progress, error);
            if (!s || !replay_save(s->battle(), *saved, error)) return nullptr;
            return s;
        });
    }
    MenuArt& art_;
    std::string dir_, message_;
    std::vector<SaveEntry> saves_;
    int sel_ = 0;
};

class SaveScreen : public client::Screen {
public:
    SaveScreen(std::string save_dir, client::BattleSession& session) : dir_(std::move(save_dir)), session_(session) {}
    bool see_through() const override { return false; }
    void handle(client::FrontEnd& fe, const client::InputEvent& e) override {
        if (e.type != client::InputEvent::Type::KeyDown) return;
        if (e.key == client::keys::kEscape) {
            fe.pop();
        } else if (e.key == client::keys::kEnter) {
            save(fe);
        } else if (e.key == 8 && !name_.empty()) {
            name_.pop_back();
        } else if (e.key >= 32 && e.key < 127 && name_.size() < 32 && e.key != '\\' && e.key != '/' && e.key != ':') {
            name_.push_back(static_cast<char>(e.key));
        }
    }
    void draw(client::FrontEnd& fe, render::Image8& out) override {
        Pen p(fe, out);
        p.clear();
        const client::MenuContent& m = fe.menu_content();
        p.text("Save game", 40, 20, m.text_highlight);
        p.text("Name: " + name_ + "_", 40, 50, m.text_colour);
        if (!message_.empty()) p.text(message_, 40, 80, m.text_colour, fe.width() - 80);
        p.text("Enter: save   Esc: back", 40, fe.height() - 30, m.text_dim);
    }

private:
    void save(client::FrontEnd& fe) {
        if (name_.empty()) return;
        auto* journaled = dynamic_cast<JournaledSession*>(&session_);
        SavedGame s;
        if (!journaled) {
            message_ = "This battle cannot be saved exactly: its inputs are not journaled.";
            return;
        }
        if (!journaled->make_save(s, message_)) return;
        s.summary.description = name_;
        std::vector<std::uint8_t> bytes;
        write_saved_game(s, bytes);
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        std::ofstream o(std::filesystem::path(dir_) / (name_ + ".sav"), std::ios::binary);
        o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        message_ = o ? "Saved." : "Could not write the file.";
        if (o) fe.pop();
    }
    std::string dir_, name_, message_;
    client::BattleSession& session_;
};

}  // namespace

void install_campaign_hooks(client::FrontEndHooks& hooks, MenuArt& art, const std::string& save_dir) {
    mount_cd_archives(art.vfs(), art.game_dir());
    hooks.campaign_menu = [&art, save_dir](client::FrontEnd&) -> std::unique_ptr<client::Screen> { return std::make_unique<CampaignScreen>(art, save_dir); };
    hooks.load_game_menu = [&art, save_dir](client::FrontEnd&, client::BattleSession*) -> std::unique_ptr<client::Screen> {
        return std::make_unique<LoadScreen>(art, save_dir);
    };
    hooks.save_game_menu = [save_dir](client::FrontEnd&, client::BattleSession& s) -> std::unique_ptr<client::Screen> {
        return std::make_unique<SaveScreen>(save_dir, s);
    };
}

}  // namespace ta

#else

void ta::install_campaign_hooks(client::FrontEndHooks&, MenuArt&, const std::string&) {}

#endif
