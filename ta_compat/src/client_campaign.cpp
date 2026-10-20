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
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "client/audio.h"
#include "client/canvas.h"
#include "sim/battle.h"
#include "sim/victory.h"
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

// A mission battle: the front end's battle session with the campaign's progression.
class MissionSession : public client::BattleSession {
public:
    MissionSession(std::unique_ptr<SkirmishSession> inner, MenuArt& art, std::string save_dir, std::shared_ptr<CampaignProgress> progress,
                   MissionInfo info = {})
        : inner_(std::move(inner)), art_(art), save_dir_(std::move(save_dir)), progress_(std::move(progress)), info_(std::move(info)) {}
    sim::Battle& battle() override { return inner_->battle(); }
    client::Content& content() override { return inner_->content(); }
    client::GameClient& client() override { return inner_->client(); }
    int frame(std::uint32_t now_ms) override {
        // (tests: TA_TEST_MISSION_END=win:STEP or lose:STEP ends the mission at that step as its
        // goals would, so the end screens can be checked without playing it)
        if (const char* t = std::getenv("TA_TEST_MISSION_END")) {
            const std::string v = t;
            const auto c = v.find(':');
            sim::Battle& b = battle();
            if (c != std::string::npos && b.game_tick() >= std::strtoul(v.c_str() + c + 1, nullptr, 10) &&
                !(b.end_game.flags & sim::end_game_flags::kEnding)) {
                if (v.compare(0, c, "win") == 0) b.end_game.flags |= sim::end_game_flags::kWon | sim::end_game_flags::kEnding;
                else b.end_game.flags |= sim::end_game_flags::kLost | sim::end_game_flags::kEnding;
            }
        }
        return inner_->frame(now_ms);
    }
    client::BattleOutcome outcome() override { return inner_->outcome(); }
    void send_chat(const std::string& t) override { inner_->send_chat(t); }
    void surrender() override { inner_->surrender(); }
    std::vector<client::PlayerStats> stats() override { return inner_->stats(); }
    bool end_table(client::EndTable& t) override { return inner_->end_table(t); }
    std::string battle_name() const override { return inner_->battle_name(); }
    int difficulty() const override { return inner_->difficulty(); }
    bool after_battle(client::FrontEnd& fe, client::BattleOutcome outcome) override;
    // The exit menu's Restart (0x460340): the same mission again at the dialog's difficulty
    // (its schema chosen anew), defined after mission_launcher.
    client::BattleLauncher restart_launcher(int difficulty) override;
    SkirmishSession& inner() { return *inner_; }
    CampaignProgress& progress() { return *progress_; }
    std::shared_ptr<CampaignProgress> progress_ptr() { return progress_; }
    MissionInfo& info() { return info_; }
    const std::string& save_dir() const { return save_dir_; }
    client::ClientOptions options;  // the client options the battle was started with (Restart)

private:
    std::unique_ptr<SkirmishSession> inner_;
    MenuArt& art_;
    std::string save_dir_;
    std::shared_ptr<CampaignProgress> progress_;
    MissionInfo info_;
};

client::BattleLauncher mission_launcher(MenuArt& art, const std::string& save_dir, std::shared_ptr<CampaignProgress> progress, const MissionInfo& info,
                                        client::FrontEnd& fe) {
    const client::ClientOptions options = client_options(fe.settings(), fe.width(), fe.height());
    MissionRequest rq;
    rq.campaign_side = progress->side;
    rq.difficulty = progress->difficulty;
    rq.battle_rng_seed = clock_value();
    rq.loading_rand_seed = static_cast<std::uint32_t>(std::time(nullptr));
    // (the program's one C runtime rand() goes on into the battle, as for a skirmish)
    rq.crt_rand_state = menus_rand().state();
    return [&art, save_dir, progress, info, rq, options](const client::LoadProgress& lp, std::string& error) -> std::unique_ptr<client::BattleSession> {
        const BattleRequest req = make_mission_request(art.vfs(), info, rq);
        std::unique_ptr<SkirmishSession> s = SkirmishSession::start(art.game_dir(), req, 0, options, lp, error);
        if (!s) return nullptr;
        auto m = std::make_unique<MissionSession>(std::move(s), art, save_dir, progress, info);
        m->options = options;
        return m;
    };
}

client::BattleLauncher MissionSession::restart_launcher(int difficulty) {
    // The same mission from its start at the difficulty the restart dialog shows; the mission's
    // file read again for it (its schema per difficulty), the clocks' inputs taken anew and the
    // program's rand() going on from where the battle left it.
    progress_->difficulty = difficulty;
    Campaign c;
    std::string err;
    MissionInfo info;
    if (load_campaign(art_.vfs(), progress_->campaign, c, err) && progress_->mission >= 0 &&
        progress_->mission < static_cast<int>(c.missions.size()))
        load_mission(art_.vfs(), c.missions[static_cast<std::size_t>(progress_->mission)].file, difficulty, info, err);
    if (info.file.empty()) return {};
    MissionRequest rq;
    rq.campaign_side = progress_->side;
    rq.difficulty = difficulty;
    rq.battle_rng_seed = clock_value();
    rq.loading_rand_seed = static_cast<std::uint32_t>(std::time(nullptr));
    rq.crt_rand_state = battle().crt_rand.state();
    MenuArt& art = art_;
    const std::string save_dir = save_dir_;
    std::shared_ptr<CampaignProgress> progress = progress_;
    const client::ClientOptions opt = options;
    return [&art, save_dir, progress, info, rq, opt](const client::LoadProgress& lp, std::string& error) -> std::unique_ptr<client::BattleSession> {
        const BattleRequest req = make_mission_request(art.vfs(), info, rq);
        std::unique_ptr<SkirmishSession> s = SkirmishSession::start(art.game_dir(), req, 0, opt, lp, error);
        if (!s) return nullptr;
        auto m = std::make_unique<MissionSession>(std::move(s), art, save_dir, progress, info);
        m->options = opt;
        return m;
    };
}

