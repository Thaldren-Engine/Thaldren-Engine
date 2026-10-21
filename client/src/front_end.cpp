// The front end's runner: the screen stack, the battle session, the loading screen,
// sounds and music, and the main loop (an SDL3 window with the GPU backend, or a headless
// run with scripted input and screenshots). See client/front_end.h.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <cstdio>
#include <set>

#include "client/audio.h"
#include "client/sound_desk.h"
#include "client/front.h"
#include "client/front_end.h"
#include "client/menu.h"
#include "client/music.h"
#include "render/draw_list.h"
#include "render/presenter.h"
#include "sim/battle.h"
#include "sim/economy.h"
#include "sim/victory.h"

#ifdef CLIENT_HAVE_SDL
#include <SDL3/SDL.h>

#include "render/gpu_renderer.h"
#endif

namespace client {
namespace {

// Pixels an overlay drew: drawn twice over two different fills, the pixels that differ are
// the fill. Returns a sprite keyed on an index the overlay does not use.
render::Sprite8 overlay_sprite(const render::Image8& a, const render::Image8& b) {
    render::Sprite8 s;
    s.width = a.width;
    s.height = a.height;
    std::vector<bool> used(256, false);
    for (std::size_t i = 0; i < a.pixels.size(); ++i)
        if (a.pixels[i] == b.pixels[i]) used[a.pixels[i]] = true;
    int key = 255;
    while (key > 0 && used[static_cast<std::size_t>(key)]) --key;
    s.key = static_cast<std::uint8_t>(key);
    s.has_key = true;
    s.pixels.resize(a.pixels.size());
    for (std::size_t i = 0; i < a.pixels.size(); ++i) s.pixels[i] = a.pixels[i] == b.pixels[i] ? a.pixels[i] : s.key;
    return s;
}

class Runner final : public FrontEnd {
public:
    Runner(FrontEndSetup& setup) : s_(setup) {
        if (!s_.settings_path.empty()) settings_.load(s_.settings_path);
        unit_limit_ = s_.unit_limit;
    }
    ~Runner() override {
        // Screens go before the battle they may refer to.
        graveyard_.clear();
        while (!stack_.empty()) stack_.pop_back();
        retired_.reset();
    }

    // ---- FrontEnd
    void push(std::unique_ptr<Screen> screen) override {
        if (!screen) return;
        stack_.push_back(std::move(screen));
        stack_.back()->enter(*this);
    }
    std::size_t depth() const override { return stack_.size(); }
    Screen* top() { return stack_.empty() ? nullptr : stack_.back().get(); }
    void pop() override {
        if (!stack_.empty()) trim(stack_.size() - 1);
        if (session_ && (ended_ ? stack_.size() <= battle_base_ : stack_.size() < battle_base_)) {
            finish_battle();  // (enters the screen it uncovers)
            return;
        }
        // Back to the battle itself: the screen under it (the skirmish set-up, the battle room)
        // stays hidden and is not entered again until the battle is over (a battle room takes
        // being entered after its launch as the end of the battle and closes itself).
        if (session_ && stack_.size() <= battle_base_) return;
        if (!stack_.empty()) stack_.back()->enter(*this);
    }
    void to_main_menu() override {
        if (session_) {
            trim(std::min(stack_.size(), battle_base_));
            finish_battle(false);
        }
        if (stack_.size() > 1) trim(1);
        if (!stack_.empty()) stack_.back()->enter(*this);
    }
    void start_battle(BattleLauncher launcher) override { pending_ = std::move(launcher); }
    BattleSession* session() override { return session_.get(); }
    void end_battle(BattleOutcome outcome) override {
        if (!session_ || ended_) return;
        ended_ = true;
        outcome_ = outcome;
        trim(std::min(stack_.size(), battle_base_));
        if (s_.battle_over) push(s_.battle_over(*this, *session_, outcome));
        if (stack_.size() <= battle_base_) finish_battle();
    }
    void restart_battle(BattleLauncher launcher) override {
        if (session_) {
            trim(std::min(stack_.size(), battle_base_));
            finish_battle(false);
        }
        start_battle(std::move(launcher));
    }
    void quit() override { quit_ = true; }
    void exit_to_windows() override {
        if (session_ && !ended_) {
            if (s_.music) s_.music->set_mood(MusicMood::Silent);  // (0x4CE690(4))
            session_->leave_game();
        }
        exiting_ = true;
    }
    Settings& settings() override { return settings_; }
    FrontEndHooks& hooks() override { return s_.hooks; }
    const MenuContent& menu_content() const override { return *s_.content; }
    int width() const override { return s_.width; }
    int height() const override { return s_.height; }
    void play_sound(int sample) override {
        if (sample >= 0) menu_sounds_.push_back({sample, MenuSound::Plain});
    }
    void play_sound_loop(int sample) override {
        if (sample >= 0) menu_sounds_.push_back({sample, MenuSound::Loop});
    }
    void play_sound_file(int sample) override {
        if (sample >= 0) menu_sounds_.push_back({sample, MenuSound::File});
    }
    void play_narration(int sample) override {
        if (sample >= 0) menu_sounds_.push_back({sample, MenuSound::Narration});
    }
    void play_narration_sample(const Sample* sample) override {
        if (sample) menu_sounds_.push_back({-1, MenuSound::Narration, sample});
    }
    void stop_narration() override { menu_sounds_.push_back({-1, MenuSound::StopNarration}); }
    void stop_sounds() override { menu_sounds_.push_back({-1, MenuSound::StopAll}); }
    MusicPlayer* music() override { return s_.music; }
    void apply_settings() override {
        // Volumes 0..64 (fxvol, musicvol); sound mode 0 off, 1 mono, 2 3D; the sounds playing
        // at once ("MixingBuffers", 8).
        fx_volume_ = settings_.get_int("fxvol", 27);
        sound_mode_ = settings_.get_int("Sound Mode", 1);
        desk_.set_fx_volume(fx_volume_);
        desk_.set_max_playing(settings_.get_int("MixingBuffers", 8));
        if (s_.music) {
            s_.music->set_volume(settings_.get_int("musicvol", 32));
            s_.music->set_mode(settings_.get_int("cdmode", 4));
            s_.music->set_enabled(settings_.get_int("musicmode", 1) != 0);
        }
        if (session_ && s_.apply_to_battle) s_.apply_to_battle(*this, *session_);
        gamma_from_settings();  // (the options screens' handlers apply the Gamma setting)
        if (!s_.settings_path.empty()) settings_.save(s_.settings_path);
    }
    std::uint32_t now_ms() const override { return now_; }
    void desktop_size(int& w, int& h) const override {
        w = desktop_w_;
        h = desktop_h_;
    }
    int desktop_w_ = 1920, desktop_h_ = 1080;
    bool focused_ = true;
    bool window_focused() const override { return focused_; }
    bool sound_driver_missing_ = false;
    int exit_code_ = 0;  // the program's exit code (1 after the original's fatal box)
    bool sound_driver_missing() const override { return sound_driver_missing_; }
    std::vector<std::array<int, 2>> modes_;
    void display_modes(std::vector<std::array<int, 2>>& out) const override { out = modes_; }
    // The frame size now: a battle's own, else the menus'.
    void frame_size(int& w, int& h) {
        if (in_battle() && !ended_) {
            w = session_->client().options().width;
            h = session_->client().options().height;
        } else {
            w = s_.width;
            h = s_.height;
        }
    }

