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
            // The narration streams on its own channel at full volume (whatever the sound
            // settings); leaving the briefing stops it.
            art_.content().samples.push_back(std::move(s));
            fe.play_narration(static_cast<int>(art_.content().samples.size()) - 1);
        }
    }
    void handle(client::FrontEnd& fe, const client::InputEvent& e) override {
        if (e.type != client::InputEvent::Type::KeyDown && e.type != client::InputEvent::Type::MouseDown) return;
        if (e.type == client::InputEvent::Type::KeyDown && e.key == client::keys::kEscape) {
            // (the previous menu: the narration stops, "Previous")
            fe.stop_narration();
            fe.play_sound(art_.sound("Previous"));
            fe.pop();
            return;
        }
        const int per_page = std::max(1, (fe.height() - 120) / 14);
        if (e.type == client::InputEvent::Type::KeyDown && (e.key == client::keys::kRight || e.key == client::keys::kDown)) {
            // (the text's "more" bar while there is more: "More")
            if ((page_ + 1) * per_page < static_cast<int>(lines_.size())) {
                ++page_;
                fe.play_sound(art_.sound("More"));
            }
            return;
        }
        if (e.type == client::InputEvent::Type::KeyDown && (e.key == client::keys::kLeft || e.key == client::keys::kUp)) {
            if (page_ > 0) --page_;
            return;
        }
        // (Start: "BigButton", the narration stops)
        fe.play_sound(art_.sound("BigButton"));
        if (!ok_) return;
        fe.stop_narration();
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
        case client::keys::kEscape: fe.play_sound(art_.sound("BigButton")); fe.pop(); return;  // (main menu)
        case client::keys::kUp: sel_ = std::max(0, sel_ - 1); return;
        case client::keys::kDown: sel_ = std::min(static_cast<int>(campaign_.missions.size()) - 1, sel_ + 1); return;
        case 'd':  // Easy -> Medium -> Hard -> Easy
            fe.play_sound(art_.sound("Skirmish"));
            progress_->difficulty = (progress_->difficulty + 1) % 3;
            return;
        case 's': fe.play_sound(art_.sound("BigButton")); saving_ = true; message_.clear(); return;
        case client::keys::kEnter:
            fe.play_sound(art_.sound("BigButton"));
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
        case client::keys::kEscape: fe.play_sound(art_.sound("Previous")); fe.pop(); return;
        case client::keys::kTab:
            if (any_mission_) focus_ = 1 - focus_;
            return;
        case client::keys::kUp:
            // (the campaign choice: the first side's "SideSelect", the second's "SideSelect2")
            if (focus_ == 0 && camp_ > 0) --camp_, load_missions(), fe.play_sound(art_.sound(camp_ == 0 ? "SideSelect" : "SideSelect2"));
            else if (focus_ == 1 && mission_ > 0) --mission_;
            return;
        case client::keys::kDown:
            if (focus_ == 0 && camp_ + 1 < static_cast<int>(names_.size()))
                ++camp_, load_missions(), fe.play_sound(art_.sound(camp_ == 0 ? "SideSelect" : "SideSelect2"));
            else if (focus_ == 1 && mission_ + 1 < static_cast<int>(campaign_.missions.size())) ++mission_;
            return;
        case 'd':
            fe.play_sound(art_.sound("SmlButton"));
            difficulty_ = (difficulty_ + 1) % 3;
            fe.settings().set_int("Difficulty", difficulty_);
            return;
        case client::keys::kEnter: {
            fe.play_sound(art_.sound("bigButton"));
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

// --- Saved games: the load and save menus (both LOADGAME.GUI).

std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The saved games the menus list (0x492B10): the files <dir>\*.sav (not directories, not names
// starting with '.') in the directory's order (NTFS: by upper-case name), then stably sorted by
// their last write time (whole seconds), newest first (0x4AF320 / 0x4AEFA0 with the times); a file
// that is not a saved game (the bank named "Total Annihilation 3.0" with a Summary) or has no
// Description string is left out. The list shows the descriptions.
struct SaveFile {
    std::filesystem::path path;
    std::string description;
};
std::vector<SaveFile> list_save_files(const std::string& dir) {
    struct Found {
        std::filesystem::path path;
        std::string upper;
        std::int64_t time = 0;
    };
    std::vector<Found> found;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.' || e.is_directory(ec)) continue;
        if (stricmp_c(e.path().extension().string(), ".sav") != 0) continue;
        Found f;
        f.path = e.path();
        for (char c : name) f.upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        const auto t = std::filesystem::last_write_time(e.path(), ec);
        f.time = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
        found.push_back(f);
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.upper < b.upper; });
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return static_cast<std::int32_t>(b.time - a.time) < 0; });
    std::vector<SaveFile> out;
    for (const Found& f : found) {
        Bank bank;
        std::string err;
        if (!read_bank(read_file(f.path), bank, err, "Summary") || stricmp_c(bank.name, "Total Annihilation 3.0") != 0) continue;
        const BankSection* s = bank.find("Summary");
        const std::string* d = s ? s->find_string("Description") : nullptr;
        if (!d) continue;
        out.push_back({f.path, *d});
    }
    return out;
}