// --- The mission briefing (MSNBRIEF.GUI): set-up 0x478E80, handler 0x478CB0; each pass the
// planet (0x478B40) and the panorama (0x478790, which also draws the wind and gravity and the
// blinking words, 0x4AFF00); the text: 0x476D80 (set-up), 0x4AC4C0 (word wrap), 0x476CD0
// (colour spans carried over line breaks), 0x476EF0 (one page as label lines).
//   * The picture bitmaps\mbrief<side prefix>; the interface colours from the game palette.
//   * The planet's name (map info "Planet"; "Lunar" becomes "Lunar2" for Core) picks one of 15
//     planets: anims\<planet>brief.GAF holds the rotating planet (<..>Rotate), the panorama strip
//     (<..>Pan) and the frame around it (Panmask, one frame per side). Unknown names: Green.
//   * PLANET: every 25 ms of the wall clock: the narration's end turns the narration switch off,
//     the rotation steps by its frames' durations; the frame centred in the gadget on colour 0.
//   * PANORAMA: the wind shown (starting at rand() % (max - min + 1) + min) moves by rand() % 5 - 2
//     whenever its countdown (first rand() % 64, then rand() % 63 passes) runs out, kept within
//     min..max; "Wind Speed : %d" and "Gravity : %.1f" (gravity / 112) in the side's font and
//     colour in SOLARSYSTEM's box (filled with colour 0) at +80, +20 / +40; the blinking words;
//     the strip of all frames (and the first again) from x - scroll, the scroll one pixel on every
//     third tick, wrapping at the strip's width (kept from one briefing to the next); Panmask's
//     side frame over the whole screen.
//   * The text: camps\briefs\<brief>.txt wrapped to TextRegion's width in the side's font with
//     CR LF breaks, "&X..&" spans (X: G, Y, R) restarted after each break inside them; a page is
//     TextRegion.h / (font height + 2) lines from row (font height + 2) / 2, at x + 5, in the
//     side's colour; each span also blinks over its line: its colour (G, Y, R) for a second,
//     colour 0x5E for a quarter. MOREBAR: "MORE...", "BACK TO START" (last page of several) or
//     nothing, in the side's second colour; a click on it or the text shows the next page ("More").
//   * Narration (camps\briefs\<narration>.wav) starts with the screen; the switch (SHUTUP, on)
//     stops it or starts it again ("Options", then "SmallButton").
//   * Start: "BigButton", the narration stops, the battle. Previous: the narration stops,
//     "Previous", the new campaign screen again.
class BriefingScreen : public client::LayoutScreen {
public:
    BriefingScreen(MenuArt& art, std::string save_dir, std::shared_ptr<CampaignProgress> progress)
        : client::LayoutScreen(art.layout("MSNBRIEF")), art_(art), save_dir_(std::move(save_dir)), progress_(std::move(progress)) {}

    void enter(client::FrontEnd& fe) override {
        if (set_up_) return;
        set_up_ = true;
        Campaign c;
        const int index = progress_->mission;
        std::string error;
        ok_ = load_campaign(art_.vfs(), progress_->campaign, c, error) && index >= 0 && index < static_cast<int>(c.missions.size()) &&
              load_mission(art_.vfs(), c.missions[static_cast<std::size_t>(index)].file, progress_->difficulty, info_, error);
        side_ = side_index(art_.vfs(), progress_->side) == 1 ? 1 : 0;
        // (0x4288D0 with its third argument set: the picture's palette goes on the display)
        background_ = art_.picture("mbrief" + side_prefix(), &palette_);
        for (int k = 0; k < 256; ++k) colours_table_[static_cast<std::size_t>(k)] = art_.gui_colour(art_.content().palette, k);
        colours_ = &colours_table_;
        for (const char* n : {"MOREBAR", "TextRegion"})
            if (client::Widget* w = widget(n)) w->attribs &= ~0x10;
        if (client::Widget* s = widget("SHUTUP")) s->stage = 1;
        // The fonts of the screen's FONT gadgets: 0 smlfont, 1 armfont, 2 corefont.
        std::vector<std::string> fonts;
        if (const client::ScreenLayout* L = layout())
            for (const client::Widget& w : L->widgets)
                if (w.kind == client::WidgetKind::FontRef) fonts.push_back(w.text);
        small_font_ = art_.fnt_font("smlfont");
        side_font_ = art_.fnt_font(side_ == 0 ? "armfont" : "corefont");
        // The wind shown and its countdown.
        sim::Lcg15& rnd = menus_rand();
        // (tests: TA_BRIEF_RAND=S puts rand() on state S here, as gui_harness's srand:S does)
        if (const char* t = std::getenv("TA_BRIEF_RAND")) rnd.set_state(static_cast<std::uint32_t>(std::strtoul(t, nullptr, 0)));
        const int span = info_.max_wind - info_.min_wind + 1;
        const int r1 = static_cast<int>(rnd.next());
        wind_ = span != 0 ? r1 % span + info_.min_wind : info_.min_wind;
        countdown_ = static_cast<int>(rnd.next()) % 64;
        load_planet();
        now_tick_ = fe.now_ms() * 30 / 1000;
        if (ok_ && !info_.brief_text.empty() && side_font_) {
            const client::Widget* t = widget("TextRegion");
            text_ = carry_spans(wrap_crlf(*side_font_, info_.brief_text, t ? t->w : 360));
            page_ = -1;
            paginate();
        }
        start_narration(fe);
    }