    // ---- the loop's side
    void begin(AudioOut* audio) {
        audio_ = audio;
        desk_.attach(audio);
        desk_.set_log(log_sounds_);
        apply_settings();
        if (s_.main_menu) push(s_.main_menu(*this));
        if (s_.music) s_.music->set_mood(MusicMood::Silent);
    }
    bool done() const { return quit_ || (stack_.empty() && !session_ && !pending_); }

    void handle(const InputEvent& e) {
        if (e.type == InputEvent::Type::Quit) {
            if (session_ && !ended_ && !pending_ && s_.close_prompt)
                if (std::unique_ptr<Screen> q = s_.close_prompt(*this, *session_)) {
                    push(std::move(q));
                    return;
                }
            quit_ = true;
            return;
        }
        if (e.type == InputEvent::Type::MouseMove || e.type == InputEvent::Type::MouseDown || e.type == InputEvent::Type::MouseUp) {
            mouse_x_ = e.x;
            mouse_y_ = e.y;
            pointer_off_ = false;
        }
        if (e.type == InputEvent::Type::PointerLeave) {
            // The system pointer is off the picture: the menus draw no pointer; a battle's
            // client is told (no pointer, no edge scrolling) whatever is open over it.
            pointer_off_ = true;
            if (in_battle()) session_->client().handle(e);
            return;
        }
        if (pending_) return;
        if (is_screenshot_key(e)) {
            // Taken off the queue at the start of the next pass, before anything is drawn: the
            // picture is the frame shown last (0x499890).
            shot_pending_ = true;
            return;
        }
        // A bar open alone over the battle (the network games' tab bar) leaves the keys to the
        // battle: its key handling runs whenever the game menu is not open.
        const bool key_event = e.type == InputEvent::Type::KeyDown || e.type == InputEvent::Type::KeyUp || e.type == InputEvent::Type::Text;
        // (the screens over the battle that leave it the keys: the tab bar, unit information)
        bool keys_to_battle = in_battle() && stack_.size() >= battle_base_;
        for (std::size_t i = battle_base_; keys_to_battle && i < stack_.size(); ++i) keys_to_battle = stack_[i]->battle_keys();
        bool bar_open = false;
        for (std::size_t i = battle_base_; in_battle() && i < stack_.size(); ++i) bar_open = bar_open || stack_[i]->tab_bar();
        if (in_battle() && (stack_.size() == battle_base_ || (keys_to_battle && key_event))) {
            GameClient& c = session_->client();
            // Tab or F2 opens the game menu (the client keeps keys while its chat line is open).
            // F2 opens the game menu; Tab too, except in network games where it opens the tab bar
            // and closes it again (F2 opens the game menu over the bar).
            if (e.type == InputEvent::Type::KeyDown && !c.chatting() && (e.key == keys::kTab || e.key == keys::kF1 + 1)) {
                if (e.key == keys::kTab && session_->network() && bar_open) {
                    for (std::size_t i = stack_.size(); i-- > battle_base_;)
                        if (stack_[i]->tab_bar()) {
                            stack_[i]->handle(*this, e);
                            break;
                        }
                }
                else if (e.key == keys::kTab && session_->network() && s_.tab_menu) push(s_.tab_menu(*this, *session_));
                else if (s_.game_menu) push(s_.game_menu(*this, *session_));
                return;
            }
            c.handle(e);
            if (c.quit_requested()) quit_ = true;
            return;
        }
        if (!stack_.empty()) stack_.back()->handle(*this, e);
    }

    // The modifier keys held now (a battle's client polls them).
    void poll_modifiers(bool shift, bool ctrl, bool alt) {
        if (in_battle()) session_->client().poll_modifiers(shift, ctrl, alt);
    }

    // The frame just shown (its list and palette), for the screenshot key.
    void shown(render::DrawList&& list, const render::Palette* palette) {
        last_shown_ = std::move(list);
        last_palette_ = palette;
        // A film frame due: the frame just drawn goes into the film's folder.
        if (in_battle() && session_->client().film_due() && palette && last_shown_.width > 0) {
            GameClient& c = session_->client();
            save_numbered_picture(c.film_folder(), "FRAM", render::execute(last_shown_, blend()), *palette);
            c.film_written(now_);
        }
    }
    void take_screenshot() {
        if (!shot_pending_) return;
        shot_pending_ = false;
        if (!last_palette_ || last_shown_.width <= 0) return;
        const std::string path =
            save_numbered_picture(s_.shot_dir, s_.shot_prefix, render::execute(last_shown_, blend()), *last_palette_);
        std::printf("screenshot %s\n", path.empty() ? "(not written)" : path.c_str());
    }

    void frame(std::uint32_t now) {
        now_ = now;
        ++frame_no_;
        // (0x496BB0 starts each front-end pass with 0x42BD10: the unit definitions enumerated when
        // there are none yet or a battle's loading asked for it again.)
        if (units_due_ && !in_battle() && !pending_ && !loading_) {
            units_due_ = false;
            if (s_.hooks.enumerate_units) s_.hooks.enumerate_units(*this);
        }
        take_screenshot();
        graveyard_.clear();  // screens closed during the last pass go now
        retired_.reset();    // then a battle left during it
        const std::uint32_t ticks = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now) * 30 / 1000);
        // The menu pointer steps by its frames' own times with the wall-clock ticks since the
        // last pass (as the battle's pointer does).
        if (!pointer_started_) {
            pointer_started_ = true;
            pointer_ticks_ = ticks;
            pointer_.start(s_.content->cursor_timing);
        }
        pointer_.step(s_.content->cursor_timing, ticks - pointer_ticks_);
        pointer_ticks_ = ticks;
        choose_pointer_look(ticks);
        if (pending_) run_launcher();
        if (in_battle()) {
            // The battle goes on under menus (the game menu pauses it outside network games).
            session_->frame(now);
            // Network games: the chat line opens as its own screen (0x494050: TALK.GUI or TALK2.GUI).
            if (session_->network() && session_->client().chatting() && s_.hooks.chat_panel) {
                bool open = false;
                for (const auto& sc : stack_) open = open || (sc && sc.get() == chat_screen_);
                if (!open)
                    if (auto d = s_.hooks.chat_panel(*this, *session_)) {
                        chat_screen_ = d.get();
                        push(std::move(d));
                    }
            }
            // A silent remote player: the time-out dialog (it closes itself when nobody is).
            if (session_->timed_out_player() >= 0 && s_.hooks.timeout_dialog) {
                bool open = false;
                for (const auto& sc : stack_) open = open || (sc && sc.get() == timeout_screen_);
                if (!open)
                    if (auto d = s_.hooks.timeout_dialog(*this, *session_)) {
                        timeout_screen_ = d.get();
                        push(std::move(d));
                    }
            }
            sim::Battle& b = session_->battle();
            // The local player put in watch mode: its question or box (taken once).
            if (b.victory.watch_prompt != 0) {
                const int kind = b.victory.watch_prompt;
                b.victory.watch_prompt = 0;
                if (s_.hooks.watch_dialog)
                    if (auto d = s_.hooks.watch_dialog(*this, *session_, kind)) push(std::move(d));
            }
            if (s_.music) {
                const int me = b.settings.local_player;
                if (!session_->client().paused() && stack_.size() == battle_base_)
                    judge_.update(ticks, b.players[static_cast<std::size_t>(me)].alive_count, *s_.music);
                if ((b.end_game.flags & sim::end_game_flags::kWon) && !mood_set_) {
                    s_.music->set_mood(MusicMood::Victory);
                    mood_set_ = true;
                } else if ((b.end_game.flags & sim::end_game_flags::kLost) && !mood_set_) {
                    s_.music->set_mood(MusicMood::Defeat);
                    mood_set_ = true;
                }
            }
            // The battle is over once the end flags say so (after the end countdown).
            if ((b.end_game.flags & sim::end_game_flags::kEnding) && session_->end_exchange_done(now)) {
                const BattleOutcome o = (b.end_game.flags & sim::end_game_flags::kWon) ? BattleOutcome::Victory : BattleOutcome::Defeat;
                end_battle(o);
            }
        }
        // The top screen's pass (a screen it pushes or uncovers has its first pass next time; the
        // stack may change during the call).
        if (!stack_.empty() && stack_.back()) stack_.back()->frame(*this, now);
        if (s_.music) s_.music->update(ticks, audio_);
    }