// A picture mapped onto a surface gadget's box as the original draws it (0x4A4980): texels (1,1)
// .. (w-1, h-1) onto the box's corners.
void draw_box_picture(render::Image8& out, const render::ClipRect& clip, const render::Sprite8& pic, int x0, int y0, int w, int h) {
    const int x1 = x0 + w - 1, y1 = y0 + h - 1;
    const render::RasterVertex v[4] = {{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}};
    const int uv[4][2] = {{1, 1}, {pic.width - 1, 1}, {pic.width - 1, pic.height - 1}, {1, pic.height - 1}};
    render::RasterTarget t;
    t.pixels = out.pixels.data();
    t.pitch = out.width;
    t.clip = clip;
    render::textured_quad_uv(t, v, uv, pic);
}

// The load menu (0x4931D0, LOADGAME.GUI with flags 0x980: centred, the screen under it dimmed
// once, no stock panel; DLOADGAME2 in its box) and the save menu (0x493060, the same layout with
// flags 0x880 at its own position, DSAVEGAME2). GAMES lists the saved games; selecting one
// (0x491EC0) shows its Summary: GAMENAME takes its description, RADAR its "Radar Image", GAMETYPE
// "???" (no players) / "Single" (campaign) / "Skirmish (<n> players)", SIDE the side's name (first
// letter kept, the rest lower case), MISSION the campaign mission or the map, DIFF Easy / Medium /
// Hard, TIME the game time as hh:mm:ss. Load: DELETE, GAMENAME and the SaveGame picture are off;
// OK / Enter / a double click load the game (sound SMLBUTTON; a file of another game type shows
// "Invalid savegame file"), Cancel closes ("Previous"). Save: GAMENAME has the focus and takes
// letters, digits, '_', ' ' and '\'' (20 at most); OK / Enter / a double click write
// <dir>\<name>.sav with that name as its description and close ("smlbutton"; an empty name writes
// nothing) — an existing file of that name is replaced without a question; Delete Game removes the
// selected file at once (SmallButton) and lists again; Cancel closes ("Previous").
class SaveGamesScreen : public client::LayoutScreen {
public:
    SaveGamesScreen(MenuArt& art, std::string save_dir, client::BattleSession* session, bool save, render::ClipRect under)
        : client::LayoutScreen(art.layout("LOADGAME")), art_(art), dir_(std::move(save_dir)), session_(session), save_(save), under_(under) {
        background_in_box_ = true;
        if (save_) background_ = art_.picture("DSAVEGAME2");
        else open_centred_ = -1;
        if (session_) {
            // In a battle the interface colours are the battle's (from the game palette).
            for (int k = 0; k < 256; ++k) battle_colours_[static_cast<std::size_t>(k)] = art_.gui_colour(art_.content().palette, k);
            colours_ = &battle_colours_;
        }
        field_escape_clears_ = true;
        if (save_) {
            std::error_code ec;
            std::filesystem::create_directories(dir_, ec);
        }
        relist();
        if (client::Widget* w = widget("GAMENAME")) {
            if (save_) w->attribs |= 2;
            else w->active = false;
        }
        if (!save_) focus_ = -1;  // no field takes the keys in the load menu
        if (client::Widget* w = widget("DELETE")) w->active = save_ && !files_.empty();
        if (client::Widget* w = widget("SaveGame")) w->active = save_;
        show();
    }
    ~SaveGamesScreen() override {
        if (paused_ && session_) session_->client().set_paused(false);
    }
    bool see_through() const override { return true; }
    const render::Palette* palette() const override { return nullptr; }
    void frame(client::FrontEnd& fe, std::uint32_t now) override {
        // The battle holds while the menu is open (GS+0x38A51 bit 0, set by both menus; the game
        // menu that opened it lets go when it closes, after this screen came up).
        if (session_ && !session_->network()) {
            session_->client().set_paused(true);
            paused_ = true;
        }
        client::LayoutScreen::frame(fe, now);
    }
    void draw(client::FrontEnd& fe, render::Image8& out) override {
        if (!placed_) {
            place(out.width, out.height);
            if (save_)
                for (std::size_t i = 0; i < widgets_.size(); ++i)
                    if (widgets_[i].kind == client::WidgetKind::TextField && widgets_[i].active) {
                        // The name field takes the focus and the keys (0x4A7190).
                        focus_gadget_ = focus_ = static_cast<int>(i);
                        widgets_[i].cursor = static_cast<int>(widgets_[i].text.size());
                    }
        }
        dim_box(fe, out);
        client::LayoutScreen::draw(fe, out);
    }

protected:
    void dim_box(client::FrontEnd& fe, render::Image8& out) {
        const std::vector<std::uint8_t>& shade = fe.menu_content().shade;
        if (shade.size() < 0x2000) return;
        const int x0 = std::max(under_.x0, 0), y0 = std::max(under_.y0, 0), x1 = std::min(under_.x1, out.width - 1), y1 = std::min(under_.y1, out.height - 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                std::uint8_t& p = out.row(y)[x];
                p = shade[static_cast<std::size_t>(8 * 256 + static_cast<std::int8_t>(p))];
            }
    }
    void relist() {
        files_ = list_save_files(dir_);
        std::vector<std::string> descs;
        for (const SaveFile& f : files_) descs.push_back(f.description);
        if (client::Widget* l = widget("GAMES")) set_list(*l, std::move(descs));
    }
    // The selected save's summary on the labels (0x491EC0).
    void show() {
        client::Widget* l = widget("GAMES");
        if (!l) return;
        auto label = [&](const char* name, const std::string& text) {
            if (client::Widget* w = widget(name)) w->text = text;
        };
        const int sel = l->selected;
        Bank bank;
        std::string err;
        const bool have = sel >= 0 && sel < static_cast<int>(files_.size()) && !files_[static_cast<std::size_t>(sel)].description.empty();
        if (have && read_bank(read_file(files_[static_cast<std::size_t>(sel)].path), bank, err) && bank.find("Summary")) {
            const BankSection& s = *bank.find("Summary");
            if (client::Widget* g = widget("GAMENAME")) {
                g->text = files_[static_cast<std::size_t>(sel)].description;
                if (focus_ >= 0 && &widgets_[static_cast<std::size_t>(focus_)] == g) g->cursor = static_cast<int>(g->text.size());
            }
            // The radar image: width, height, then the rows; the picture is shown when all rows are there.
            radar_.reset();
            if (const BankSection::Blob* b = s.find_blob("Radar Image"); b && b->data.size() >= 8) {
                std::int32_t w = 0, h = 0;
                std::memcpy(&w, b->data.data(), 4);
                std::memcpy(&h, b->data.data() + 4, 4);
                if (w >= 0 && h >= 0 && b->data.size() >= 8 + static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {
                    radar_ = std::make_unique<render::Sprite8>();
                    radar_->width = w;
                    radar_->height = h;
                    radar_->has_key = false;
                    radar_->pixels.assign(b->data.begin() + 8, b->data.begin() + 8 + static_cast<std::ptrdiff_t>(w) * h);
                }
            }
            if (client::Widget* r = widget("RADAR")) r->active = radar_ != nullptr;
            const std::int32_t players = s.get_int("Players", 0), type = s.get_int("Gametype", 0);
            label("GAMETYPE", players == 0 ? "???" : type == 1 ? "Single" : "Skirmish (" + std::to_string(players) + " players)");
            if (const std::string* m = s.find_string(type == 1 ? "Mission" : "Map")) label("MISSION", *m);
            const std::int32_t t = s.get_int("Game Time", 0);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", t / 108000, t / 1800 % 60, t / 30 % 60);
            label("TIME", buf);
            const std::int32_t side = s.get_int("Side", 0);
            std::string side_text = "???";
            if (side >= 0 && side < static_cast<int>(art_.sides().size())) {
                side_text = art_.sides()[static_cast<std::size_t>(side)];
                for (std::size_t k = 1; k < side_text.size(); ++k) side_text[k] = static_cast<char>(side_text[k] + ' ');
            }
            label("SIDE", side_text);
            static const char* const kDifficulty[3] = {"Easy", "Medium", "Hard"};
            const std::int32_t d = s.get_int("Difficulty", 0);
            label("DIFF", d >= 0 && d < 3 ? kDifficulty[d] : "");
            return;
        }
        if (client::Widget* g = widget("GAMENAME")) g->text.clear();
        for (const char* n : {"SIDE", "DIFF", "MISSION", "GAMETYPE", "TIME"}) label(n, "");
    }
    void on_list_select(client::FrontEnd&, client::Widget&) override { show(); }
    void on_text(client::FrontEnd& fe, client::Widget& w) override { on_press(fe, w); }
    void draw_surface(client::FrontEnd&, render::Image8& out, const client::Widget& w) override {
        if (!radar_ || !is_named(w, "RADAR")) return;
        const client::Widget& h = widgets_.front();
        const render::ClipRect box{std::max(ox_, 0), std::max(oy_, 0), std::min(ox_ + h.w - 1, out.width - 1), std::min(oy_ + h.h - 1, out.height - 1)};
        draw_box_picture(out, box, *radar_, ox_ + w.x, oy_ + w.y, w.w, w.h);
    }
    static bool is_named(const client::Widget& w, const char* n) { return stricmp_c(w.name, n) == 0; }
    void on_press(client::FrontEnd& fe, client::Widget& w) override {
        if (is_named(w, "CANCEL")) {
            fe.play_sound(art_.sound("Previous"));
            fe.pop();
            return;
        }
        if (save_ && is_named(w, "DELETE")) {
            fe.play_sound(art_.sound("SmallButton"));
            const client::Widget* l = widget("GAMES");
            if (l && l->selected >= 0 && l->selected < static_cast<int>(files_.size())) {
                std::error_code ec;
                std::filesystem::remove(files_[static_cast<std::size_t>(l->selected)].path, ec);
            }
            relist();
            show();
            return;
        }
        const bool act = is_named(w, "GAMES") || is_named(w, "LOAD") || (save_ && is_named(w, "GAMENAME"));
        if (!act) return;
        if (save_) save(fe);
        else load(fe);
    }

private:
    void save(client::FrontEnd& fe) {
        fe.play_sound(art_.sound("smlbutton"));
        const client::Widget* g = widget("GAMENAME");
        const std::string name = g ? g->text : std::string();
        if (!name.empty() && session_) {
            SkirmishSession* sk = dynamic_cast<SkirmishSession*>(session_);
            CampaignProgress* progress = nullptr;
            if (auto* ms = dynamic_cast<MissionSession*>(session_)) {
                sk = &ms->inner();
                progress = &ms->progress();
            }
            if (sk) {
                const SaveContext c = battle_save_context(*sk, progress, name);
                std::vector<std::uint8_t> bytes;
                write_save_file(sk->world().loaded(), c, bytes);
                write_file(dir_, name, bytes);
            }
        }
        fe.pop();
    }
    void load(client::FrontEnd& fe) {
        const client::Widget* l = widget("GAMES");
        if (!l || l->selected < 0 || l->selected >= static_cast<int>(files_.size())) return;
        const std::vector<std::uint8_t> bytes = read_file(files_[static_cast<std::size_t>(l->selected)].path);
        auto save = std::make_shared<SavedGame>();
        std::string message;
        const client::Widget& hd = widgets_.front();
        const render::ClipRect box{ox_, oy_, ox_ + hd.w - 1, oy_ + hd.h - 1};
        if (!read_saved_game(bytes, *save, message) || (save->summary.game_type != 1 && save->summary.game_type != 2)) {
            show_message(fe, art_, "Invalid savegame file", &box);
            return;
        }
        fe.play_sound(art_.sound("SMLBUTTON"));
        const SaveSummary& m = save->summary;
        std::shared_ptr<CampaignProgress> progress;
        if (m.game_type == 1) {
            Campaign c;
            if (!load_campaign(art_.vfs(), m.campaign, c, message)) {
                show_message(fe, art_, "Invalid savegame file", &box);
                return;
            }
            progress = std::make_shared<CampaignProgress>();
            progress->campaign = m.campaign;
            progress->side = m.side == 0 ? "ARM" : "CORE";
            progress->difficulty = m.difficulty;
            progress->results = progress_from_thumbs(m.thumbs);
            progress->mission = -1;
            for (std::size_t k = 0; k < c.missions.size(); ++k)
                if (stricmp_c(c.missions[k].name, m.mission) == 0) progress->mission = static_cast<int>(k);
            if (progress->mission < 0) {
                show_message(fe, art_, "Invalid savegame file", &box);
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
    std::string dir_;
    client::BattleSession* session_ = nullptr;
    bool save_ = false;
    render::ClipRect under_{};
    bool paused_ = false;
    std::vector<SaveFile> files_;
    std::unique_ptr<render::Sprite8> radar_;
    std::array<std::uint8_t, 256> battle_colours_{};
};

// The screen under the load / save menu, dimmed when it opens (open flag 0x800): in a battle the
// side panel's box, else the whole screen (the menus' screens fill it).
render::ClipRect screen_under(client::FrontEnd& fe, client::BattleSession* in_battle) {
    if (in_battle) return {0, 128, 127, fe.height() - 1};
    return {0, 0, fe.width() - 1, fe.height() - 1};
}

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
    // The radar image: the minimap with its marks (GS+0x142DB, written by 0x4C6F10: width, height, rows).
    const render::Image8& mm = s.client().minimap_image();
    c.radar_width = mm.width;
    c.radar_height = mm.height;
    c.radar_pixels = mm.pixels;
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
    // The original rescans the archives when New Campaign is pressed (0x4775C9) and when the
    // load-game dialog closes (0x49257A; its save listing cannot see archives, so rescanning
    // as it opens is the same).
    hooks.campaign_menu = [&art, save_dir](client::FrontEnd&) -> std::unique_ptr<client::Screen> {
        mount_cd_archives(art.vfs(), art.game_dir());
        return std::make_unique<CampaignScreen>(art, save_dir);
    };
    hooks.load_game_menu = [&art, save_dir](client::FrontEnd& fe, client::BattleSession* s) -> std::unique_ptr<client::Screen> {
        mount_cd_archives(art.vfs(), art.game_dir());
        // No saved games: the message instead (0x4931D0).
        if (list_save_files(save_dir).empty()) {
            show_message(fe, art, "There are no saved games to choose from");
            return nullptr;
        }
        return std::make_unique<SaveGamesScreen>(art, save_dir, s, false, screen_under(fe, s));
    };
    hooks.save_game_menu = [&art, save_dir](client::FrontEnd& fe, client::BattleSession& s) -> std::unique_ptr<client::Screen> {
        return std::make_unique<SaveGamesScreen>(art, save_dir, &s, true, screen_under(fe, &s));
    };
}

}  // namespace ta

#else

ta::SaveContext ta::battle_save_context(SkirmishSession&, const CampaignProgress*, const std::string&) { return {}; }
void ta::install_campaign_hooks(client::FrontEndHooks&, MenuArt&, const std::string&) {}

#endif
