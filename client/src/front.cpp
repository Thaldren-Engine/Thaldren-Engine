#include "client/front.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <sstream>

#include "render/draw_list.h"
#include "render/presenter.h"

#ifdef CLIENT_HAVE_SDL
#include <SDL3/SDL.h>

#include "client/audio.h"
#include "render/gpu_renderer.h"
#endif

namespace client {
namespace {

int key_by_name(const std::string& s) {
    if (s.size() == 1) return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(s[0])));
    if (s == "left") return keys::kLeft;
    if (s == "right") return keys::kRight;
    if (s == "up") return keys::kUp;
    if (s == "down") return keys::kDown;
    if (s == "pause") return keys::kPause;
    if (s == "escape") return keys::kEscape;
    if (s == "enter") return keys::kEnter;
    if (s == "tab") return keys::kTab;
    if (s == "plus") return '+';
    if (s == "minus") return '-';
    if (s.size() >= 2 && s[0] == 'f') return keys::kF1 + std::atoi(s.c_str() + 1) - 1;
    return 0;
}

void save(const render::Image8& frame, const render::Palette& pal, const std::string& prefix, int n) {
    char name[32];
    std::snprintf(name, sizeof name, "%04d.bmp", n);
    render::write_bmp(prefix + name, frame, pal);
    std::printf("screenshot %s%s\n", prefix.c_str(), name);
}

} // namespace

bool parse_script(const std::string& text, std::vector<ScriptedInput>& out, std::string& error) {
    std::istringstream in(text);
    std::string line;
    int ln = 0;
    while (std::getline(in, line)) {
        ++ln;
        if (const auto c = line.find('#'); c != std::string::npos) line.resize(c);
        std::istringstream ls(line);
        std::vector<std::string> w;
        for (std::string t; ls >> t;) w.push_back(t);
        if (w.empty()) continue;
        InputEvent e;
        while (!w.empty() && w.back()[0] == '+') {
            if (w.back() == "+shift") e.shift = true;
            else if (w.back() == "+ctrl") e.ctrl = true;
            else if (w.back() == "+alt") e.alt = true;
            w.pop_back();
        }
        if (w.size() < 2) {
            error = "line " + std::to_string(ln) + ": too short";
            return false;
        }
        const int frame = std::atoi(w[0].c_str());
        const std::string& ev = w[1];
        auto num = [&](std::size_t i) { return i < w.size() ? std::atoi(w[i].c_str()) : 0; };
        if (ev == "move") {
            e.type = InputEvent::Type::MouseMove;
            e.x = num(2), e.y = num(3);
            out.push_back({frame, e});
        } else if (ev == "down" || ev == "up" || ev == "click") {
            e.x = num(2), e.y = num(3), e.button = w.size() > 4 ? num(4) : 1;
            InputEvent m = e;
            m.type = InputEvent::Type::MouseMove;
            out.push_back({frame, m});
            if (ev != "up") {
                e.type = InputEvent::Type::MouseDown;
                out.push_back({frame, e});
            }
            if (ev != "down") {
                e.type = InputEvent::Type::MouseUp;
                out.push_back({frame, e});
            }
        } else if (ev == "key" || ev == "keyup") {
            e.type = ev == "key" ? InputEvent::Type::KeyDown : InputEvent::Type::KeyUp;
            e.key = key_by_name(w[2]);
            out.push_back({frame, e});
            if (ev == "key" && e.key < 0x100) {  // characters are typed: down and up
                InputEvent u = e;
                u.type = InputEvent::Type::KeyUp;
                out.push_back({frame, u});
            }
        } else if (ev == "quit") {
            e.type = InputEvent::Type::Quit;
            out.push_back({frame, e});
        } else {
            error = "line " + std::to_string(ln) + ": unknown event " + ev;
            return false;
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const ScriptedInput& a, const ScriptedInput& b) { return a.frame < b.frame; });
    return true;
}