    void activity(int amount) { judge_.activity(amount); }

    // The frame and its palette.
    render::DrawList draw(const render::Palette*& palette) {
        int w = 0, h = 0;
        frame_size(w, h);
        render::DrawList list;
        list.width = w;
        list.height = h;
        palette = &s_.content->palette;
        if (exiting_) {
            // (0x4257A0 / state 8: the back buffer filled with colour 0 and shown, no pointer;
            // the window closes after it)
            if (in_battle()) palette = &session_->content().palette;
            render::Sprite8 s;
            s.width = w;
            s.height = h;
            s.has_key = false;
            s.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
            list.blit(list.keep(std::move(s)), 0, 0, {0, 0, w - 1, h - 1}, false);
            quit_ = true;
            return list;
        }
        if (in_battle() && !ended_) {
            session_->client().set_pointer_hidden(stack_.size() > battle_base_);
            list = session_->client().draw();
            palette = &session_->content().palette;
            if (stack_.size() > battle_base_) {
                // Menus over the battle: drawn twice (two fills) to find their pixels.
                render::Image8 a(w, h, 0), b(w, h, 255);
                for (std::size_t i = battle_base_; i < stack_.size(); ++i) {
                    stack_[i]->draw(*this, a);
                    stack_[i]->draw(*this, b);
                }
                list.blit(list.keep(overlay_sprite(a, b)), 0, 0, {0, 0, w - 1, h - 1}, false);
                add_pointer(list);
            }
            return list;
        }
        render::Image8 img(w, h, 0);
        if (loading_) {
            load_.now_ms = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          std::chrono::steady_clock::now() - load_clock_start_).count());
            if (s_.draw_loading) s_.draw_loading(*this, img, load_);
        } else if (!stack_.empty()) {
            // The top screen, and the ones it lets show through.
            std::size_t first = stack_.size() - 1;
            while (first > 0 && stack_[first]->see_through()) --first;
            for (std::size_t i = first; i < stack_.size(); ++i) stack_[i]->draw(*this, img);
            for (std::size_t i = stack_.size(); i-- > 0;)
                if (const render::Palette* p = stack_[i]->palette()) {
                    palette = p;
                    break;
                }
        }
        render::Sprite8 s;
        s.width = w;
        s.height = h;
        s.has_key = false;
        s.pixels = std::move(img.pixels);
        list.blit(list.keep(std::move(s)), 0, 0, {0, 0, w - 1, h - 1}, false);
        if (!loading_) add_pointer(list);
        return list;
    }

    void play_pending_sounds() {
        // The main loop's 100 ms sweep, then this pass's starts: the battle's (simulation and
        // interface), then the menus'.
        desk_.tick(now_);
        if (in_battle()) {
            const std::vector<Sample>& smp = session_->content().samples;
            for (const SoundEvent& e : session_->client().take_sounds()) {
                if (e.sample < 0 || static_cast<std::size_t>(e.sample) >= smp.size()) continue;
                SoundRequest r;
                r.sample = &smp[static_cast<std::size_t>(e.sample)];
                r.volume = e.volume;
                r.placed = e.placed;
                r.pos = e.pos;
                r.min_distance = e.min_distance;
                r.max_distance = e.max_distance;
                r.one_shot = e.one_shot;
                desk_.play(r, now_);
            }
        }
        const std::vector<Sample>& ms = s_.content->samples;
        const bool fx_on = fx_volume_ != 0 && sound_mode_ != 0;
        for (const MenuSound& m : menu_sounds_) {
            const Sample* smp = m.direct ? m.direct : m.sample >= 0 && static_cast<std::size_t>(m.sample) < ms.size() ? &ms[static_cast<std::size_t>(m.sample)] : nullptr;
            switch (m.kind) {
            case MenuSound::Plain:
            case MenuSound::Loop:
            case MenuSound::File:
                if (smp && fx_on) {
                    SoundRequest r;
                    r.sample = smp;
                    r.loop = m.kind == MenuSound::Loop;
                    r.one_shot = m.kind == MenuSound::File;
                    desk_.play(r, now_);
                }
                break;
            case MenuSound::Narration: desk_.play_stream(smp); break;
            case MenuSound::StopNarration: desk_.stop_stream(); break;
            case MenuSound::StopAll: desk_.stop_all(now_); break;
            }
        }
        menu_sounds_.clear();
        std::string label;
        if (log_sounds_) {
            label = "frame " + std::to_string(frame_no_);
            if (in_battle()) label += " step " + std::to_string(session_->battle().game_tick());
        }
        desk_.flush(label);
        sound_count_ = desk_.started();
    }

    // The loop sets this: draws and shows a frame at once (the loading screen's progress).
    std::function<void()> present_now;
    // The window loop sets this: the system's message box (null: printed).
    std::function<void(const std::string&, const std::string&)> system_box;
    void system_message(const std::string& title, const std::string& text) override {
        if (system_box) system_box(title, text);
        else FrontEnd::system_message(title, text);
    }
    const render::BlendTable& blend() const { return s_.content->blend; }
    float load_fraction() const {
        int sum = 0;
        for (int r : load_.rows) sum += r;
        return static_cast<float>(sum) / (100.0f * kLoadRows);
    }
    LoadingState* loading() override { return loading_ ? &load_ : nullptr; }
    std::size_t screens_over_battle() const { return session_ ? stack_.size() - battle_base_ : 0; }