    void frame(client::FrontEnd& fe, std::uint32_t now) override {
        client::LayoutScreen::frame(fe, now);
        const std::uint32_t tick = now * 30 / 1000;
        now_tick_ = tick;
        // PLANET (0x478B40): every 25 ms of the wall clock.
        if (now >= planet_next_) {
            planet_next_ = now + 25;
            if (now >= narration_end_)
                if (client::Widget* s = widget("SHUTUP"); s && s->stage != 0) s->stage = 0;
            if (rotate_ && !rotate_times_.empty() && tick != 0) {
                // The frame cursor (0x4B8B90): a countdown below 2 moves to the next frame.
                if (cursor_wait_ < 2) {
                    if (++cursor_frame_ >= static_cast<int>(rotate_times_.size())) cursor_frame_ = rotate_loop_ ? 0 : static_cast<int>(rotate_times_.size());
                    if (cursor_frame_ < static_cast<int>(rotate_times_.size())) cursor_wait_ = rotate_times_[static_cast<std::size_t>(cursor_frame_)];
                } else {
                    --cursor_wait_;
                }
            }
        }
        // PANORAMA (0x478790): once per pass.
        if (pan_) {
            sim::Lcg15& rnd = menus_rand();
            if (--countdown_ < 1) {
                wind_ += static_cast<int>(rnd.next()) % 5 - 2;
                if (wind_ < info_.min_wind) wind_ = info_.min_wind;
                if (wind_ > info_.max_wind) wind_ = info_.max_wind;
                countdown_ = static_cast<int>(rnd.next()) % 63;
            }
            int strip = 0;
            for (const render::Sprite8& f : *pan_) strip += f.width;
            if (static_cast<std::int32_t>(pan_next_) < static_cast<std::int32_t>(tick)) {
                if (++pan_scroll_ >= strip) pan_scroll_ = 0;
                pan_next_ = tick + 2;
            }
        }
        // The blinking words (0x4AFF00): one second in their colour, a quarter in 0x5E.
        for (Blink& b : blinks_)
            if (b.next < static_cast<float>(tick)) {
                b.next = static_cast<float>(tick) + 30.0f * (b.off ? 1.0f : 0.25f);
                b.off = !b.off;
            }
    }

    void draw(client::FrontEnd& fe, render::Image8& out) override {
        client::LayoutScreen::draw(fe, out);
        const render::ClipRect all = out.bounds();
        // The page's lines and MOREBAR.
        if (side_font_)
            for (const Line& l : lines_) client::draw_text(out, all, *side_font_, l.text, l.x, l.y, kSideColour[side_][0], -1, 640 - l.x - 5);
        if (const client::Widget* m = widget("MOREBAR"); m && small_font_ && !more_text_.empty()) {
            // (0x4A56B0 measures a centred label's text in the interface's picture font while a
            // picture font is set, then draws it in the label's bit font)
            const client::Font* measure = fe.menu_content().small_font ? fe.menu_content().small_font : small_font_;
            const int tx = m->x + m->w / 2 - measure->text_width(more_text_) / 2;
            client::draw_text(out, all, *small_font_, more_text_, tx, m->y, kSideColour[side_][1], -1, m->w);
        }
        // PLANET.
        if (const client::Widget* p = widget("PLANET")) {
            client::fill_rect(out, all, p->x, p->y, p->x + p->w - 1, p->y + p->h - 1, colours_table_[static_cast<std::size_t>(p->bg & 0xFF)]);
            if (rotate_ && cursor_frame_ < static_cast<int>(rotate_->size())) {
                const render::Sprite8& f = (*rotate_)[static_cast<std::size_t>(cursor_frame_)];
                render::blit(out, all, f, p->w / 2 - (f.width >> 1) + p->x + f.x_offset, p->h / 2 - (f.height >> 1) + p->y + f.y_offset);
            }
        }
        // PANORAMA: the wind and gravity, the blinking words, the strip, the frame.
        if (const client::Widget* g = widget("PANORAMA"); g && pan_) {
            if (const client::Widget* s = widget("SOLARSYSTEM")) {
                client::fill_rect(out, all, s->x, s->y, s->x + s->w - 1, s->y + s->h - 1, colours_table_[0]);
                if (side_font_) {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "%s : %d", "Wind Speed", wind_);
                    client::draw_text(out, all, *side_font_, buf, s->x + 80, s->y + 20, kSideColour[side_][0], -1, s->w - 1 - 80);
                    std::snprintf(buf, sizeof buf, "%s : %.1f", "Gravity", static_cast<double>(info_.gravity) * 0.008928571428571428);
                    client::draw_text(out, all, *side_font_, buf, s->x + 80, s->y + 40, kSideColour[side_][0], -1, s->w - 1 - 80);
                }
            }
            if (side_font_)
                for (const Blink& b : blinks_)
                    client::draw_text(out, all, *side_font_, b.text, b.x, b.y, b.off ? 0x5E : b.colour, -1, -1);
            client::fill_rect(out, all, g->x, g->y, g->x + g->w - 1, g->y + g->h - 1, colours_table_[static_cast<std::size_t>(g->bg & 0xFF)]);
            int x = g->x - pan_scroll_;
            const int n = static_cast<int>(pan_->size());
            for (int k = 0; k <= n; ++k) {
                const render::Sprite8& f = (*pan_)[static_cast<std::size_t>(k % n)];
                render::blit(out, all, f, x + f.x_offset, g->y + f.y_offset);
                x += f.width;
            }
            if (mask_ && side_ < static_cast<int>(mask_->size())) render::blit(out, all, (*mask_)[static_cast<std::size_t>(side_)], (*mask_)[static_cast<std::size_t>(side_)].x_offset, (*mask_)[static_cast<std::size_t>(side_)].y_offset);
        }
    }

    void handle(client::FrontEnd& fe, const client::InputEvent& e) override {
        // A click on the text or MOREBAR (labels the pointer reaches through the interface pass).
        if (e.type == client::InputEvent::Type::MouseDown && e.button == 1) text_down_ = over_text(e.x, e.y);
        if (e.type == client::InputEvent::Type::MouseUp && e.button == 1 && text_down_ && over_text(e.x, e.y)) more(fe);
        if (e.type == client::InputEvent::Type::MouseUp) text_down_ = false;
        client::LayoutScreen::handle(fe, e);
    }