namespace {

int run_headless(GameClient& client, Content& content, const FrontOptions& o) {
#ifdef CLIENT_HAVE_SDL
    std::unique_ptr<render::GpuRenderer> gpu;
    if (o.gpu_check) {
        gpu = render::GpuRenderer::create(content.blend);
        if (!gpu) std::printf("GPU backend unavailable: %s\n", render::GpuRenderer::last_error().c_str());
    }
#endif
    std::size_t next = 0;
    int total_steps = 0;
    // CLIENT_PROFILE=N: every N frames, the average time of each part of a frame, with the CPU
    // and (when there is a GPU) the GPU path drawn every frame as a window would.
    const int profile = std::getenv("CLIENT_PROFILE") ? std::max(1, std::atoi(std::getenv("CLIENT_PROFILE"))) : 0;
#ifdef CLIENT_HAVE_SDL
    if (profile && !gpu) gpu = render::GpuRenderer::create(content.blend);
#endif
    using clock = std::chrono::steady_clock;
    double t_sim = 0, t_list = 0, t_cpu = 0, t_lines = 0, t_gpu = 0;
    std::size_t cmds = 0, lines = 0;
    for (int f = 0; f < o.frames && !client.quit_requested(); ++f) {
        const std::uint32_t now = static_cast<std::uint32_t>(static_cast<std::uint64_t>(f) * 1000u / 30u);
        for (; next < o.script.size() && o.script[next].frame <= f; ++next) {
            InputEvent e = o.script[next].event;
            e.time_ms = now;
            client.handle(e);
        }
        const auto t0 = clock::now();
        total_steps += client.frame(now);
        const auto t1 = clock::now();
        const auto sounds = client.take_sounds();
        if (!sounds.empty()) {
            // The sound log: every sound started this frame (name, volume), and the same sound
            // started twice in one frame flagged.
            std::printf("frame %d (step %u): %zu sound(s):", f, client.battle().game_tick(), sounds.size());
            for (std::size_t i = 0; i < sounds.size(); ++i) {
                const SoundEvent& s = sounds[i];
                const bool known = s.sample >= 0 && static_cast<std::size_t>(s.sample) < content.samples.size();
                bool twice = false;
                for (std::size_t k = 0; k < i; ++k) twice |= sounds[k].sample == s.sample;
                std::printf(" %s/%d%s", known ? content.samples[static_cast<std::size_t>(s.sample)].name.c_str() : "?", s.volume,
                            twice ? "(again)" : "");
            }
            std::printf("\n");
        }
        render::DrawList list = client.draw();
        if (profile) {
            const auto t2 = clock::now();
            const render::Image8 img = render::execute(list, content.blend);
            const auto t3 = clock::now();
            for (const auto& c : list.cmds) lines += c.op == render::DrawOp::Line;
            cmds += list.cmds.size();
            render::lines_to_sprites(list);
            const auto t4 = clock::now();
#ifdef CLIENT_HAVE_SDL
            render::Image8 g;
            if (gpu) gpu->render(list, g);
#endif
            const auto t5 = clock::now();
            auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
            t_sim += ms(t1 - t0), t_list += ms(t2 - t1), t_cpu += ms(t3 - t2), t_lines += ms(t4 - t3), t_gpu += ms(t5 - t4);
            if ((f + 1) % profile == 0) {
                const double n = profile;
                std::printf("profile frames %d-%d: sim %.2f ms, draw list %.2f ms (%zu cmds, %zu lines), cpu raster %.2f ms, "
                            "lines->sprites %.2f ms, gpu %.2f ms\n",
                            f + 1 - profile, f, t_sim / n, t_list / n, cmds / static_cast<std::size_t>(profile),
                            lines / static_cast<std::size_t>(profile), t_cpu / n, t_lines / n, t_gpu / n);
                t_sim = t_list = t_cpu = t_lines = t_gpu = 0;
                cmds = lines = 0;
            }
        }
        if (std::find(o.screenshot_frames.begin(), o.screenshot_frames.end(), f) != o.screenshot_frames.end()) {
            const render::Image8 img = render::execute(list, content.blend);
            save(img, content.palette, o.screenshot_prefix, f);
#ifdef CLIENT_HAVE_SDL
            if (gpu) {
                render::lines_to_sprites(list);
                render::Image8 g;
                if (gpu->render(list, g)) {
                    std::size_t diff = 0;
                    for (std::size_t i = 0; i < g.pixels.size() && i < img.pixels.size(); ++i) diff += g.pixels[i] != img.pixels[i];
                    std::printf("  GPU (%s) vs CPU: %zu differing pixels\n", gpu->driver().c_str(), diff);
                    save(g, content.palette, o.screenshot_prefix + "gpu_", f);
                }
            }
#endif
        }
    }
    std::printf("headless: %d frames, %d simulation steps\n", o.frames, total_steps);
    return 0;
}

#ifdef CLIENT_HAVE_SDL
int translate_key(SDL_Keycode k) {
    switch (k) {
    case SDLK_LEFT: return keys::kLeft;
    case SDLK_RIGHT: return keys::kRight;
    case SDLK_UP: return keys::kUp;
    case SDLK_DOWN: return keys::kDown;
    case SDLK_PAUSE: return keys::kPause;
    case SDLK_ESCAPE: return keys::kEscape;
    case SDLK_RETURN: return keys::kEnter;
    case SDLK_TAB: return keys::kTab;
    case SDLK_KP_PLUS: return '+';
    case SDLK_KP_MINUS: return '-';
    default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) return keys::kF1 + static_cast<int>(k - SDLK_F1);
    if (k >= 32 && k < 127) return static_cast<int>(std::tolower(static_cast<int>(k)));
    return 0;
}

}  // namespace