private:
    bool in_battle() const { return session_ != nullptr; }
    // Closes the screens above the first n (kept until the next pass: a screen may close itself or the
    // battle from its own handler).
    void trim(std::size_t n) {
        while (stack_.size() > n) {
            graveyard_.push_back(std::move(stack_.back()));
            stack_.pop_back();
        }
    }

    // The menus' pointer (0x4AB440 every interface pass): inside the top screen's box the plain
    // pointer's first frame; outside it the look 0x491C80 set last (20: the hourglass, its
    // animation started again each time the pointer leaves the box; 19 or none set: the plain
    // pointer).
    bool pointer_outside_box() {
        if (stack_.empty()) return false;
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
        if (!stack_.back()->pointer_box(x0, y0, x1, y1)) return false;
        return mouse_x_ < x0 || mouse_x_ > x1 || mouse_y_ < y0 || mouse_y_ > y1;
    }
    void choose_pointer_look(std::uint32_t ticks) {
        const bool busy = look_ == 20 && s_.content->cursor_busy && !s_.content->cursor_busy->empty() && pointer_outside_box();
        if (busy && !busy_shown_) {
            busy_.start(s_.content->cursor_busy_timing);
            busy_ticks_ = ticks;
        }
        busy_shown_ = busy;
        if (busy) {
            busy_.step(s_.content->cursor_busy_timing, ticks - busy_ticks_);
            busy_ticks_ = ticks;
        }
    }
    void set_pointer_look(int look) override { look_ = look; }
    void add_pointer(render::DrawList& list) {
        if (!draw_pointer_ || pointer_off_) return;
        if (!stack_.empty() && stack_.back()->hides_pointer() && !in_battle()) return;
        if (busy_shown_ && !in_battle()) {
            const auto* c = s_.content->cursor_busy;
            if (busy_.ended) return;
            list.blit(&(*c)[static_cast<std::size_t>(busy_.frame) % c->size()], mouse_x_, mouse_y_, {0, 0, list.width - 1, list.height - 1}, true);
            return;
        }
        const auto* c = s_.content->cursor;
        if (c && !c->empty()) {
            if (pointer_.ended) return;
            const render::Sprite8& s = (*c)[static_cast<std::size_t>(pointer_.frame) % c->size()];
            list.blit(&s, mouse_x_, mouse_y_, {0, 0, list.width - 1, list.height - 1}, true);
        }
    }

    void run_launcher() {
        // The sounds of the button that started it play first (the menus' loop goes on while
        // the battle loads).
        play_pending_sounds();
        BattleLauncher l = std::move(pending_);
        pending_ = nullptr;
        // A battle that ended and is still kept (the campaign's statistics screen, which stays
        // under the next mission's briefing) is left now, before the next one loads.
        if (session_ && ended_) {
            std::unique_ptr<BattleSession> old = std::move(session_);
            ended_ = false;
            battle_base_ = 0;
            desk_.forget(old->content().samples);
            retired_ = std::move(old);
        }
        // The loading screen starts with every row at 0 and its highlights off; it is drawn at
        // the first report (the launcher names the map first) and then at most every 200 ms
        // (the original's loading routine draws a frame, then sleeps 200 ms).
        loading_ = true;
        load_ = LoadingState{};
        load_clock_start_ = std::chrono::steady_clock::now();
        bool drawn = false;
        std::chrono::steady_clock::time_point last_draw{};
        std::string error;
        auto progress = [this, &drawn, &last_draw](float f, const std::string& stage) {
            const int pct = std::clamp(static_cast<int>(f * 100.0f), 0, 100);
            if (stage.rfind("row:", 0) == 0) {
                const int r = std::atoi(stage.c_str() + 4);
                if (r >= 0 && r < kLoadRows) load_.rows[static_cast<std::size_t>(r)] = pct;
            } else if (stage.rfind("map:", 0) == 0) {
                load_.map = stage.substr(4);
                load_.map_line = !load_.map.empty();
            }
            std::printf("loading: %3d%% %s\n", pct, stage.c_str());
            const auto now = std::chrono::steady_clock::now();
            if (drawn && now - last_draw < std::chrono::milliseconds(kLoadRedrawMs)) return;
            drawn = true;
            if (present_now) present_now();
            last_draw = std::chrono::steady_clock::now();
        };
        std::unique_ptr<BattleSession> s = l(progress, error);
        loading_ = false;
        if (!s) {
            std::printf("battle failed to start: %s\n", error.c_str());
            if (error.rfind("fatal:", 0) == 0) {
                // The original's fatal box (0x4B6290: the program's name as the title, then
                // exit(1)).
                fatal_box(error.substr(6), !system_box);
                exit_code_ = 1;
                quit();
            }
            return;
        }
        // The end of the loading stops every sound (the menus' loop too; 0x497F40).
        desk_.stop_all(now_);
        desk_.flush(log_sounds_ ? "frame " + std::to_string(frame_no_) : std::string());
        session_ = std::move(s);
        ended_ = false;
        mood_set_ = false;
        battle_base_ = stack_.size();
        judge_.reset();
        if (s_.music) s_.music->set_mood(MusicMood::Building);
        BattleSession* sess = session_.get();
        session_->client().on_chat = [sess](const std::string& text) { sess->send_chat(text); };
        // Settings the battle saves at once (the original writes the registry, 0x430F00).
        session_->client().save_setting = [this](const char* name, int value) {
            settings_.set_int(name, value);
            if (!s_.settings_path.empty()) settings_.save(s_.settings_path);
        };
        // The battle's load applies the Gamma setting (0x491200); "+gamma n" shows n x 0.1 and
        // keeps n as the setting (saved at once).
        gamma_from_settings();
        // The settings writer (0x430F00) after a command that saves: the battle's settings by
        // their registry names (the rest of the settings file stays as it is).
        session_->client().save_settings = [this, sess]() {
            const ClientOptions& o = sess->client().options();
            settings_.set_int("Interface Type", o.right_click_orders ? 1 : 0);
            settings_.set_int("scrollspeed", o.scroll_speed);
            settings_.set_int("screenchat", o.screen_chat ? 1 : 0);
            settings_.set_int("damagebars", o.damage_bars ? 1 : 0);
            settings_.set_int("Sound Mode", o.sound_mode);
            settings_.set_int("Anti-Alias", o.antialias ? 1 : 0);
            settings_.set_int("Shadows", o.shadows ? 1 : 0);
            settings_.set_int("FeatureShadows", o.feature_shadows ? 1 : 0);
            settings_.set_int("VehicleShadows", o.unit_shadows ? 1 : 0);
            settings_.set_int("Shading", o.shading ? 1 : 0);
            settings_.set_int("DitheredFog", o.dithered_fog ? 1 : 0);
            settings_.set_int("SwitchAlt", o.alt_groups ? 1 : 0);
            settings_.set_int("textlines", o.text_lines);
            settings_.set_int("textscroll", o.text_seconds);
            settings_.set_int("unitchat", o.unit_chat);
            settings_.set_int("unitchattext", o.unit_chat_text);
            settings_.set_int("clock", o.clock ? 1 : 0);
            if (!o.image_dir.empty()) settings_.set_text("Image Output Directory", o.image_dir);
            settings_.set_int("Movie Output Rate", o.film_speed);
            if (!s_.settings_path.empty()) settings_.save(s_.settings_path);
        };
        session_->client().save_game_command = [this, sess](const std::string& name) {
            if (s_.hooks.save_game_command) s_.hooks.save_game_command(*sess, name);
        };
        session_->client().music_command = [this](const char* what, int n) {
            MusicPlayer* m = s_.music;
            if (!m) return;
            const std::string w = what;
            if (w == "play") {
                // (0x4CEB60: 0 lets the mood pick; the track already playing goes on)
                if (n == 0) m->next();
                else if (!(m->is_playing() && m->current_track() == n)) m->play(n);
            } else if (w == "stop") {
                m->stop();
            } else if (w == "mode") {
                // (0x4175E0 -> 0x4CE690: the music mood, 0 building .. 4 silent)
                m->set_mood(static_cast<MusicMood>(static_cast<std::uint8_t>(n)));  // (any number: no track has another mood)
            }
        };
        session_->client().warp_pointer = [this](int x, int y) {
            if (warp_) warp_(x, y);
        };
        session_->client().on_gamma_command = [this](int n) {
            display_gamma_ = gamma_from_command(n);
            gamma_setting_ = n;
            settings_.set_int("Gamma", n);
            gamma_seen_ = n;
            if (!s_.settings_path.empty()) settings_.save(s_.settings_path);
        };
        session_->client().save_text_setting = [this](const char* name, const std::string& value) {
            settings_.set_text(name, value);
            if (!s_.settings_path.empty()) settings_.save(s_.settings_path);
            // (the image output folder holds the screenshots' folder too)
            if (std::string(name) == "Image Output Directory") s_.shot_dir = value + "\\screenshots";
        };
        // 'h' in a network game: the share screen over the battle, as the tab bar's SHARE opens it.
        session_->client().on_share_key = [this, sess]() {
            if (stack_.size() == battle_base_ && s_.hooks.share_menu) push(s_.hooks.share_menu(*this, *sess));
        };
        // F1: the unit information screen over the battle (several may be open at once).
        session_->client().on_unit_info = [this, sess](std::uint16_t type) {
            if (s_.hooks.unit_info)
                if (auto sc = s_.hooks.unit_info(*this, *sess, type)) push(std::move(sc));
        };
        session_->combat_activity = [this](int amount) { judge_.activity(amount); };
        if (s_.apply_to_battle) s_.apply_to_battle(*this, *session_);
    }

    void finish_battle(bool ask_session = true) {
        if (!session_) return;
        units_due_ = true;  // (the battle's loading, GameData_LoadAll, set GS+0x14397)
        std::unique_ptr<BattleSession> s = std::move(session_);
        const BattleOutcome o = ended_ ? outcome_ : BattleOutcome::Quit;
        ended_ = false;
        battle_base_ = 0;
        if (s_.music) s_.music->set_mood(MusicMood::Silent);
        desk_.forget(s->content().samples);
        if (log_sounds_) {
            static const char* const names[] = {"none", "victory", "defeat", "draw", "quit"};
            std::printf("frame %d: battle left (%s)\n", frame_no_, names[static_cast<int>(o)]);
        }
        if (ask_session) s->after_battle(*this, o);
        retired_ = std::move(s);  // (gone after the screens closed with it, which may refer to it)
        if (!stack_.empty()) stack_.back()->enter(*this);
    }

    FrontEndSetup& s_;
    // Display gamma: the factor the screen's palette is shown with (1 until a battle loads or an
    // options screen applies the setting), and the Gamma setting as the program holds it (read
    // at start-up with 10 taken as 12, 0x42F9A0; a changed setting is taken as it is).
    float display_gamma_ = 1.0f;
    int gamma_setting_ = 12, gamma_seen_ = -1;
    void gamma_from_settings() {
        const int raw = settings_.get_int("Gamma", 12);
        if (gamma_seen_ < 0) gamma_setting_ = raw == 10 ? 12 : raw;
        else if (raw != gamma_seen_) gamma_setting_ = raw;
        gamma_seen_ = raw;
        display_gamma_ = gamma_from_setting(gamma_setting_);
    }

