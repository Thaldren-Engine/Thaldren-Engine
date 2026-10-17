// The campaign and saved games in the front end (ta/client_campaign.h). Built when the front
// end's screens (ta/client_menus.h) are present.

#include "ta/client_campaign.h"

std::string ta::progress_from_thumbs(const std::string& thumbs) {
    // The load menu copies at most 25 characters and starts over when the string is not 25 long.
    const std::string t = thumbs.substr(0, 25);
    return t.size() == 25 ? t : std::string(25, 'U');
}

#if __has_include("ta/client_menus.h")

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "client/audio.h"
#include "client/canvas.h"
#include "client/front_end.h"
#include "client/game_client.h"
#include "client/menu.h"
#include "ta/campaign.h"
#include "ta/client_menus.h"
#include "ta/client_world.h"
#include "ta/tdf.h"

namespace ta {

namespace {

std::uint32_t clock_value() {
    // The original seeds a battle from the high-resolution clock (an input).
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

int side_index(Vfs& vfs, const std::string& side) {
    std::vector<std::uint8_t> sd;
    if (vfs.read("gamedata\\sidedata.tdf", sd)) {
        const TdfParseResult r = tdf_parse(std::string_view(reinterpret_cast<const char*>(sd.data()), sd.size()));
        for (int k = 0; r.root; ++k) {
            const TdfSection* sec = r.root->child("SIDE" + std::to_string(k));
            if (!sec) break;
            if (stricmp_c(sec->get_string("name", 32, ""), side) == 0) return k;
        }
    }
    return stricmp_c(side, "CORE") == 0 ? 1 : 0;
}

bool write_file(const std::string& dir, const std::string& name, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream o(std::filesystem::path(dir) / (name + ".SAV"), std::ios::binary);
    o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(o);
}

class BriefingScreen;
class EndMissionScreen;

// A mission battle: the front end's battle session with the campaign's progression.
class MissionSession : public client::BattleSession {
public:
    MissionSession(std::unique_ptr<SkirmishSession> inner, MenuArt& art, std::string save_dir, std::shared_ptr<CampaignProgress> progress)
        : inner_(std::move(inner)), art_(art), save_dir_(std::move(save_dir)), progress_(std::move(progress)) {}
    sim::Battle& battle() override { return inner_->battle(); }
    client::Content& content() override { return inner_->content(); }
    client::GameClient& client() override { return inner_->client(); }
    int frame(std::uint32_t now_ms) override { return inner_->frame(now_ms); }
    client::BattleOutcome outcome() override { return inner_->outcome(); }
    void send_chat(const std::string& t) override { inner_->send_chat(t); }
    void surrender() override { inner_->surrender(); }
    std::vector<client::PlayerStats> stats() override { return inner_->stats(); }
    bool end_table(client::EndTable& t) override { return inner_->end_table(t); }
    std::string battle_name() const override { return inner_->battle_name(); }
    int difficulty() const override { return inner_->difficulty(); }
    bool after_battle(client::FrontEnd& fe, client::BattleOutcome outcome) override;
    SkirmishSession& inner() { return *inner_; }
    CampaignProgress& progress() { return *progress_; }

private:
    std::unique_ptr<SkirmishSession> inner_;
    MenuArt& art_;
    std::string save_dir_;
    std::shared_ptr<CampaignProgress> progress_;
};

client::BattleLauncher mission_launcher(MenuArt& art, const std::string& save_dir, std::shared_ptr<CampaignProgress> progress, const MissionInfo& info,
                                        client::FrontEnd& fe) {
    const client::ClientOptions options = client_options(fe.settings(), fe.width(), fe.height());
    MissionRequest rq;
    rq.campaign_side = progress->side;
    rq.difficulty = progress->difficulty;
    rq.battle_rng_seed = clock_value();
    rq.loading_rand_seed = static_cast<std::uint32_t>(std::time(nullptr));
    rq.crt_rand_state = clock_value() * 2654435761u;
    return [&art, save_dir, progress, info, rq, options](const client::LoadProgress& lp, std::string& error) -> std::unique_ptr<client::BattleSession> {
        const BattleRequest req = make_mission_request(art.vfs(), info, rq);
        std::unique_ptr<SkirmishSession> s = SkirmishSession::start(art.game_dir(), req, 0, options, lp, error);
        if (!s) return nullptr;
        return std::make_unique<MissionSession>(std::move(s), art, save_dir, progress);
    };
}

// --- The briefing of one mission: text pages, narration, then the battle.
class BriefingScreen : public client::Screen {
public:
    BriefingScreen(MenuArt& art, std::string save_dir, std::shared_ptr<CampaignProgress> progress)
        : art_(art), save_dir_(std::move(save_dir)), progress_(std::move(progress)) {}

    void enter(client::FrontEnd& fe) override {
        if (loaded_) return;
        loaded_ = true;
        Campaign c;
        const int index = progress_->mission;
        if (!load_campaign(art_.vfs(), progress_->campaign, c, error_) || index < 0 || index >= static_cast<int>(c.missions.size())) {
            if (error_.empty()) error_ = "no such mission";
            return;
        }
        title_ = c.missions[static_cast<std::size_t>(index)].name;
        if (!load_mission(art_.vfs(), c.missions[static_cast<std::size_t>(index)].file, progress_->difficulty, info_, error_)) return;
        ok_ = true;
        lines_ = wrap(info_.brief_text.empty() ? info_.description : info_.brief_text, fe.menu_content().font, fe.width() - 80);
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
        fe.start_battle(mission_launcher(art_, save_dir_, progress_, info_, fe));
    }
    void draw(client::FrontEnd& fe, render::Image8& out) override {
        Pen p(fe, out);
        p.clear();
        const client::MenuContent& m = fe.menu_content();
        p.text(progress_->campaign, 40, 20, m.text_dim);
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
    std::string save_dir_, title_, error_;
    std::shared_ptr<CampaignProgress> progress_;
    MissionInfo info_;
    std::vector<std::string> lines_;
    int page_ = 0;
    bool loaded_ = false, ok_ = false;
};

// The save menu's file name line (SAVEGAME\<name>.SAV).
struct NameLine {
    std::string name;
    bool key(int k) {
        if (k == 8 && !name.empty()) {
            name.pop_back();
            return true;
        }
        if (k >= 32 && k < 127 && name.size() < 32 && k != '\\' && k != '/' && k != ':' && k != '*' && k != '?') {
            name.push_back(static_cast<char>(k));
            return true;
        }
        return false;
    }
};

// --- Between missions (ENDMSN, 0x41F0A0 / 0x41EC50): the missions with their results.
class EndMissionScreen : public client::Screen {
public:
    EndMissionScreen(MenuArt& art, std::string save_dir, std::shared_ptr<CampaignProgress> progress)
        : art_(art), save_dir_(std::move(save_dir)), progress_(std::move(progress)) {
        std::string err;
        load_campaign(art_.vfs(), progress_->campaign, campaign_, err);
        // The mission after a won one is selected, else the same one.
        sel_ = std::min(progress_->mission + (progress_->won ? 1 : 0), std::max(0, static_cast<int>(campaign_.missions.size()) - 1));
    }
    void handle(client::FrontEnd& fe, const client::InputEvent& e) override {
        if (e.type != client::InputEvent::Type::KeyDown) return;
        if (saving_) {
            if (e.key == client::keys::kEscape) saving_ = false;
            else if (e.key == client::keys::kEnter && !name_.name.empty()) save_between_missions();
            else name_.key(e.key);
            return;
        }
        switch (e.key) {
        case client::keys::kEscape: fe.pop(); return;
        case client::keys::kUp: sel_ = std::max(0, sel_ - 1); return;
        case client::keys::kDown: sel_ = std::min(static_cast<int>(campaign_.missions.size()) - 1, sel_ + 1); return;
        case 'd': progress_->difficulty = (progress_->difficulty + 1) % 3; return;  // Easy -> Medium -> Hard -> Easy
        case 's': saving_ = true; message_.clear(); return;
        case client::keys::kEnter:
            if (sel_ >= 0 && sel_ < static_cast<int>(campaign_.missions.size())) {
                progress_->mission = sel_;
                fe.push(std::make_unique<BriefingScreen>(art_, save_dir_, progress_));
            }
            return;
        default: return;
        }
    }
    void draw(client::FrontEnd& fe, render::Image8& out) override {
        static const char* const kDifficulty[3] = {"Easy", "Medium", "Hard"};
        Pen p(fe, out);
        p.clear();
        const client::MenuContent& m = fe.menu_content();
        p.text(progress_->campaign + (progress_->won ? ": mission won" : ": mission lost"), 40, 20, m.text_highlight);
        p.text(std::string("Difficulty: ") + kDifficulty[progress_->difficulty % 3] + "   (D)", fe.width() / 2, 20, m.text_colour);
        for (std::size_t k = 0; k < campaign_.missions.size(); ++k) {
            const char r = k < progress_->results.size() ? progress_->results[k] : 'U';
            const std::string mark = r == 'W' ? "[won]  " : r == 'L' ? "[lost] " : "       ";
            p.row(mark + campaign_.missions[k].name, 40, 50 + static_cast<int>(k) * p.line(), fe.width() - 80, static_cast<int>(k) == sel_);
        }
        if (saving_) p.text("Save as: " + name_.name + "_", 40, fe.height() - 50, m.text_colour);
        else if (!message_.empty()) p.text(message_, 40, fe.height() - 50, m.text_colour);
        p.text("Arrows: mission   Enter: start   S: save   D: difficulty   Esc: main menu", 40, fe.height() - 30, m.text_dim);
    }

private:
    void save_between_missions() {
        // GameSave_SaveAll outside a battle: the summary with the next mission's name (the map
        // info moves on when there is a next mission, whatever the outcome) and BetweenMissions.
        SaveContext c;
        c.description = name_.name;
        c.game_id = static_cast<std::int32_t>(std::time(nullptr));
        c.game_type = 1;
        c.campaign = progress_->campaign;
        const int next = progress_->mission + 1 < static_cast<int>(campaign_.missions.size()) ? progress_->mission + 1 : progress_->mission;
        c.mission = next < static_cast<int>(campaign_.missions.size()) ? campaign_.missions[static_cast<std::size_t>(next)].name : std::string();
        c.thumbs = progress_->results;
        c.difficulty = progress_->difficulty;
        c.side = side_index(art_.vfs(), progress_->side);
        c.between_missions = true;
        std::vector<std::uint8_t> bytes;
        write_bank(make_between_missions_bank(c), bytes, nullptr, c.bank);
        message_ = write_file(save_dir_, name_.name, bytes) ? "Saved." : "Could not write the file.";
        saving_ = false;
    }
    MenuArt& art_;
    std::string save_dir_, message_;
    std::shared_ptr<CampaignProgress> progress_;
    Campaign campaign_;
    int sel_ = 0;
    bool saving_ = false;
    NameLine name_;
};

bool MissionSession::after_battle(client::FrontEnd& fe, client::BattleOutcome outcome) {
    if (outcome != client::BattleOutcome::Victory && outcome != client::BattleOutcome::Defeat) return false;
    // 0x41DC20: the mission's result goes into the results string at its index.
    progress_->won = outcome == client::BattleOutcome::Victory;
    if (progress_->mission >= 0 && progress_->mission < 25) progress_->results[static_cast<std::size_t>(progress_->mission)] = progress_->won ? 'W' : 'L';
    fe.push(std::make_unique<EndMissionScreen>(art_, save_dir_, progress_));
    return true;
}

// --- A new campaign (0x477AB0): the campaign, then its first mission (any one with the
// AllMissions setting); the results start over.
class CampaignScreen : public client::Screen {
public:
    CampaignScreen(MenuArt& art, std::string save_dir) : art_(art), save_dir_(std::move(save_dir)) { names_ = list_campaigns(art_.vfs()); }
    void enter(client::FrontEnd& fe) override {
        difficulty_ = std::clamp(fe.settings().get_int("Difficulty", 1), 0, 2);
        any_mission_ = fe.settings().get_int("AllMissions", 0) != 0;
        load_missions();
    }
    void handle(client::FrontEnd& fe, const client::InputEvent& e) override {
        if (e.type != client::InputEvent::Type::KeyDown) return;
        switch (e.key) {
        case client::keys::kEscape: fe.pop(); return;
        case client::keys::kTab:
            if (any_mission_) focus_ = 1 - focus_;
            return;
        case client::keys::kUp:
            if (focus_ == 0 && camp_ > 0) --camp_, load_missions();
            else if (focus_ == 1 && mission_ > 0) --mission_;
            return;
        case client::keys::kDown:
            if (focus_ == 0 && camp_ + 1 < static_cast<int>(names_.size())) ++camp_, load_missions();
            else if (focus_ == 1 && mission_ + 1 < static_cast<int>(campaign_.missions.size())) ++mission_;
            return;
        case 'd':
            difficulty_ = (difficulty_ + 1) % 3;
            fe.settings().set_int("Difficulty", difficulty_);
            return;
        case client::keys::kEnter: {
            if (camp_ >= static_cast<int>(names_.size())) return;
            auto progress = std::make_shared<CampaignProgress>();
            progress->campaign = names_[static_cast<std::size_t>(camp_)];
            progress->side = campaign_.side.empty() ? "ARM" : campaign_.side;
            progress->mission = any_mission_ ? mission_ : 0;
            progress->difficulty = difficulty_;
            fe.push(std::make_unique<BriefingScreen>(art_, save_dir_, progress));
            return;
        }
        default: return;
        }
    }
    void draw(client::FrontEnd& fe, render::Image8& out) override {
        static const char* const kDifficulty[3] = {"Easy", "Medium", "Hard"};
        Pen p(fe, out);
        p.clear();
        const client::MenuContent& m = fe.menu_content();
        p.text("New campaign", 40, 20, m.text_highlight);
        p.text(std::string("Difficulty: ") + kDifficulty[difficulty_] + "   (D)", fe.width() / 2, 20, m.text_colour);
        const int col = fe.width() / 2 - 60;
        for (std::size_t k = 0; k < names_.size(); ++k) p.row(names_[k], 40, 50 + static_cast<int>(k) * p.line(), col, focus_ == 0 && static_cast<int>(k) == camp_);
        if (any_mission_)
            for (std::size_t k = 0; k < campaign_.missions.size(); ++k)
                p.row(campaign_.missions[k].name, fe.width() / 2, 50 + static_cast<int>(k) * p.line(), col, focus_ == 1 && static_cast<int>(k) == mission_);
        p.text(any_mission_ ? "Arrows: choose   Tab: list   Enter: start   Esc: back" : "Arrows: choose   Enter: start   Esc: back", 40, fe.height() - 30,
               m.text_dim);
    }

private:
    void load_missions() {
        campaign_ = Campaign{};
        std::string err;
        if (camp_ < static_cast<int>(names_.size())) load_campaign(art_.vfs(), names_[static_cast<std::size_t>(camp_)], campaign_, err);
        mission_ = std::min(mission_, std::max(0, static_cast<int>(campaign_.missions.size()) - 1));
    }
    MenuArt& art_;
    std::string save_dir_;
    std::vector<std::string> names_;
    Campaign campaign_;
    int camp_ = 0, mission_ = 0, focus_ = 0, difficulty_ = 1;
    bool any_mission_ = false;
};

// --- Saved games: the list of <dir>\*.SAV with their summaries.
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
                line += "  " + s.summary.mission + "  " + format_time(s.summary.game_time);
                if (s.summary.between_missions) line += "  (between missions)";
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
        auto save = std::make_shared<SavedGame>();
        if (!read_saved_game(bytes, *save, message_)) return;
        const SaveSummary& m = save->summary;
        if (m.game_type != 1 && m.game_type != 2) {
            message_ = "Invalid savegame file";
            return;
        }
        std::shared_ptr<CampaignProgress> progress;
        if (m.game_type == 1) {
            Campaign c;
            if (!load_campaign(art_.vfs(), m.campaign, c, message_)) return;
            progress = std::make_shared<CampaignProgress>();
            progress->campaign = m.campaign;
            progress->side = m.side == 0 ? "ARM" : "CORE";
            progress->difficulty = m.difficulty;
            progress->results = progress_from_thumbs(m.thumbs);
            progress->mission = -1;
            for (std::size_t k = 0; k < c.missions.size(); ++k)
                if (stricmp_c(c.missions[k].name, m.mission) == 0) progress->mission = static_cast<int>(k);
            if (progress->mission < 0) {
                message_ = "Invalid savegame file";
                return;
            }
            if (m.between_missions) {
                // Between missions: the campaign's mission screen with that mission.
                if (fe.session()) fe.end_battle(client::BattleOutcome::Quit);
                fe.push(std::make_unique<BriefingScreen>(art_, dir_, progress));
                return;
            }
        }
        if (fe.session()) fe.end_battle(client::BattleOutcome::Quit);
        const client::ClientOptions options = client_options(fe.settings(), fe.width(), fe.height());
        MenuArt& art = art_;
        const std::string dir = dir_;
        LoadInputs inputs;
        inputs.battle_rng_seed = clock_value();
        inputs.loading_rand_seed = static_cast<std::uint32_t>(std::time(nullptr));
        inputs.crt_rand_state = clock_value() * 2654435761u;
        inputs.unit_limit = static_cast<std::uint16_t>(std::clamp(fe.settings().get_int("UnitLimit", 250), 20, 500));
        fe.start_battle([&art, save, progress, options, dir, inputs](const client::LoadProgress& lp, std::string& error) -> std::unique_ptr<client::BattleSession> {
            auto view = std::make_shared<LoadedView>();
            BattleRequest req;
            if (!make_saved_game_request(art.vfs(), *save, inputs, req, *view, error)) return nullptr;
            int human = req.start.local_player;
            std::unique_ptr<SkirmishSession> s = SkirmishSession::start(art.game_dir(), req, human, options, lp, error);
            if (!s) return nullptr;
            s->client().set_scroll(view->camera_x, view->camera_z);
            if (progress) return std::make_unique<MissionSession>(std::move(s), art, dir, progress);
            return s;
        });
    }
    MenuArt& art_;
    std::string dir_, message_;
    std::vector<SaveEntry> saves_;
    int sel_ = 0;
};

// --- The in-game save menu (0x493060 / 0x492DF0): the typed name, written at once.
class SaveScreen : public client::Screen {
public:
    SaveScreen(std::string save_dir, client::BattleSession& session) : dir_(std::move(save_dir)), session_(session) {}
    void handle(client::FrontEnd& fe, const client::InputEvent& e) override {
        if (e.type != client::InputEvent::Type::KeyDown) return;
        if (e.key == client::keys::kEscape) {
            fe.pop();
        } else if (e.key == client::keys::kEnter) {
            save();
        } else {
            name_.key(e.key);
        }
    }
    void draw(client::FrontEnd& fe, render::Image8& out) override {
        Pen p(fe, out);
        p.clear();
        const client::MenuContent& m = fe.menu_content();
        p.text("Save game", 40, 20, m.text_highlight);
        p.text("Name: " + name_.name + "_", 40, 50, m.text_colour);
        if (!message_.empty()) p.text(message_, 40, 80, m.text_colour, fe.width() - 80);
        p.text("Enter: save   Esc: back", 40, fe.height() - 30, m.text_dim);
    }

private:
    void save() {
        if (name_.name.empty()) return;
        SkirmishSession* sk = dynamic_cast<SkirmishSession*>(&session_);
        CampaignProgress* progress = nullptr;
        if (auto* ms = dynamic_cast<MissionSession*>(&session_)) {
            sk = &ms->inner();
            progress = &ms->progress();
        }
        if (!sk) {
            message_ = "This battle cannot be saved.";
            return;
        }
        const SaveContext c = battle_save_context(*sk, progress, name_.name);
        std::vector<std::uint8_t> bytes;
        write_save_file(sk->world().loaded(), c, bytes);
        message_ = write_file(dir_, name_.name, bytes) ? "Saved." : "Could not write the file.";
    }
    std::string dir_, message_;
    client::BattleSession& session_;
    NameLine name_;
};

}  // namespace

SaveContext battle_save_context(SkirmishSession& s, const CampaignProgress* campaign, const std::string& description) {
    SaveContext c;
    const sim::Battle& b = s.battle();
    const BattleRequest& req = s.world().request();
    c.description = description;
    c.game_id = static_cast<std::int32_t>(std::time(nullptr));
    c.max_units = b.settings.unit_limit;
    c.difficulty = b.settings.ai_difficulty;
    c.players = b.settings.player_count;
    c.camera_x = s.client().scroll_x();
    c.camera_z = s.client().scroll_y();
    Vfs& vfs = s.world().vfs_;
    for (int k = 0; k < 10; ++k) {
        c.player_side[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(side_index(vfs, req.sides[static_cast<std::size_t>(k)]));
        c.player_logo[static_cast<std::size_t>(k)] = req.start.players[static_cast<std::size_t>(k)].colour;
    }
    c.side = c.player_side[b.settings.local_player % 10];
    if (campaign) {
        c.game_type = 1;
        c.campaign = campaign->campaign;
        Campaign camp;
        std::string err;
        if (load_campaign(vfs, campaign->campaign, camp, err) && campaign->mission >= 0 && campaign->mission < static_cast<int>(camp.missions.size()))
            c.mission = camp.missions[static_cast<std::size_t>(campaign->mission)].name;
        c.thumbs = campaign->results;
    } else {
        c.game_type = 2;
        c.mission = req.map;
        c.commander_death = req.start.commander_mode;
        c.location = req.start.fixed_locations ? 1 : 0;
        c.mapping = req.start.explore ? 1 : 0;
        c.line_of_sight = req.start.fog_of_war ? 1 : 0;
        c.line_of_sight_type = req.start.true_sight ? 1 : 0;
    }
    return c;
}

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

ta::SaveContext ta::battle_save_context(SkirmishSession&, const CampaignProgress*, const std::string&) { return {}; }
void ta::install_campaign_hooks(client::FrontEndHooks&, MenuArt&, const std::string&) {}

#endif
