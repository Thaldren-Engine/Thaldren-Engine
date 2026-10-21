#include "client/front.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <sstream>

#include "client/audio.h"
#include "client/menu.h"
#include "client/sound_desk.h"
#include "render/draw_list.h"
#include "render/presenter.h"

#ifdef CLIENT_HAVE_SDL
#include <SDL3/SDL.h>

#include "render/gpu_renderer.h"
#endif

namespace client {

void use_system_clipboard() {
#ifdef CLIENT_HAVE_SDL
    // The clipboard's text (UTF-8) in the game's 8-bit set: characters below 256 as they are,
    // others '?' (as the system's conversion to the ANSI set replaces what it cannot show).
    clipboard_text = [](std::string& out) {
        if (!SDL_HasClipboardText()) return false;
        char* t = SDL_GetClipboardText();
        if (!t) return false;
        out.clear();
        const unsigned char* p = reinterpret_cast<const unsigned char*>(t);
        while (*p) {
            unsigned cp = *p++;
            int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
            if (more) cp &= 0x3Fu >> more;
            for (; more > 0 && (*p & 0xC0) == 0x80; --more) cp = (cp << 6) | (*p++ & 0x3Fu);
            out.push_back(cp < 0x100 ? static_cast<char>(cp) : '?');
        }
        SDL_free(t);
        return true;
    };
#endif
}

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
    if (s == "backspace") return keys::kBackspace;
    if (s == "delete") return keys::kDelete;
    if (s == "insert") return keys::kInsert;
    if (s == "home") return keys::kHome;
    if (s == "end") return keys::kEnd;
    if (s == "space") return ' ';
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
        while (!w.empty() && (w.back() == "+shift" || w.back() == "+ctrl" || w.back() == "+alt")) {
            if (w.back() == "+shift") e.shift = true;
            else if (w.back() == "+ctrl") e.ctrl = true;
            else e.alt = true;
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
        } else if (ev == "wmove") {
            // wmove X Y W H [SCALE]: the system pointer at window pixel (X, Y) of a W x H window
            // (may lie outside it) — what the window front ends make of a motion event.
            const int scale = w.size() > 6 ? std::max(1, num(6)) : 1;
            InputEvent m = pointer_motion(static_cast<float>(num(2)), static_cast<float>(num(3)), scale, num(4) / scale, num(5) / scale, num(4), num(5));
            m.shift = e.shift, m.ctrl = e.ctrl, m.alt = e.alt;
            out.push_back({frame, m});
        } else if (ev == "leave") {
            e.type = InputEvent::Type::PointerLeave;
            out.push_back({frame, e});
        } else if (ev == "deactivate" || ev == "activate") {
            // The program window deactivated / activated (WM_ACTIVATE, Alt+Tab).
            e.type = InputEvent::Type::Activate;
            e.key = ev == "activate" ? 1 : 0;
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
        } else if (ev == "key" || ev == "keyup" || ev == "hold") {
            // (hold: the key goes down and stays down until a keyup)
            e.type = ev == "keyup" ? InputEvent::Type::KeyUp : InputEvent::Type::KeyDown;
            e.key = key_by_name(w[2]);
            out.push_back({frame, e});
            if (ev == "key" && e.key < 0x100) {  // characters are typed: down and up
                InputEvent u = e;
                u.type = InputEvent::Type::KeyUp;
                out.push_back({frame, u});
            }
        } else if (ev == "type") {
            // Types the rest of the line (words joined by single spaces) key by key; capitals
            // are typed with shift.
            std::string typed;
            for (std::size_t i = 2; i < w.size(); ++i) typed += (i > 2 ? " " : "") + w[i];
            for (char ch : typed) {
                InputEvent k = e;
                k.type = InputEvent::Type::KeyDown;
                k.key = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(ch)));
                k.shift = std::isupper(static_cast<unsigned char>(ch)) != 0;
                if (ch == '+') k.key = '+';
                out.push_back({frame, k});
                k.type = InputEvent::Type::KeyUp;
                out.push_back({frame, k});
            }
        } else if (ev == "clip") {
            // clip TEXT: the clipboard's text for the run (the rest of the line, words joined
            // by single spaces) for the paste keys of headless runs.
            static std::string scripted;
            scripted.clear();
            for (std::size_t i = 2; i < w.size(); ++i) scripted += (i > 2 ? " " : "") + w[i];
            if (!clipboard_text) clipboard_text = [](std::string& o) {
                o = scripted;
                return true;
            };
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
    SoundDesk desk;
    desk.set_log(true);
    bool shot_pending = false;
    render::DrawList last_shown;  // the frame shown last (the screenshot key)
    for (int f = 0; f < o.frames && !client.quit_requested(); ++f) {
        const std::uint32_t now = static_cast<std::uint32_t>(static_cast<std::uint64_t>(f) * 1000u / 30u);
        for (; next < o.script.size() && o.script[next].frame <= f; ++next) {
            InputEvent e = o.script[next].event;
            e.time_ms = now;
            if (is_screenshot_key(e)) {
                shot_pending = true;
                continue;
            }
            client.handle(e);
        }
        if (shot_pending && last_shown.width > 0) {
            const std::string path = save_numbered_picture(o.shot_dir, o.shot_prefix, render::execute(last_shown, content.blend), content.palette);
            std::printf("screenshot %s\n", path.empty() ? "(not written)" : path.c_str());
        }
        shot_pending = false;
        const auto t0 = clock::now();
        total_steps += client.frame(now);
        const auto t1 = clock::now();
        // The sound log: every sound started this frame (name, volume, copy, placement) and
        // every sound cut off to make room.
        desk.tick(now);
        for (const SoundEvent& e : client.take_sounds())
            desk.play_event(content.samples, e.sample, e.volume, e.placed, e.pos, e.min_distance, e.max_distance, e.one_shot, now);
        desk.flush("frame " + std::to_string(f) + " step " + std::to_string(client.battle().game_tick()));
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
        last_shown = std::move(list);
        if (client.film_due()) {
            save_numbered_picture(client.film_folder(), "FRAM", render::execute(last_shown, content.blend), content.palette);
            client.film_written(now);
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

void confine_pointer(SDL_Window* window, const SDL_Event* ev) {
    if (!window) return;
    const SDL_WindowFlags f = SDL_GetWindowFlags(window);
    bool focused = (f & SDL_WINDOW_INPUT_FOCUS) != 0;
    bool shown = !(f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN));
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    // Where the system pointer is: the event's own position for mouse events, else asked.
    bool inside = false;
    if (ev && ev->type == SDL_EVENT_MOUSE_MOTION) {
        inside = ev->motion.x >= 0 && ev->motion.y >= 0 && ev->motion.x < static_cast<float>(ww) && ev->motion.y < static_cast<float>(wh);
    } else if (ev && ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        inside = ev->button.x >= 0 && ev->button.y >= 0 && ev->button.x < static_cast<float>(ww) && ev->button.y < static_cast<float>(wh);
    } else if (ev && ev->type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
        inside = false;
    } else {
        float gx = 0, gy = 0;
        int wx = 0, wy = 0;
        SDL_GetGlobalMouseState(&gx, &gy);
        SDL_GetWindowPosition(window, &wx, &wy);
        inside = gx >= static_cast<float>(wx) && gy >= static_cast<float>(wy) && gx < static_cast<float>(wx + ww) && gy < static_cast<float>(wy + wh);
    }
    if (ev) switch (ev->type) {
        // (The window's flags may not say so yet when these come.)
        case SDL_EVENT_WINDOW_FOCUS_LOST: focused = false; break;
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN: shown = false; break;
        default: break;
        }
    const bool want = pointer_held(focused, shown, inside);
    if (want != SDL_GetWindowMouseGrab(window)) SDL_SetWindowMouseGrab(window, want);
}

bool pointer_event(SDL_Window* window, const SDL_Event& ev, int scale, int frame_w, int frame_h, InputEvent& out) {
    int ww = frame_w * scale, wh = frame_h * scale;
    if (window) SDL_GetWindowSize(window, &ww, &wh);
    switch (ev.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const InputEvent m = pointer_motion(ev.motion.x, ev.motion.y, scale, frame_w, frame_h, ww, wh);
        out.type = m.type;
        out.x = m.x;
        out.y = m.y;
        return true;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        out.type = InputEvent::Type::PointerLeave;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        out.type = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? InputEvent::Type::MouseDown : InputEvent::Type::MouseUp;
        out.x = std::clamp(static_cast<int>(ev.button.x) / std::max(1, scale), 0, frame_w - 1);
        out.y = std::clamp(static_cast<int>(ev.button.y) / std::max(1, scale), 0, frame_h - 1);
        out.button = ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_RIGHT ? 3 : 2;
        return true;
    default: return false;
    }
}

namespace {

int run_window(GameClient& client, Content& content, const FrontOptions& o) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::printf("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const int w = client.options().width, h = client.options().height, scale = std::max(1, o.scale);
    SDL_Window* win = SDL_CreateWindow(o.title.c_str(), w * scale, h * scale, 0);
    if (!win) {
        std::printf("SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    fit_window_to_frame(win, w, h, scale);
    SDL_HideCursor();
    confine_pointer(win, nullptr);
    SDL_StartTextInput(win);  // typed characters (Text events) for the text fields
    use_system_clipboard();   // the paste keys
    client.warp_pointer = [win, scale](int x, int y) { SDL_WarpMouseInWindow(win, static_cast<float>(x * scale), static_cast<float>(y * scale)); };
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
    if (o.sound && !audio.open()) std::printf("no sound output: %s\n", SDL_GetError());
    SoundDesk desk;
    desk.attach(audio.ok() ? &audio : nullptr);
    desk.set_fx_volume(client.options().fx_volume);
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    const std::uint64_t start = SDL_GetTicks();
    std::size_t next = 0;
    bool shot_pending = false;
    render::DrawList last_shown;  // the frame shown last (the screenshot key)
    for (int f = 0; (o.frames == 0 || f < o.frames) && !client.quit_requested(); ++f) {
        const std::uint32_t now = static_cast<std::uint32_t>(SDL_GetTicks() - start);
        for (; next < o.script.size() && o.script[next].frame <= f; ++next) {
            InputEvent e = o.script[next].event;
            e.time_ms = now;
            client.handle(e);
        }
        SDL_Event ev;
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
            case SDL_EVENT_MOUSE_BUTTON_UP: pointer_event(win, ev, scale, w, h, e); break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                e.type = ev.type == SDL_EVENT_KEY_DOWN ? InputEvent::Type::KeyDown : InputEvent::Type::KeyUp;
                e.key = translate_key(ev.key.key);
                if (e.key == 0 || (ev.key.repeat && e.type == InputEvent::Type::KeyDown && e.key >= 0x100 && !key_repeats(e.key))) continue;
                // Shifted characters the original maps to their own codes.
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
                        client.handle(t);
                    }
                continue;
            default: continue;
            }
            if (is_screenshot_key(e)) {
                shot_pending = true;  // (taken before the next pass draws: the frame shown last)
                continue;
            }
            client.handle(e);
        }
        {
            const SDL_Keymod mod = SDL_GetModState();
            client.poll_modifiers((mod & SDL_KMOD_SHIFT) != 0, (mod & SDL_KMOD_CTRL) != 0, (mod & SDL_KMOD_ALT) != 0);
        }
        if (shot_pending && last_shown.width > 0) {
            const std::string path = save_numbered_picture(o.shot_dir, o.shot_prefix, render::execute(last_shown, content.blend), content.palette);
            std::printf("screenshot %s\n", path.empty() ? "(not written)" : path.c_str());
        }
        shot_pending = false;
        client.frame(now);
        desk.tick(now);
        for (const SoundEvent& e : client.take_sounds())
            desk.play_event(content.samples, e.sample, e.volume, e.placed, e.pos, e.min_distance, e.max_distance, e.one_shot, now);
        desk.flush(std::string());
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
        last_shown = std::move(list);
        if (client.film_due()) {
            save_numbered_picture(client.film_folder(), "FRAM", render::execute(last_shown, content.blend), content.palette);
            client.film_written(now);
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

InputEvent pointer_motion(float x, float y, int scale, int frame_w, int frame_h, int win_w, int win_h) {
    InputEvent e;
    if (x < 0 || y < 0 || x >= static_cast<float>(win_w) || y >= static_cast<float>(win_h)) {
        e.type = InputEvent::Type::PointerLeave;
        return e;
    }
    e.type = InputEvent::Type::MouseMove;
    e.x = std::clamp(static_cast<int>(x) / std::max(1, scale), 0, frame_w - 1);
    e.y = std::clamp(static_cast<int>(y) / std::max(1, scale), 0, frame_h - 1);
    return e;
}

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