public:
    // The palette the screen shows for a frame's palette.
    render::Palette display_palette(const render::Palette& p) const { return display_gamma_ == 1.0f ? p : gamma_palette(p, display_gamma_); }
    std::function<void(int, int)> warp_;  // the window's pointer warp (frame pixels)

private:
    bool shot_pending_ = false;
    const Screen* timeout_screen_ = nullptr;  // the time-out dialog while it is open
    render::DrawList last_shown_;
    const render::Palette* last_palette_ = nullptr;
    Settings settings_;
    std::vector<std::unique_ptr<Screen>> stack_;
    std::vector<std::unique_ptr<Screen>> graveyard_;
    std::unique_ptr<BattleSession> session_;
    std::unique_ptr<BattleSession> retired_;  // a battle just left (kept until its screens are gone)
    std::size_t battle_base_ = 0;  // stack size when the battle started (screens above are its menus)
    bool ended_ = false, mood_set_ = false;
    BattleOutcome outcome_ = BattleOutcome::None;
    BattleLauncher pending_;
    bool loading_ = false;
    bool units_due_ = true;  // the unit definitions to be enumerated at the next front-end pass
    LoadingState load_;
    std::chrono::steady_clock::time_point load_clock_start_{};
    bool quit_ = false;
    bool exiting_ = false;
    const Screen* chat_screen_ = nullptr;  // the network chat line's screen while it is open  // exit to Windows: the next frame is the black one, then the end
    AudioOut* audio_ = nullptr;
    SoundDesk desk_;
    int fx_volume_ = 27, sound_mode_ = 1;
    struct MenuSound {
        int sample = -1;
        enum Kind : std::uint8_t { Plain, Loop, File, Narration, StopNarration, StopAll } kind = Plain;
        const Sample* direct = nullptr;  // a sound the caller keeps (instead of `sample`)
    };
    std::vector<MenuSound> menu_sounds_;
    int frame_no_ = -1;
    std::uint32_t now_ = 0;
    FrameClock pointer_;  // the menu pointer's animation
    std::uint32_t pointer_ticks_ = 0;
    int look_ = 0;              // the menus' pointer look (0x491C80; 0: none set yet)
    bool busy_shown_ = false;   // the hourglass shown (look 20, the pointer outside the top box)
    FrameClock busy_;
    std::uint32_t busy_ticks_ = 0;
    bool pointer_started_ = false;
    int mouse_x_ = 0, mouse_y_ = 0;
    bool pointer_off_ = false;  // the system pointer is outside the picture
    BattleMusicJudge judge_;

