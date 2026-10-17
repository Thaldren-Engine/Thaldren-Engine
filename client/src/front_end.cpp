// The front end's runner: the screen stack, the battle session, the loading screen,
// sounds and music, and the main loop (an SDL3 window with the GPU backend, or a headless
// run with scripted input and screenshots). See client/front_end.h.

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <set>

#include "client/audio.h"
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
    }
    ~Runner() override {
        // Screens go before the battle they may refer to.
        graveyard_.clear();
        while (!stack_.empty()) stack_.pop_back();
    }

    // ---- FrontEnd
    void push(std::unique_ptr<Screen> screen) override {
        if (!screen) return;
        stack_.push_back(std::move(screen));
        stack_.back()->enter(*this);
    }
    void pop() override {
        if (!stack_.empty()) trim(stack_.size() - 1);
        if (session_ && ended_ && stack_.size() <= battle_base_) finish_battle();
        else if (session_ && stack_.size() < battle_base_) finish_battle();
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
    void quit() override { quit_ = true; }
    Settings& settings() override { return settings_; }
    FrontEndHooks& hooks() override { return s_.hooks; }
    const MenuContent& menu_content() const override { return *s_.content; }
    int width() const override { return s_.width; }
    int height() const override { return s_.height; }
    void play_sound(int sample) override {
        if (sample >= 0) menu_sounds_.push_back(sample);
    }
    MusicPlayer* music() override { return s_.music; }
    void apply_settings() override {
        // Volumes 0..64 (fxvol, musicvol); sound mode 0 is off.
        fx_volume_ = settings_.get_int("Sound Mode", 1) == 0 ? 0 : settings_.get_int("fxvol", 27) * 256 / 64;
        if (audio_) audio_->set_fx_volume(fx_volume_);
        if (s_.music) {
            s_.music->set_volume(settings_.get_int("musicvol", 32));
            s_.music->set_mode(settings_.get_int("cdmode", 4));
            s_.music->set_enabled(settings_.get_int("musicmode", 1) != 0);
        }
        if (session_ && s_.apply_to_battle) s_.apply_to_battle(*this, *session_);
        if (!s_.settings_path.empty()) settings_.save(s_.settings_path);
    }
    std::uint32_t now_ms() const override { return now_; }
    void desktop_size(int& w, int& h) const override {
        w = desktop_w_;
        h = desktop_h_;
    }
    int desktop_w_ = 1920, desktop_h_ = 1080;
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
        apply_settings();
        if (s_.main_menu) push(s_.main_menu(*this));
        if (s_.music) s_.music->set_mood(MusicMood::Silent);
    }
    bool done() const { return quit_ || (stack_.empty() && !session_ && !pending_); }

    void handle(const InputEvent& e) {
        if (e.type == InputEvent::Type::Quit) {
            quit_ = true;
            return;
        }
        if (e.type == InputEvent::Type::MouseMove || e.type == InputEvent::Type::MouseDown || e.type == InputEvent::Type::MouseUp) {
            mouse_x_ = e.x;
            mouse_y_ = e.y;
        }
        if (pending_) return;
        if (in_battle() && stack_.size() == battle_base_) {
            GameClient& c = session_->client();
            // Tab or F2 opens the game menu (the client keeps keys while its chat line is open).
            // F2 opens the game menu; Tab too, except in network games where it opens the tab bar.
            if (e.type == InputEvent::Type::KeyDown && !c.chatting() && (e.key == keys::kTab || e.key == keys::kF1 + 1)) {
                if (e.key == keys::kTab && session_->network() && s_.tab_menu) push(s_.tab_menu(*this, *session_));
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

    void frame(std::uint32_t now) {
        now_ = now;
        graveyard_.clear();  // screens closed during the last pass go now
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
        if (pending_) run_launcher();
        if (in_battle()) {
            // The battle goes on under menus (the game menu pauses it outside network games).
            session_->frame(now);
            sim::Battle& b = session_->battle();
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
            if (b.end_game.flags & sim::end_game_flags::kEnding) {
                const BattleOutcome o = (b.end_game.flags & sim::end_game_flags::kWon) ? BattleOutcome::Victory : BattleOutcome::Defeat;
                end_battle(o);
            }
        }
        for (auto& sc : stack_)
            if (sc) {
                if (&sc == &stack_.back()) sc->frame(*this, now);
            }
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
            if (s_.draw_loading) s_.draw_loading(*this, img, load_fraction_, load_stage_);
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
        if (in_battle()) {
            const std::vector<Sample>& smp = session_->content().samples;
            const std::vector<SoundEvent> started = session_->client().take_sounds();
            if (log_sounds_ && !started.empty()) {
                // The sound log (headless runs): the battle's sounds started in this pass.
                std::printf("step %u: %zu sound(s):", session_->battle().game_tick(), started.size());
                for (const SoundEvent& e : started)
                    std::printf(" %s/%d", e.sample >= 0 && static_cast<std::size_t>(e.sample) < smp.size() ? smp[static_cast<std::size_t>(e.sample)].name.c_str() : "?",
                                e.volume);
                std::printf("\n");
            }
            for (const SoundEvent& e : started)
                if (audio_ && e.sample >= 0 && static_cast<std::size_t>(e.sample) < smp.size())
                    audio_->play_sample(&smp[static_cast<std::size_t>(e.sample)], e.pan, e.volume);
        }
        const std::vector<Sample>& ms = s_.content->samples;
        for (int i : menu_sounds_)
            if (audio_ && static_cast<std::size_t>(i) < ms.size()) audio_->play_sample(&ms[static_cast<std::size_t>(i)], 0, 256);
        sound_count_ += menu_sounds_.size();
        menu_sounds_.clear();
    }

    // The loop sets this: draws and shows a frame at once (the loading screen's progress).
    std::function<void()> present_now;
    const render::BlendTable& blend() const { return s_.content->blend; }
    float load_fraction() const { return load_fraction_; }

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

    void add_pointer(render::DrawList& list) {
        if (!draw_pointer_) return;
        const auto* c = s_.content->cursor;
        if (c && !c->empty()) {
            if (pointer_.ended) return;
            const render::Sprite8& s = (*c)[static_cast<std::size_t>(pointer_.frame) % c->size()];
            list.blit(&s, mouse_x_, mouse_y_, {0, 0, list.width - 1, list.height - 1}, true);
        }
    }

    void run_launcher() {
        BattleLauncher l = std::move(pending_);
        pending_ = nullptr;
        loading_ = true;
        load_fraction_ = 0.0f;
        load_stage_ = "Loading";
        if (present_now) present_now();
        std::string error;
        auto progress = [this](float f, const std::string& stage) {
            load_fraction_ = f;
            load_stage_ = stage;
            std::printf("loading: %3d%% %s\n", static_cast<int>(f * 100.0f), stage.c_str());
            if (present_now) present_now();
        };
        std::unique_ptr<BattleSession> s = l(progress, error);
        loading_ = false;
        if (!s) {
            std::printf("battle failed to start: %s\n", error.c_str());
            return;
        }
        session_ = std::move(s);
        ended_ = false;
        mood_set_ = false;
        battle_base_ = stack_.size();
        judge_.reset();
        if (s_.music) s_.music->set_mood(MusicMood::Building);
        BattleSession* sess = session_.get();
        session_->client().on_chat = [sess](const std::string& text) { sess->send_chat(text); };
        session_->combat_activity = [this](int amount) { judge_.activity(amount); };
        if (s_.apply_to_battle) s_.apply_to_battle(*this, *session_);
    }

    void finish_battle(bool ask_session = true) {
        if (!session_) return;
        std::unique_ptr<BattleSession> s = std::move(session_);
        const BattleOutcome o = ended_ ? outcome_ : BattleOutcome::Quit;
        ended_ = false;
        battle_base_ = 0;
        if (s_.music) s_.music->set_mood(MusicMood::Silent);
        if (audio_) audio_->set_samples(nullptr);
        if (ask_session) s->after_battle(*this, o);
        s.reset();
        if (!stack_.empty()) stack_.back()->enter(*this);
    }

    FrontEndSetup& s_;
    Settings settings_;
    std::vector<std::unique_ptr<Screen>> stack_;
    std::vector<std::unique_ptr<Screen>> graveyard_;
    std::unique_ptr<BattleSession> session_;
    std::size_t battle_base_ = 0;  // stack size when the battle started (screens above are its menus)
    bool ended_ = false, mood_set_ = false;
    BattleOutcome outcome_ = BattleOutcome::None;
    BattleLauncher pending_;
    bool loading_ = false;
    float load_fraction_ = 0.0f;
    std::string load_stage_;
    bool quit_ = false;
    AudioOut* audio_ = nullptr;
    int fx_volume_ = 256;
    std::vector<int> menu_sounds_;
    std::uint32_t now_ = 0;
    FrameClock pointer_;  // the menu pointer's animation
    std::uint32_t pointer_ticks_ = 0;
    bool pointer_started_ = false;
    int mouse_x_ = 0, mouse_y_ = 0;
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
        }
        std::printf("front end (headless): %d frames, %zu interface sounds\n", f, run.sound_count_);
        return 0;
    }
#ifdef CLIENT_HAVE_SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::printf("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const int scale = std::max(1, o.scale);
    SDL_Window* win = SDL_CreateWindow("Battle", w * scale, h * scale, 0);
    if (!win) {
        std::printf("SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_HideCursor();
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
        tex = sr ? SDL_CreateTexture(sr, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h) : nullptr;
        if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    }
    AudioOut audio;
    if (o.sound && !audio.open(nullptr)) std::printf("no sound output: %s\n", SDL_GetError());
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    const std::uint64_t start = SDL_GetTicks();
    if (const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        run.desktop_w_ = dm->w;
        run.desktop_h_ = dm->h;
    }
    {
        // The display's modes (the original lists the display's modes in full-screen play).
        int n = 0;
        if (SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &n)) {
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
            rgba.assign(static_cast<std::size_t>(cur_w) * static_cast<std::size_t>(cur_h) * 4, 0);
            if (tex) {
                SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(sr, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, cur_w, cur_h);
                if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
            }
        }
        if (wants_shot(frame_no)) save_shot(render::execute(list, run.blend()), *pal, o.screenshot_prefix, frame_no);
        if (gpu) {
            render::lines_to_sprites(list);
            if (!gpu->present_to_window(win, list, *pal, scale)) {
                std::printf("present failed: %s\n", render::GpuRenderer::last_error().c_str());
                return false;
            }
        } else if (tex) {
            const render::Image8 img = render::execute(list, run.blend());
            for (std::size_t i = 0; i < img.pixels.size(); ++i) {
                const render::Rgb& c = (*pal)[img.pixels[i]];
                rgba[i * 4] = c.r, rgba[i * 4 + 1] = c.g, rgba[i * 4 + 2] = c.b, rgba[i * 4 + 3] = 255;
            }
            SDL_UpdateTexture(tex, nullptr, rgba.data(), cur_w * 4);
            SDL_RenderTexture(sr, tex, nullptr, nullptr);
            SDL_RenderPresent(sr);
        }
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
        while (SDL_PollEvent(&ev)) {
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
                e.type = InputEvent::Type::MouseMove;
                e.x = std::clamp(static_cast<int>(ev.motion.x) / scale, 0, cur_w - 1);
                e.y = std::clamp(static_cast<int>(ev.motion.y) / scale, 0, cur_h - 1);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                e.type = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? InputEvent::Type::MouseDown : InputEvent::Type::MouseUp;
                e.x = std::clamp(static_cast<int>(ev.button.x) / scale, 0, cur_w - 1);
                e.y = std::clamp(static_cast<int>(ev.button.y) / scale, 0, cur_h - 1);
                e.button = ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_RIGHT ? 3 : 2;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                e.type = ev.type == SDL_EVENT_KEY_DOWN ? InputEvent::Type::KeyDown : InputEvent::Type::KeyUp;
                e.key = sdl_key(ev.key.key);
                if (e.key == 0 || (ev.key.repeat && e.type == InputEvent::Type::KeyDown && e.key >= 0x100 && e.key != keys::kBackspace)) continue;
                if (e.shift && e.key == '=') e.key = '+';
                if (e.shift && e.key == '-') e.key = '_';
                break;
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
    return 0;
#else
    std::printf("built without SDL3: only --headless is available\n");
    return 1;
#endif
}

}  // namespace client