protected:
    void draw_surface(client::FrontEnd&, render::Image8&, const client::Widget&) override {}
    void on_press(client::FrontEnd& fe, client::Widget& w) override {
        if (named(w, "Start")) {
            fe.play_sound(art_.sound("BigButton"));
            if (!ok_) return;
            mount_cd_archives(art_.vfs(), art_.game_dir());
            fe.stop_narration();
            fe.start_battle(mission_launcher(art_, save_dir_, progress_, info_, fe));
        } else if (named(w, "SHUTUP")) {
            fe.play_sound(art_.sound("Options"));
            if (w.stage == 0) {
                fe.stop_narration();
                narration_end_ = 0;
            } else {
                start_narration(fe);
            }
            fe.play_sound(art_.sound("SmallButton"));
        } else if (named(w, "PrevMenu")) {
            fe.stop_narration();
            fe.play_sound(art_.sound("Previous"));
            fe.pop();
        }
    }

private:
    struct Line {
        std::string text;
        int x = 0, y = 0;
    };
    struct Blink {
        std::string text;
        int x = 0, y = 0;
        std::uint8_t colour = 0;
        bool off = false;
        float next = 0;
    };
    // Per side: the text colour, then the G, Y and R spans' colours (0x507B70).
    static constexpr std::uint8_t kSideColour[2][4] = {{0x35, 0x33, 0x40, 0xD0}, {0x75, 0x56, 0x52, 0xD4}};

    static bool named(const client::Widget& w, const char* n) { return stricmp_c(w.name, n) == 0; }
    std::string side_prefix() {
        std::vector<std::uint8_t> sd;
        if (art_.vfs().read("gamedata\\sidedata.tdf", sd)) {
            const TdfParseResult r = tdf_parse(std::string_view(reinterpret_cast<const char*>(sd.data()), sd.size()));
            if (r.root)
                if (const TdfSection* s = r.root->child("SIDE" + std::to_string(side_))) return s->get_string("nameprefix", 32, "");
        }
        return side_ == 0 ? "ARM" : "COR";
    }
    void load_planet() {
        static const char* const kNames[] = {"Green planet", "Archipelago", "Wet Desert", "Desert", "Lava", "Red Planet", "Lunar", "Metal",
                                             "Lunar2", "Ice", "Lush", "Slate", "Water World", "Acid", "Crystal"};
        static const char* const kBrief[] = {"Greenbrief", "Archibrief", "WDesertbrief", "Desertbrief", "Lavabrief", "Marsbrief", "Lunarbrief", "Metalbrief",
                                             "Lunar2brief", "Icebrief", "Lushbrief", "Slatebrief", "Waterbrief", "Acidbrief", "Crystalbrief"};
        static const char* const kPan[] = {"GreenPan", "ArchiPan", "WDesPan", "DDesPan", "LavaPan", "MarsPan", "LunarPan", "MetalPan",
                                           "Lunar2Pan", "IcePan", "LushPan", "SlatePan", "WaterPan", "AcidPan", "CrystPan"};
        static const char* const kRotate[] = {"GreenRotate", "ArchiRotate", "WDesertRotate", "DDesRotate", "LavaRotate", "MarsRotate", "LunarRotate", "MetalRotate",
                                              "Lunar2Rotate", "IceRotate", "LushRotate", "SlateRotate", "WaterRotate", "AcidRotate", "CrystalRotate"};
        std::string planet = info_.planet;
        if (planet == "Lunar" && side_ != 0) planet += "2";
        int k = 0;
        for (int i = 0; i < 15; ++i)
            if (planet == kNames[i]) {
                k = i;
                break;
            }
        const std::string gaf = std::string("anims\\") + kBrief[k] + ".GAF";
        pan_ = art_.gaf_sequence(gaf, kPan[k]);
        rotate_ = art_.gaf_sequence(gaf, kRotate[k]);
        rotate_times_ = art_.sequence_times(gaf, kRotate[k], &rotate_loop_);
        mask_ = art_.gaf_sequence(gaf, "Panmask");
        cursor_frame_ = 0;
        cursor_wait_ = rotate_times_.empty() ? 0 : rotate_times_[0];
    }
    void start_narration(client::FrontEnd& fe) {
        if (info_.narration.empty()) return;
        if (narration_.pcm.empty()) {
            std::vector<std::uint8_t> wav;
            if (!art_.vfs().read("camps\\briefs\\" + info_.narration + ".wav", wav) || !client::decode_wav(wav, narration_)) return;
        }
        fe.play_narration_sample(&narration_);
        const std::size_t frames = narration_.pcm.size() / static_cast<std::size_t>(std::max(1, narration_.channels));
        narration_end_ = fe.now_ms() + static_cast<std::uint32_t>(frames * 1000 / static_cast<std::size_t>(std::max(1, narration_.rate)));
    }
    bool over_text(int x, int y) {
        for (const char* n : {"TextRegion", "MOREBAR"})
            if (const client::Widget* w = widget(n); w && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) return true;
        return false;
    }
    void more(client::FrontEnd& fe) {
        if (text_.empty()) return;
        fe.play_sound(art_.sound("More"));
        paginate();
    }

    // 0x4AC4C0: the text broken for `width` in `font`: when the next character is a space, a line
    // feed or a hyphen and the line so far (its own characters) is at least `width` wide, the
    // line goes back to its last space or hyphen, which becomes CR LF; a line feed starts a line.
    static std::string wrap_crlf(const client::Font& font, const std::string& src, int width) {
        std::string out;
        std::size_t line = 0, i = 0;
        auto at = [&](std::size_t k) -> unsigned char { return k < src.size() ? static_cast<unsigned char>(src[k]) : 0; };
        while (at(i) != 0 && at(i) != 0xFF) {
            out.push_back(static_cast<char>(at(i)));
            const unsigned char next = at(i + 1);
            std::size_t n = i + 1;
            if (next == ' ' || next == '\n' || next == '-') {
                if (font.text_width(std::string_view(out).substr(line)) >= width) {
                    std::size_t j = i;
                    while (at(j) != ' ' && at(j) != '-') {
                        out.pop_back();
                        if (j == 0) break;
                        --j;
                    }
                    if (!out.empty()) out.back() = '\r';
                    out.push_back('\n');
                    line = out.size();
                    n = j + 1;
                }
            }
            if (at(n) == '\n') line = out.size() + 1;
            i = n;
        }
        return out;
    }
    // 0x476CD0: a line feed inside a "&X..&" span ends the span before the break and opens it again
    // after it (the character before the line feed, the CR, becomes the closing '&').
    static std::string carry_spans(const std::string& in) {
        std::string out;
        bool open = false;
        char colour = 0;
        for (std::size_t i = 0; i < in.size() && static_cast<unsigned char>(in[i]) != 0xFF; ++i) {
            const char c = in[i];
            out.push_back(c);
            if (c == '&') {
                colour = i + 1 < in.size() ? in[i + 1] : '\0';
                open = !open;
            }
            if (c == '\n' && open) {
                if (out.size() >= 2) out[out.size() - 2] = '&';
                out.back() = '\r';
                out.push_back('\n');
                out.push_back('&');
                out.push_back(colour);
            }
        }
        return out;
    }
    // 0x476EF0: the next page (after the last: the first) as lines and blinking words.
    void paginate() {
        const client::Widget* t = widget("TextRegion");
        if (!t || !side_font_ || text_.empty()) return;
        const int row = side_font_->height + 2;
        const int lines = t->h / row;
        auto at = [&](std::size_t k) -> unsigned char { return k < text_.size() ? static_cast<unsigned char>(text_[k]) : 0; };
        // The position after the n-th line feed (npos: the text ends first).
        auto after_lines = [&](int n) -> std::size_t {
            int seen = 0;
            for (std::size_t k = 0; at(k) != 0 && at(k) != 0xFF; ++k)
                if (at(k) == '\n' && ++seen == n) return k + 1;
            return std::string::npos;
        };
        ++page_;
        std::size_t p = 0;
        if (page_ != 0) {
            p = after_lines(lines * page_);
            if (p == std::string::npos) {
                page_ = 0;
                p = 0;
            }
        }
        const bool more = after_lines((page_ + 1) * lines) != std::string::npos;
        more_text_ = more ? "MORE..." : page_ == 0 ? "" : "BACK TO START";
        lines_.clear();
        blinks_.clear();
        bool expect_open = true;
        int y = row / 2 + t->y;
        const std::uint32_t tick = now_tick_;
        for (int k = 0; k < lines; ++k, y += row) {
            Line l;
            l.x = t->x + 5;
            l.y = y;
            if (at(p) != '\n') {
                for (;;) {
                    unsigned char c = at(p);
                    if (c == 0 || c == 0xFF) break;
                    if (c == '&') {
                        if (expect_open) {
                            const unsigned char letter = at(p + 1);
                            expect_open = false;
                            const int ci = letter == 'R' ? 3 : letter == 'Y' ? 2 : letter == 'G' ? 1 : 3;
                            p += 2;
                            Blink b;
                            b.x = side_font_->text_width(l.text) + t->x + 5;
                            b.y = y;
                            b.colour = kSideColour[side_][ci];
                            for (std::size_t q = p; at(q) != '&' && b.text.size() < 0x7F; ++q) b.text.push_back(static_cast<char>(at(q)));
                            b.next = static_cast<float>(tick) + 30.0f;
                            blinks_.push_back(b);
                        } else {
                            ++p;
                            expect_open = true;
                        }
                    }
                    l.text.push_back(static_cast<char>(at(p)));
                    ++p;
                    if (at(p) == '\n') break;
                }
            }
            ++p;
            lines_.push_back(std::move(l));
        }
    }

    MenuArt& art_;
    std::string save_dir_;
    std::shared_ptr<CampaignProgress> progress_;
    MissionInfo info_;
    const render::Palette* palette_ = nullptr;