public:
    std::size_t sound_count_ = 0;
    bool draw_pointer_ = true;
    bool log_sounds_ = false;
};

void save_shot(const render::Image8& frame, const render::Palette& pal, const std::string& prefix, int n) {
    char name[32];
    std::snprintf(name, sizeof name, "%04d.bmp", n);
    render::write_bmp(prefix + name, frame, pal);
    std::printf("screenshot %s%s\n", prefix.c_str(), name);
}

#ifdef CLIENT_HAVE_SDL
int sdl_key(SDL_Keycode k) {
    switch (k) {
    case SDLK_LEFT: return keys::kLeft;
    case SDLK_RIGHT: return keys::kRight;
    case SDLK_UP: return keys::kUp;
    case SDLK_DOWN: return keys::kDown;
    case SDLK_PAUSE: return keys::kPause;
    case SDLK_ESCAPE: return keys::kEscape;
    case SDLK_RETURN: case SDLK_KP_ENTER: return keys::kEnter;
    case SDLK_TAB: return keys::kTab;
    case SDLK_BACKSPACE: return keys::kBackspace;
    case SDLK_DELETE: return keys::kDelete;
    case SDLK_INSERT: return keys::kInsert;
    case SDLK_HOME: return keys::kHome;
    case SDLK_END: return keys::kEnd;
    case SDLK_KP_PLUS: return '+';
    case SDLK_KP_MINUS: return '-';
    default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) return keys::kF1 + static_cast<int>(k - SDLK_F1);
    if (k >= 32 && k < 127) return static_cast<int>(std::tolower(static_cast<int>(k)));
    return 0;
}
#endif

}  // namespace

namespace {
std::vector<std::string> split_list(const std::string& v) {
    std::vector<std::string> o;
    for (std::size_t a = 0; a <= v.size();) {
        std::size_t e = v.find(',', a);
        if (e == std::string::npos) e = v.size();
        if (e > a) o.push_back(v.substr(a, e - a));
        a = e + 1;
    }
    return o;
}
// A key's name (gui_harness's names) as the window front end's key: 0 unknown.
int sweep_key(const std::string& k) {
    if (k.size() == 1) return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(k[0])));
    if (k == "enter") return keys::kEnter;
    if (k == "esc") return keys::kEscape;
    if (k == "tab") return keys::kTab;
    if (k == "space") return ' ';
    if (k == "bksp") return keys::kBackspace;
    if (k == "left") return keys::kLeft;
    if (k == "up") return keys::kUp;
    if (k == "right") return keys::kRight;
    if (k == "down") return keys::kDown;
    if (k == "home") return keys::kHome;
    if (k == "end") return keys::kEnd;
    if (k == "del") return keys::kDelete;
    if (k.size() > 1 && k[0] == 'f') return keys::kF1 + std::atoi(k.c_str() + 1) - 1;
    return 0;
}
// The key codes the menus take from the event.
std::vector<int> sweep_codes(const InputEvent& e) { return menu_key_codes(e); }
// The character a US keyboard types for the key with Shift.
int shifted_char(int k) {
    if (k >= 'a' && k <= 'z') return k - 'a' + 'A';
    static const char plain[] = "1234567890-=[]\\;',./`", shifted[] = "!@#$%^&*()_+{}|:\"<>?~";
    for (int i = 0; plain[i]; ++i)
        if (plain[i] == k) return static_cast<unsigned char>(shifted[i]);
    return k;
}
}  // namespace

int run_key_sweep(FrontEndSetup& setup, const std::string& keys_arg, const std::string& mods_arg, int click_x, int click_y, const std::string& typed) {
    std::string keys_list = keys_arg;
    if (keys_list == "all") {
        keys_list.clear();
        for (char c = 'a'; c <= 'z'; ++c) keys_list += std::string(1, c) + ",";
        for (char c = '0'; c <= '9'; ++c) keys_list += std::string(1, c) + ",";
        keys_list += "enter,esc,tab,space,bksp,left,up,right,down";
    }
    using Desc = std::vector<std::pair<std::string, std::string>>;
    auto value = [](const Desc& d, const std::string& k) {
        for (const auto& [a, b] : d)
            if (a == k) return b;
        return std::string();
    };
    for (const std::string& m : split_list(mods_arg))
        for (const std::string& kn : split_list(keys_list)) {
            FrontEndSetup fresh = setup;  // (no settings file: every trial starts alike)
            fresh.settings_path.clear();
            Runner run(fresh);
            run.begin(nullptr);
            const render::Palette* pal = nullptr;
            for (int f = 0; f < 2; ++f) {
                run.frame(static_cast<std::uint32_t>(f * 33));
                run.draw(pal);
            }
            if (click_x >= 0) {
                InputEvent c;
                c.x = click_x;
                c.y = click_y;
                c.button = 1;
                c.time_ms = 40;
                c.type = InputEvent::Type::MouseMove;
                run.handle(c);
                c.type = InputEvent::Type::MouseDown;
                run.handle(c);
                c.type = InputEvent::Type::MouseUp;
                run.handle(c);
                run.frame(50);
                run.draw(pal);
            }
            for (const char ch : typed) {
                InputEvent d;
                d.type = InputEvent::Type::KeyDown;
                d.key = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(ch)));
                d.shift = std::isupper(static_cast<unsigned char>(ch)) != 0;
                d.text_follows = true;
                run.handle(d);
                InputEvent t = d;
                t.type = InputEvent::Type::Text;
                t.key = static_cast<unsigned char>(ch);
                t.text_follows = false;
                run.handle(t);
                d.type = InputEvent::Type::KeyUp;
                run.handle(d);
                run.frame(60);
                run.draw(pal);
            }
            auto top = [&]() -> LayoutScreen* { return run.depth() ? dynamic_cast<LayoutScreen*>(run.top()) : nullptr; };
            LayoutScreen* before = top();
            const Desc a = before ? before->describe() : Desc{};
            if (before) before->take_presses();
            const std::size_t depth = run.depth();
            InputEvent e;
            e.type = InputEvent::Type::KeyDown;
            e.key = sweep_key(kn);
            e.shift = m.find("shift") != std::string::npos;
            e.ctrl = m.find("ctrl") != std::string::npos;
            e.alt = m.find("alt") != std::string::npos;
            e.time_ms = 66;
            const bool character = e.key >= 0x20 && e.key < 0x7F && !e.ctrl && !e.alt;
            e.text_follows = character;
            std::vector<int> codes = sweep_codes(e);
            run.handle(e);
            if (character) {
                // (the screen the key down left on top gets the character)
                InputEvent t = e;
                t.type = InputEvent::Type::Text;
                t.key = e.shift ? shifted_char(e.key) : e.key;
                t.text_follows = false;
                for (int c : sweep_codes(t)) codes.push_back(c);
                run.handle(t);
            }
            InputEvent u = e;
            u.type = InputEvent::Type::KeyUp;
            run.handle(u);
            // (a closed screen lives until the next frame)
            std::string out;
            if (before)
                for (const std::string& p : before->take_presses()) out += " press:" + p;
            run.frame(99);
            run.draw(pal);
            LayoutScreen* after = top();
            const Desc b = after ? after->describe() : Desc{};
            const bool same = after == before && run.depth() == depth;
            if (!same) out += " screen:" + value(a, "layout") + "->" + (after ? value(b, "layout") : std::string("none"));
            if (same) {
                if (value(a, "focus") != value(b, "focus")) out += " focus:" + value(a, "focus") + "->" + value(b, "focus");
                if (value(a, "field") != value(b, "field")) out += " active:" + value(a, "field") + "->" + value(b, "field");
                for (const auto& [k, v] : b)
                    if (k.rfind("text:", 0) == 0 && value(a, k) != v) out += " text:" + k.substr(5) + "=\"" + v + "\"";
                for (const auto& [k, v] : b)
                    if (k.rfind("stage:", 0) == 0 && value(a, k) != v) out += " stage:" + k.substr(6) + "=" + v;
            }
            std::string q;
            for (int c : codes) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "%s%02X", q.empty() ? "" : ",", c);
                q += buf;
            }
            std::printf("OUTCOME %s %s %s [%s]%s\n", value(a, "layout").c_str(), kn.c_str(), m.c_str(), q.c_str(), out.empty() ? " nothing" : out.c_str());
        }
    return 0;
}