void fit_window_to_frame(SDL_Window* window, int frame_w, int frame_h, int scale) {
    const int w = frame_w * scale, h = frame_h * scale;
    const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window));
    const bool whole_display = dm && w >= dm->w && h >= dm->h;
    if (whole_display) {
        SDL_SetWindowFullscreenMode(window, nullptr);  // the desktop's own mode: no mode change
        SDL_SetWindowFullscreen(window, true);
    } else {
        SDL_SetWindowFullscreen(window, false);
        SDL_SetWindowSize(window, w, h);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    SDL_SyncWindow(window);
}

namespace {

int run_window(GameClient& client, Content& content, const FrontOptions& o) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::printf("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const int w = client.options().width, h = client.options().height, scale = std::max(1, o.scale);
    SDL_Window* win = SDL_CreateWindow("Battle", w * scale, h * scale, 0);
    if (!win) {
        std::printf("SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    fit_window_to_frame(win, w, h, scale);
    SDL_HideCursor();
    std::printf("window %dx%d (scale %d)\n", w * scale, h * scale, scale);
    std::unique_ptr<render::GpuRenderer> gpu;
    if (o.gpu) {
        gpu = render::GpuRenderer::create(content.blend);
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
    if (o.sound && !audio.open(&content.samples)) std::printf("no sound output: %s\n", SDL_GetError());
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    const std::uint64_t start = SDL_GetTicks();
    std::size_t next = 0;
    for (int f = 0; (o.frames == 0 || f < o.frames) && !client.quit_requested(); ++f) {
        const std::uint32_t now = static_cast<std::uint32_t>(SDL_GetTicks() - start);
        for (; next < o.script.size() && o.script[next].frame <= f; ++next) {
            InputEvent e = o.script[next].event;
            e.time_ms = now;
            client.handle(e);
        }
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
                e.x = std::clamp(static_cast<int>(ev.motion.x) / scale, 0, w - 1);
                e.y = std::clamp(static_cast<int>(ev.motion.y) / scale, 0, h - 1);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                e.type = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? InputEvent::Type::MouseDown : InputEvent::Type::MouseUp;
                e.x = std::clamp(static_cast<int>(ev.button.x) / scale, 0, w - 1);
                e.y = std::clamp(static_cast<int>(ev.button.y) / scale, 0, h - 1);
                e.button = ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_RIGHT ? 3 : 2;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                e.type = ev.type == SDL_EVENT_KEY_DOWN ? InputEvent::Type::KeyDown : InputEvent::Type::KeyUp;
                e.key = translate_key(ev.key.key);
                if (e.key == 0 || (ev.key.repeat && e.type == InputEvent::Type::KeyDown && e.key >= 0x100)) continue;
                // Shifted characters the original maps to their own codes.
                if (e.shift && e.key == '=') e.key = '+';
                if (e.shift && e.key == '-') e.key = '_';
                break;
            default: continue;
            }
            client.handle(e);
        }
        client.frame(now);
        for (const SoundEvent& s : client.take_sounds())
            if (s.sample >= 0 && static_cast<std::size_t>(s.sample) < content.samples.size())
                audio.play_sample(&content.samples[static_cast<std::size_t>(s.sample)], s.pan, s.volume);
        render::DrawList list = client.draw();
        if (std::find(o.screenshot_frames.begin(), o.screenshot_frames.end(), f) != o.screenshot_frames.end())
            save(render::execute(list, content.blend), content.palette, o.screenshot_prefix, f);
        if (gpu) {
            render::lines_to_sprites(list);
            if (!gpu->present_to_window(win, list, content.palette, scale)) {
                std::printf("present failed: %s\n", render::GpuRenderer::last_error().c_str());
                break;
            }
        } else if (tex) {
            const render::Image8 img = render::execute(list, content.blend);
            for (std::size_t i = 0; i < img.pixels.size(); ++i) {
                const render::Rgb& c = content.palette[img.pixels[i]];
                rgba[i * 4] = c.r, rgba[i * 4 + 1] = c.g, rgba[i * 4 + 2] = c.b, rgba[i * 4 + 3] = 255;
            }
            SDL_UpdateTexture(tex, nullptr, rgba.data(), w * 4);
            SDL_RenderTexture(sr, tex, nullptr, nullptr);
            SDL_RenderPresent(sr);
        } else {
            SDL_Delay(10);
        }
    }
    gpu.reset();
    if (tex) SDL_DestroyTexture(tex);
    if (sr) SDL_DestroyRenderer(sr);
    SDL_DestroyWindow(win);
    return 0;
}
#endif

} // namespace

int run_client(GameClient& client, Content& content, const FrontOptions& options) {
    if (options.headless) return run_headless(client, content, options);
#ifdef CLIENT_HAVE_SDL
    return run_window(client, content, options);
#else
    std::printf("built without SDL3: only --headless is available\n");
    return 1;
#endif
}

} // namespace client