public:
    const render::Palette* palette() const override { return palette_ ? palette_ : client::LayoutScreen::palette(); }

private:
    bool set_up_ = false, ok_ = false, text_down_ = false;
    int side_ = 0;
    std::array<std::uint8_t, 256> colours_table_{};
    const client::Font *small_font_ = nullptr, *side_font_ = nullptr;
    int wind_ = 0, countdown_ = 0;
    const std::vector<render::Sprite8>*pan_ = nullptr, *rotate_ = nullptr, *mask_ = nullptr;
    std::vector<int> rotate_times_;
    bool rotate_loop_ = true;
    int cursor_frame_ = 0, cursor_wait_ = 0;
    std::uint32_t planet_next_ = 0, narration_end_ = 0, now_tick_ = 0;
    static inline int pan_scroll_ = 0;              // kept from one briefing to the next (0x51E674)
    static inline std::uint32_t pan_next_ = 0;      // (0x51E678)
    client::Sample narration_;
    std::string text_, more_text_;
    int page_ = -1;
    std::vector<Line> lines_;
    std::vector<Blink> blinks_;
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

// The campaign's end of a mission goes through the statistics screen (ta::campaign_end).
bool MissionSession::after_battle(client::FrontEnd&, client::BattleOutcome) { return false; }

// --- A new campaign (NEWGAME.GUI: set-up 0x478240, handler 0x477AB0). `any` (the AnyMsn button,
// 0x478240(1)): the Missions list too, to start at any mission.
//   * Background: playanygame4 (any mission), else newcampaign4 when camps\ holds more than two
//     campaign files, else newcampaign4x (no campaign list: the side's own "Arm Campaign" /
//     "Core Campaign"). The picture's indices are shown on the palette already on the display.
//   * Any mission: the Campaign list and its knob move to y 308, 48 high, the Missions list is 62
//     high; both lists shown, the Missions list has the focus. Otherwise the Campaign list has the
//     focus (newcampaign4x: the Difficulty switch).
//   * Side: registry "side" (0 Arm, 1 Core): the side's button (Arm / Core) and picture (Side0 /
//     Side1) pressed. The Campaign list holds camps\*.tdf in name order (case-insensitive) whose
//     [HEADER] campaignside is the side's name or "ALL" ("smlbutton" each time it is filled).
//   * Difficulty: registry "Difficulty" (0..2) on the switch; a press steps it ("SmlButton").
//   * Arm / Side0 ("SideSelect"), Core / Side1 ("SideSelect2"): the side, the list again.
//   * Start, Enter on a list or a double click on the Campaign list (the Missions list in any
//     mission mode): "bigButton", the archives rescanned, the results string reset, the campaign
//     and the mission (the first, or the chosen one) loaded, the settings saved, the briefing.
//   * Previous Menu: "Previous", back to the single player menu.
class NewCampaignScreen : public client::LayoutScreen {
public:
    NewCampaignScreen(MenuArt& art, std::string save_dir, bool any) : client::LayoutScreen(art.layout("NEWGAME")), art_(art), save_dir_(std::move(save_dir)), any_(any) {}