void fatal_box(const std::string& text, bool headless) {
    std::printf("fatal: %s\n", text.c_str());
#ifdef CLIENT_HAVE_SDL
    if (!headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Total Annihilation", text.c_str(), nullptr);
#else
    (void)headless;
#endif
}

int run_front_end(FrontEndSetup& setup, const FrontOptions& o) {
    Runner run(setup);
    run.draw_pointer_ = o.draw_pointer;
    const int w = setup.width, h = setup.height;
    std::size_t next = 0;
    auto feed_script = [&](int f, std::uint32_t now) {
        for (; next < o.script.size() && o.script[next].frame <= f; ++next) {
            InputEvent e = o.script[next].event;
            e.time_ms = now;
            run.handle(e);
        }
    };
    auto wants_shot = [&](int f) { return std::find(o.screenshot_frames.begin(), o.screenshot_frames.end(), f) != o.screenshot_frames.end(); };

    if (o.headless) {
#ifdef CLIENT_HAVE_SDL
        std::unique_ptr<render::GpuRenderer> gpu;
        if (o.gpu_check) {
            gpu = render::GpuRenderer::create(run.blend());
            if (!gpu) std::printf("GPU backend unavailable: %s\n", render::GpuRenderer::last_error().c_str());
        }
#endif
        run.log_sounds_ = true;
        run.begin(nullptr);
        // The loading screen is shown once, half way, when screenshots are wanted.
        bool loading_shot = false;
        run.present_now = [&] {
            if (o.screenshot_prefix.empty() || loading_shot || run.load_fraction() < 0.5f) return;
            loading_shot = true;
            const render::Palette* pal = nullptr;
            render::DrawList list = run.draw(pal);
            render::write_bmp(o.screenshot_prefix + "loading.bmp", render::execute(list, run.blend()), *pal);
            std::printf("screenshot %sloading.bmp\n", o.screenshot_prefix.c_str());
        };
        int f = 0;
        const auto wall_start = std::chrono::steady_clock::now();
        for (; f < o.frames && !run.done(); ++f) {
            const std::uint32_t now = static_cast<std::uint32_t>(static_cast<std::uint64_t>(f) * 1000u / 30u);
            if (o.realtime) std::this_thread::sleep_until(wall_start + std::chrono::milliseconds(now));
            feed_script(f, now);
            run.frame(now);
            run.play_pending_sounds();
            const render::Palette* pal = nullptr;
            render::DrawList list = run.draw(pal);
            if (wants_shot(f)) {
                const render::Image8 img = render::execute(list, run.blend());
                save_shot(img, *pal, o.screenshot_prefix, f);
#ifdef CLIENT_HAVE_SDL
                if (gpu) {
                    render::lines_to_sprites(list);
                    render::Image8 g;
                    if (gpu->render(list, g)) {
                        std::size_t diff = 0;
                        for (std::size_t i = 0; i < g.pixels.size() && i < img.pixels.size(); ++i) diff += g.pixels[i] != img.pixels[i];
                        std::printf("  GPU (%s) vs CPU: %zu differing pixels\n", gpu->driver().c_str(), diff);
                    }
                }
#endif
            }
            run.shown(std::move(list), pal);
        }
        std::printf("front end (headless): %d frames, %zu interface sounds\n", f, run.sound_count_);
        // (tests: whether a battle is still running at the end, and the screens open over it)
        if (run.session()) {
            std::printf("battle running, %zu screens over it\n", run.screens_over_battle());
            // (tests: each player record in use, its controller and its live units)
            const sim::Battle& b = run.session()->battle();
            for (std::size_t k = 0; k < b.players.size() && k < 10; ++k)
                if (b.players[k].active)
                    std::printf("player %zu: controller %d, %d units\n", k, static_cast<int>(b.players[k].controller),
                                static_cast<int>(b.players[k].alive_count));
        } else {
            std::printf("no battle running\n");
        }
        return run.exit_code_;
    }
#ifdef CLIENT_HAVE_SDL
    // The display's set-up failing (0x4B5980): the original's box (owner the window, the
    // program's name as the title, no icon) and the end of the program.
    auto environment_failed = [] {
        SDL_ShowSimpleMessageBox(0, "Total Annihilation", "Error:  Environment Initialization Failed!\nCheck your DirectX setup", nullptr);
        return 1;
    };
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::printf("SDL_Init: %s\n", SDL_GetError());
        return environment_failed();
    }
    const int scale = std::max(1, o.scale);
    SDL_Window* win = SDL_CreateWindow(o.title.c_str(), w * scale, h * scale, 0);
    if (!win) {
        std::printf("SDL_CreateWindow: %s\n", SDL_GetError());
        return environment_failed();
    }
    SDL_HideCursor();
    confine_pointer(win, nullptr);
    SDL_StartTextInput(win);  // typed characters (Text events) for the text fields
    // (0x4B6B80: MessageBoxA(NULL, text, title, MB_OK | MB_TOPMOST): no owner, no icon; the
    // program waits for it)
    run.system_box = [](const std::string& title, const std::string& text) {
        SDL_ShowCursor();
        SDL_ShowSimpleMessageBox(0, title.c_str(), text.c_str(), nullptr);
        SDL_HideCursor();
    };
    use_system_clipboard();   // the paste keys
    run.warp_ = [win, scale](int x, int y) { SDL_WarpMouseInWindow(win, static_cast<float>(x * scale), static_cast<float>(y * scale)); };
    std::unique_ptr<render::GpuRenderer> gpu;
    if (o.gpu) {
        gpu = render::GpuRenderer::create(run.blend());
        if (!gpu) std::printf("GPU backend unavailable (%s); drawing on the CPU\n", render::GpuRenderer::last_error().c_str());
        else std::printf("GPU backend: %s\n", gpu->driver().c_str());
    }
    SDL_Renderer* sr = nullptr;
    SDL_Texture* tex = nullptr;
    if (!gpu) {
        sr = SDL_CreateRenderer(win, nullptr);
        // Frames are shown at the display's vertical blank (as the original's page flips wait
        // for it): the loop, and what runs once per pass, keeps the display's pace.
        if (sr) SDL_SetRenderVSync(sr, 1);
        tex = sr ? SDL_CreateTexture(sr, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h) : nullptr;
        if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    }
    if (!gpu && !tex) {
        if (sr) SDL_DestroyRenderer(sr);
        SDL_DestroyWindow(win);
        return environment_failed();
    }
    AudioOut audio;
    if (o.sound && !audio.open()) {
        // No output device: the original's DirectSound start failing with "no driver" leaves
        // the game silent and the first main menu says so (0x4CEF90 / 0x4263B0).
        std::printf("no sound output: %s\n", SDL_GetError());
        run.sound_driver_missing_ = true;
    }
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    const std::uint64_t start = SDL_GetTicks();
    if (const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        run.desktop_w_ = dm->w;
        run.desktop_h_ = dm->h;
    }
    {
        // The display's modes (the original lists the display's modes in full-screen play; a
        // windowed display keeps its fixed list).
        int n = 0;
        if (SDL_DisplayMode** list = o.windowed_display ? nullptr : SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &n)) {
            for (int i = 0; i < n; ++i) run.modes_.push_back({list[i]->w, list[i]->h});
            SDL_free(list);
        }
    }
    int cur_w = w, cur_h = h;  // the frame size the window shows (menus 640 x 480, battles their own)
    int frame_no = 0;
    auto present = [&]() -> bool {
        const render::Palette* pal = nullptr;
        render::DrawList list = run.draw(pal);
        if (list.width != cur_w || list.height != cur_h) {
            // The screen size changes between the menus and a battle (the chosen display mode).
            cur_w = list.width;
            cur_h = list.height;
            fit_window_to_frame(win, cur_w, cur_h, scale);
            confine_pointer(win, nullptr);
            rgba.assign(static_cast<std::size_t>(cur_w) * static_cast<std::size_t>(cur_h) * 4, 0);
            if (tex) {
                SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(sr, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, cur_w, cur_h);
                if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
            }
        }
        if (wants_shot(frame_no)) save_shot(render::execute(list, run.blend()), *pal, o.screenshot_prefix, frame_no);
        const render::Palette shown_palette = run.display_palette(*pal);  // (display gamma)
        if (gpu) {
            render::lines_to_sprites(list);
            if (!gpu->present_to_window(win, list, shown_palette, scale)) {
                std::printf("present failed: %s\n", render::GpuRenderer::last_error().c_str());
                return false;
            }
        } else if (tex) {
            const render::Image8 img = render::execute(list, run.blend());
            for (std::size_t i = 0; i < img.pixels.size(); ++i) {
                const render::Rgb& c = shown_palette[img.pixels[i]];
                rgba[i * 4] = c.r, rgba[i * 4 + 1] = c.g, rgba[i * 4 + 2] = c.b, rgba[i * 4 + 3] = 255;
            }
            SDL_UpdateTexture(tex, nullptr, rgba.data(), cur_w * 4);
            SDL_RenderTexture(sr, tex, nullptr, nullptr);
            SDL_RenderPresent(sr);
        }
        run.shown(std::move(list), pal);
        return true;
    };
    run.present_now = [&] {
        SDL_PumpEvents();
        present();
    };
    run.begin(audio.ok() ? &audio : nullptr);
    for (; (o.frames == 0 || frame_no < o.frames) && !run.done(); ++frame_no) {
        const std::uint32_t now = static_cast<std::uint32_t>(SDL_GetTicks() - start);
        feed_script(frame_no, now);
        SDL_Event ev;
        // (A script run drives the window without the system's focus: it counts as focused.)
        if (win && o.script.empty()) run.focused_ = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
        while (SDL_PollEvent(&ev)) {
            confine_pointer(win, &ev);
            InputEvent e;
            const SDL_Keymod mod = SDL_GetModState();
            e.shift = (mod & SDL_KMOD_SHIFT) != 0;
            e.ctrl = (mod & SDL_KMOD_CTRL) != 0;
            e.alt = (mod & SDL_KMOD_ALT) != 0;
            e.time_ms = now;
            switch (ev.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: e.type = InputEvent::Type::Quit; break;
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: pointer_event(win, ev, scale, cur_w, cur_h, e); break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                e.type = ev.type == SDL_EVENT_KEY_DOWN ? InputEvent::Type::KeyDown : InputEvent::Type::KeyUp;
                e.key = sdl_key(ev.key.key);
                if (e.key == 0 || (ev.key.repeat && e.type == InputEvent::Type::KeyDown && e.key >= 0x100 && !key_repeats(e.key))) continue;
                if (e.shift && e.key == '=') e.key = '+';
                if (e.shift && e.key == '-') e.key = '_';
                // A character key without Ctrl or Alt: its character comes as a Text event too.
                e.text_follows = e.type == InputEvent::Type::KeyDown && e.key >= 0x20 && e.key < 0x7F && !e.ctrl && !e.alt;
                break;
            case SDL_EVENT_TEXT_INPUT:
                // Typed characters as the keyboard layout makes them (the chat line's text).
                for (const char* p = ev.text.text; p && *p; ++p)
                    if (static_cast<unsigned char>(*p) < 0x80) {
                        InputEvent t = e;
                        t.type = InputEvent::Type::Text;
                        t.key = static_cast<unsigned char>(*p);
                        run.handle(t);
                    }
                continue;
            default: continue;
            }
            run.handle(e);
        }
        {
            const SDL_Keymod mod = SDL_GetModState();
            run.poll_modifiers((mod & SDL_KMOD_SHIFT) != 0, (mod & SDL_KMOD_CTRL) != 0, (mod & SDL_KMOD_ALT) != 0);
        }
        run.frame(now);
        run.play_pending_sounds();
        if (!present()) break;
        SDL_Delay(1);
    }
    run.present_now = nullptr;
    gpu.reset();
    if (tex) SDL_DestroyTexture(tex);
    if (sr) SDL_DestroyRenderer(sr);
    SDL_DestroyWindow(win);
    return run.exit_code_;
#else
    std::printf("built without SDL3: only --headless is available\n");
    return 1;
#endif
}

void BattleSession::set_allied_victory(int slot, bool on) {
    if (slot >= 0 && slot < 10) battle().victory.sides[static_cast<std::size_t>(slot)].allied_victory = on;
}

}  // namespace client