    void enter(client::FrontEnd& fe) override {
        if (set_up_ && !again_) return;
        if (again_) {
            // Back from the briefing: the screen is set up anew (state 0xB/0xC sub-state 3 calls
            // 0x478240 again).
            again_ = false;
            widgets_ = layout()->widgets;
            placed_ = false;
        }
        set_up_ = true;
        auto count = static_cast<int>(list_campaigns(art_.vfs()).size());
        no_list_ = !any_ && count <= 2;
        const char* bg = any_ ? "playanygame4" : no_list_ ? "newcampaign4x" : "newcampaign4";
        background_ = art_.picture(bg);
        // (the palette on the display stays the main menu's; the interface colours are the layout's)
        if (any_) {
            // (changed after the screen was opened: the heights are not cut to whole rows again;
            // the slider's arrows sit at its new ends)
            if (client::Widget* w = widget("Campaign")) {
                w->y = 308;
                w->h = 48;
            }
            if (client::Widget* w = widget("Missions")) w->h = 62;
            relocate_slider("CampaignKnob", 308, 48);
        }
        side_ = std::clamp(fe.settings().get_int("side", 0), 0, 1);
        show_side();
        if (client::Widget* d = widget("Difficulty")) d->stage = std::clamp(fe.settings().get_int("Difficulty", 1), 0, 2);
        if (!no_list_ || any_) {
            for (const char* n : {"Campaign", "CampaignKnob"})
                if (client::Widget* w = widget(n)) w->active = true;
            fill_campaigns(fe);
            if (any_) {
                for (const char* n : {"Missions", "MissionsKnob"})
                    if (client::Widget* w = widget(n)) w->active = true;
                fill_missions();
            }
        }
        first_focus_ = any_ ? "Missions" : no_list_ ? "Difficulty" : "Campaign";
    }
    void draw(client::FrontEnd& fe, render::Image8& out) override {
        if (!placed_) {
            place(out.width, out.height);
            focus_on(first_focus_);  // (0x4A76B0 at the end of the set-up)
        }
        client::LayoutScreen::draw(fe, out);
    }

protected:
    static bool named(const client::Widget& w, const char* n) { return stricmp_c(w.name, n) == 0; }
    // A vertical slider placed anew at y with height h: its arrows at the two ends, the bar between.
    void relocate_slider(const char* name, int y, int h) {
        client::Widget* bar = widget(name);
        if (!bar) return;
        client::Widget *up = nullptr, *down = nullptr;
        for (client::Widget& w : widgets_)
            if (stricmp_c(w.link, name) == 0 && w.slider_step != 0) (w.slider_step < 0 ? up : down) = &w;
        if (!up || !down) {
            bar->y = y;
            bar->h = h;
            return;
        }
        up->y = y;
        down->y = y + h - down->h;
        bar->y = y + up->h;
        bar->h = h - 2 * up->h;
    }
    void focus_on(const char* name) {
        for (std::size_t i = 0; i < widgets_.size(); ++i)
            if (named(widgets_[i], name)) focus_gadget_ = static_cast<int>(i);
    }
    std::string side_name() const {
        const auto& s = art_.sides();
        return side_ < static_cast<int>(s.size()) ? s[static_cast<std::size_t>(side_)] : (side_ == 0 ? "ARM" : "CORE");
    }
    // 0x477360: the side's button and picture pressed (a radio pair each).
    void show_side() {
        for (const char* n : {"Arm", "Side0"})
            if (client::Widget* w = widget(n)) w->status = side_ == 0;
        for (const char* n : {"Core", "Side1"})
            if (client::Widget* w = widget(n)) w->status = side_ == 1;
    }
    // 0x476A60: the side's campaigns (and those for every side), in name order.
    void fill_campaigns(client::FrontEnd& fe) {
        fe.play_sound(art_.sound("smlbutton"));
        std::vector<std::string> names = list_campaigns(art_.vfs());
        std::stable_sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return stricmp_c(a, b) < 0; });
        campaigns_.clear();
        const std::string side = side_name();
        for (const std::string& n : names) {
            Campaign c;
            std::string err;
            if (!load_campaign(art_.vfs(), n, c, err) || !c.has_header) continue;
            if (c.side == side || c.side == "ALL") campaigns_.push_back(n);
        }
        if (client::Widget* l = widget("Campaign")) set_list(*l, campaigns_);
    }
    // 0x4779E0 / 0x435760: the chosen campaign's missions.
    void fill_missions() {
        missions_.clear();
        const client::Widget* l = widget("Campaign");
        Campaign c;
        std::string err;
        if (l && l->selected >= 0 && l->selected < static_cast<int>(campaigns_.size()) &&
            load_campaign(art_.vfs(), campaigns_[static_cast<std::size_t>(l->selected)], c, err))
            for (const CampaignMission& m : c.missions) missions_.push_back(m.name);
        if (client::Widget* m = widget("Missions")) set_list(*m, missions_);
    }
    void choose_side(client::FrontEnd& fe, int side) {
        fe.play_sound(art_.sound(side == 0 ? "SideSelect" : "SideSelect2"));
        side_ = side;
        fe.settings().set_int("side", side);
        show_side();
        // The chosen side's picture is drawn through light level 31 from now on (its colour word
        // set to 0x1F; the other picture keeps whatever it had).
        if (client::Widget* p = widget(side == 0 ? "Side0" : "Side1")) p->light = 0x1F;
        if (!no_list_ || any_) fill_campaigns(fe);
        if (any_) fill_missions();
    }
    void on_list_select(client::FrontEnd&, client::Widget& w) override {
        if (any_ && named(w, "Campaign")) fill_missions();
    }
    void on_pick(client::FrontEnd& fe, client::Widget& w, int, bool double_click) override {
        if (!double_click) return;
        if (named(w, any_ ? "Missions" : "Campaign")) start(fe);
    }
    void on_press(client::FrontEnd& fe, client::Widget& w) override {
        if (named(w, "Start") || named(w, any_ ? "Missions" : "Campaign")) {
            start(fe);
        } else if (named(w, "PrevMenu")) {
            fe.play_sound(art_.sound("Previous"));
            fe.pop();
        } else if (named(w, "Difficulty")) {
            fe.play_sound(art_.sound("SmlButton"));
            fe.settings().set_int("Difficulty", w.stage);
        } else if (named(w, "Side0") || named(w, "Arm")) {
            choose_side(fe, 0);
        } else if (named(w, "Side1") || named(w, "Core")) {
            choose_side(fe, 1);
        }
    }
    void start(client::FrontEnd& fe) {
        fe.play_sound(art_.sound("bigButton"));
        mount_cd_archives(art_.vfs(), art_.game_dir());
        std::string name;
        if (!no_list_ || any_) {
            const client::Widget* l = widget("Campaign");
            if (!l || l->selected < 0 || l->selected >= static_cast<int>(campaigns_.size())) return;
            name = campaigns_[static_cast<std::size_t>(l->selected)];
        } else {
            name = side_ == 0 ? "Arm Campaign" : "Core Campaign";
        }
        int mission = 0;
        if (any_) {
            const client::Widget* m = widget("Missions");
            mission = m && m->selected >= 0 ? m->selected : 0;
        }
        Campaign c;
        std::string err;
        if (!load_campaign(art_.vfs(), name, c, err) || mission >= static_cast<int>(c.missions.size())) return;
        auto progress = std::make_shared<CampaignProgress>();
        progress->campaign = name;
        progress->side = side_name();
        progress->mission = mission;
        if (const client::Widget* d = widget("Difficulty")) progress->difficulty = d->stage;
        fe.settings().set_int("Difficulty", progress->difficulty);
        fe.apply_settings();  // (the settings saved, 0x430F00)
        again_ = true;
        fe.push(std::make_unique<BriefingScreen>(art_, save_dir_, progress));
    }

    MenuArt& art_;
    std::string save_dir_;
    bool any_ = false, no_list_ = false, set_up_ = false, again_ = false;
    int side_ = 0;
    const char* first_focus_ = "Campaign";
    std::vector<std::string> campaigns_, missions_;
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
        if (!name.empty() && !session_ && between_) {
            // Between missions (the campaign's statistics screen): only the summary (0x4326B0).
            SaveContext c = between_();
            c.description = name;
            std::vector<std::uint8_t> bytes;
            write_bank(make_between_missions_bank(c), bytes, nullptr, c.bank);
            write_file(dir_, name, bytes);
        }
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
            if (progress) {
                auto m = std::make_unique<MissionSession>(std::move(s), art, dir, progress);
                m->options = options;
                return m;
            }
            return s;
        });
    }
public:
    std::function<SaveContext()> between_;  // a save outside a battle: its summary

private:
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


bool campaign_end(MenuArt& art, client::BattleSession& session, client::BattleOutcome outcome, CampaignEnd& out) {
    auto* ms = dynamic_cast<MissionSession*>(&session);
    if (!ms) return false;
    std::shared_ptr<CampaignProgress> progress = ms->progress_ptr();
    const std::string save_dir = ms->save_dir();
    // 0x41DC20: the mission's result into the results string at its index.
    progress->won = outcome == client::BattleOutcome::Victory;
    if (outcome == client::BattleOutcome::Victory || outcome == client::BattleOutcome::Defeat)
        if (progress->mission >= 0 && progress->mission < 25) progress->results[static_cast<std::size_t>(progress->mission)] = progress->won ? 'W' : 'L';
    Campaign c;
    std::string err;
    load_campaign(art.vfs(), progress->campaign, c, err);
    MissionInfo& info = ms->info();
    if (info.file.empty() && progress->mission >= 0 && progress->mission < static_cast<int>(c.missions.size()))
        load_mission(art.vfs(), c.missions[static_cast<std::size_t>(progress->mission)].file, progress->difficulty, info, err);
    for (const CampaignMission& m : c.missions) out.missions.push_back(m.name);
    out.results = progress->results;
    out.mission = progress->mission;
    out.won = progress->won;
    out.difficulty = progress->difficulty;
    const bool has_next = progress->mission + 1 < static_cast<int>(c.missions.size());
    // (0x41F400 / 0x41F7F0 state 5: the buttons unless the last mission was won; then the movies)
    out.buttons = !(out.won && !has_next);
    out.final_win = out.won && !has_next;
    out.final_movies = {side_index(art.vfs(), progress->side) == 1 ? "4.zrb" : "3.zrb", "5.zrb"};
    if (out.won && !info.glamour.empty()) {
        // 0x41DA60: bitmaps\glamour\<glamour>.pcx with its palette; its sound camps\briefs\<..>.wav.
        out.glamour = art.picture("glamour\\" + info.glamour, &out.glamour_palette);
        if (!info.glamour_sound.empty()) {
            std::vector<std::uint8_t> wav;
            auto smp = std::make_shared<client::Sample>();
            if (art.vfs().read("camps\\briefs\\" + info.glamour_sound + ".wav", wav) && client::decode_wav(wav, *smp)) out.glamour_sound = smp;
        }
    }
    // "Click to continue." in the side's console font (sidedata "font").
    {
        std::vector<std::uint8_t> sd;
        if (art.vfs().read("gamedata\\sidedata.tdf", sd)) {
            const TdfParseResult r = tdf_parse(std::string_view(reinterpret_cast<const char*>(sd.data()), sd.size()));
            if (r.root)
                if (const TdfSection* sec = r.root->child("SIDE" + std::to_string(side_index(art.vfs(), progress->side))))
                    out.click_font = art.fnt_font(sec->get_string("font", 32, ""));
        }
    }
    out.start = [&art, progress, save_dir](client::FrontEnd& fe, int mission) {
        mount_cd_archives(art.vfs(), art.game_dir());
        progress->mission = mission;
        progress->won = false;
        fe.push(std::make_unique<BriefingScreen>(art, save_dir, progress));
    };
    out.load = [&art, save_dir](client::FrontEnd& fe) {
        if (fe.hooks().load_game_menu)
            if (auto s = fe.hooks().load_game_menu(fe, nullptr)) fe.push(std::move(s));
    };
    out.save = [&art, progress, save_dir](client::FrontEnd& fe) {
        auto s = std::make_unique<SaveGamesScreen>(art, save_dir, nullptr, true, screen_under(fe, nullptr));
        s->between_ = [&art, progress]() {
            SaveContext c;
            c.game_id = static_cast<std::int32_t>(std::time(nullptr));
            c.game_type = 1;
            c.campaign = progress->campaign;
            Campaign camp;
            std::string e;
            load_campaign(art.vfs(), progress->campaign, camp, e);
            const int n = static_cast<int>(camp.missions.size());
            const int next = progress->mission + 1 < n ? progress->mission + 1 : progress->mission;
            c.mission = next >= 0 && next < n ? camp.missions[static_cast<std::size_t>(next)].name : std::string();
            c.thumbs = progress->results;
            c.difficulty = progress->difficulty;
            c.side = side_index(art.vfs(), progress->side);
            c.between_missions = true;
            return c;
        };
        fe.push(std::move(s));
    };
    out.set_difficulty = [progress](int d) { progress->difficulty = d; };
    return true;
}

void install_campaign_hooks(client::FrontEndHooks& hooks, MenuArt& art, const std::string& save_dir) {
    // The original rescans the archives when New Campaign is pressed (0x4775C9) and when the
    // load-game dialog closes (0x49257A; its save listing cannot see archives, so rescanning
    // as it opens is the same).
    hooks.campaign_menu = [&art, save_dir](client::FrontEnd&) -> std::unique_ptr<client::Screen> {
        mount_cd_archives(art.vfs(), art.game_dir());
        return std::make_unique<NewCampaignScreen>(art, save_dir, false);
    };
    hooks.any_mission_menu = [&art, save_dir](client::FrontEnd&) -> std::unique_ptr<client::Screen> {
        mount_cd_archives(art.vfs(), art.game_dir());
        return std::make_unique<NewCampaignScreen>(art, save_dir, true);
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
    hooks.save_game_command = [save_dir](client::BattleSession& s, const std::string& name) {
        // 0x417430: savegame\<name>.sav (the folder made first) with 0x4326B0's summary
        // description "Generic Game Description" and the game id 666 in place of the time.
        SkirmishSession* sk = dynamic_cast<SkirmishSession*>(&s);
        CampaignProgress* progress = nullptr;
        if (auto* ms = dynamic_cast<MissionSession*>(&s)) {
            sk = &ms->inner();
            progress = &ms->progress();
        }
        if (!sk) return;
        SaveContext c = battle_save_context(*sk, progress, "Generic Game Description");
        c.game_id = 0x29A;
        std::vector<std::uint8_t> bytes;
        write_save_file(sk->world().loaded(), c, bytes);
        std::error_code ec;
        std::filesystem::create_directories(save_dir, ec);
        std::ofstream o(std::filesystem::path(save_dir) / (name + ".sav"), std::ios::binary);
        o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
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
