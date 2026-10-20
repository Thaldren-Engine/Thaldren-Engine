// The screenshot key's file: the next free number in a folder (docs/RE_MEMORY.md, "Client",
// "Screenshots"), and the film (debug switch, Ctrl+F10).

#include "client/front.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "render/draw_list.h"
#include "render/presenter.h"

namespace client {

std::string save_numbered_picture(const std::string& dir, const std::string& prefix, const render::Image8& frame,
                                  const render::Palette& palette) {
    // The folder is made when it is missing; the number is one more than the largest found after
    // the prefix in the folder's "<prefix>*.pcx" names (the C runtime's atoi of what follows the
    // prefix; none: 1), written with four digits.
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!dir.empty()) fs::create_directories(fs::path(dir), ec);
    int highest = 0;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string want = lower(prefix);
    for (fs::directory_iterator it(dir.empty() ? fs::path(".") : fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = lower(it->path().filename().string());
        if (name.size() < want.size() + 4 || name.compare(0, want.size(), want) != 0 || name.compare(name.size() - 4, 4, ".pcx") != 0)
            continue;
        const int n = std::atoi(name.c_str() + want.size());
        if (n > highest) highest = n;
    }
    char num[16];
    std::snprintf(num, sizeof num, "%04i", highest + 1);
    std::string path = dir;
    if (!path.empty() && path.back() != '\\' && path.back() != '/') path += '/';
    path += prefix + num + ".pcx";
    return render::write_pcx(path, frame, palette) ? path : std::string();
}

// ---------------------------------------------------------------- display gamma
render::Palette gamma_palette(const render::Palette& palette, float factor) {
    // (each channel: FILD of the byte times the float factor, above 255 -> 255, then the
    // truncating ftol; the product is exact in a double)
    render::Palette out = palette;
    auto scale = [factor](std::uint8_t c) -> std::uint8_t {
        const double v = static_cast<double>(c) * static_cast<double>(factor);
        return static_cast<std::uint8_t>(static_cast<long long>(v > 255.0 ? 255.0 : v));
    };
    for (render::Rgb& c : out) {
        c.r = scale(c.r);
        c.g = scale(c.g);
        c.b = scale(c.b);
    }
    return out;
}

float gamma_from_setting(int n) {
    // 0.5 - n x (float)-0.041666668 (x87 extended; exact here in a double), stored as a float.
    return static_cast<float>(0.5 - static_cast<double>(n) * static_cast<double>(-0.041666668f));
}

float gamma_from_command(int n) { return static_cast<float>(static_cast<double>(n) * static_cast<double>(0.1f)); }

// ---------------------------------------------------------------- the film
void GameClient::toggle_film() {
    // Ctrl+F10 with the debug switch (0x495E90 case 0xD7): filming stops; else a new film starts:
    // its number is one more than the largest atoi of the text after "MOVIE" among the entries
    // "<image folder>\MOVIE*", its folder "<image folder>\MOVIE%03i" is made, and a frame drawn
    // at once without the console is its first picture; the next one is due at this game tick.
    if (!opt_.debug_commands) return;
    const int was = film_number_;
    film_number_ = 0;
    film_due_ = false;
    if (was != 0) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    int highest = 0;
    for (fs::directory_iterator it(fs::path(opt_.image_dir.empty() ? std::string(".") : opt_.image_dir), ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.size() < 5) continue;
        bool movie = true;
        for (int k = 0; k < 5; ++k) movie = movie && std::toupper(static_cast<unsigned char>(name[static_cast<std::size_t>(k)])) == "MOVIE"[k];
        if (!movie) continue;
        const int n = std::atoi(name.c_str() + 5);
        if (n > highest) highest = n;
    }
    film_number_ = highest + 1;
    char folder[32];
    std::snprintf(folder, sizeof folder, "MOVIE%03i", film_number_);
    film_folder_ = opt_.image_dir + "\\" + folder;
    fs::create_directories(fs::path(film_folder_), ec);
    no_console_ = true;
    const render::Image8 first = render::execute(draw(), c_.blend);
    no_console_ = false;
    const std::string path = save_numbered_picture(film_folder_, "FRAM", first, c_.palette);
    std::printf("film %s\n", path.empty() ? "(not written)" : path.c_str());
    film_next_tick_ = b_.game_tick();
}

std::string GameClient::make_poster(int width, int height, bool all) {
    // 0x417600: the size (default 3200 x 2400; "all": the scroll extent) is at least the view's
    // size in whole cells and at most the scroll extent; its corner = the view's centre less half
    // the size, at least 0 and at most the extent less the size. 0x495A30 then draws it in square
    // tiles of the view's height in cells x 16 - 1 pixels, each a whole battle frame (the frame's
    // side effects with it) at that scroll, the paused sign, the clock and the console off,
    // copies the view's part into the picture and writes it to <image folder>\screenshots as
    // BIGSHOTnnnn.pcx; then the view goes back and the step clock restarts.
    const int sw = scroll_width(), sh = scroll_height();
    const int vwc = (opt_.width - 128) / 16, vhc = (opt_.height - 64) / 16;
    int w = all ? sw : width, h = all ? sh : height;
    if (w <= vwc * 16) w = vwc * 16;
    if (sw <= w) w = sw;
    if (h <= vhc * 16) h = vhc * 16;
    if (sh <= h) h = sh;
    int x = scroll_x_ - w / 2 + vwc * 8;
    int y = scroll_y_ - h / 2 + vhc * 8;
    if (x < 1) x = 0;
    if (sw - w <= x) x = sw - w;
    if (y < 1) y = 0;
    if (sh - h <= y) y = sh - h;
    if (w <= 0 || h <= 0) return std::string();
    const int tile = vhc * 16 - 1;
    if (tile <= 0) return std::string();

    const int keep_x = scroll_x_, keep_y = scroll_y_;
    const bool keep_paused = budget_.paused, keep_clock = opt_.clock, keep_hidden = pointer_hidden_;
    budget_.paused = false;
    opt_.clock = false;
    no_console_ = true;
    pointer_hidden_ = true;
    render::Image8 poster(w, h, 0);
    const render::ClipRect view = view_rect();
    for (int ty = 0; ty < h; ty += tile)
        for (int tx = 0; tx < w; tx += tile) {
            set_scroll(x + tx, y + ty);
            const render::Image8 frame = render::execute(draw(), c_.blend);
            const int ex = std::min(tx + tile, w), ey = std::min(ty + tile, h);
            for (int py = ty; py < ey; ++py) {
                const int fy = y + py - scroll_y_ + view.y0;
                if (fy < view.y0 || fy > view.y1 || fy >= frame.height) continue;
                for (int px = tx; px < ex; ++px) {
                    const int fx = x + px - scroll_x_ + view.x0;
                    if (fx < view.x0 || fx > view.x1 || fx >= frame.width) continue;
                    poster.row(py)[px] = frame.pixels[static_cast<std::size_t>(fy) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(fx)];
                }
            }
        }
    budget_.paused = keep_paused;
    opt_.clock = keep_clock;
    no_console_ = false;
    pointer_hidden_ = keep_hidden;
    set_scroll(keep_x, keep_y);
    budget_restart_ = true;
    const std::string dir = (opt_.image_dir.empty() ? std::string(".") : opt_.image_dir) + "\\screenshots";
    return save_numbered_picture(dir, "BIGSHOT", poster, c_.palette);
}

void GameClient::film_written(std::uint32_t now_ms) {
    // (the battle loop then restarts the step budget's clock: the writing's time is not caught up)
    film_due_ = false;
    budget_.last_scaled_tick = sim::StepBudget::scale_ms(now_ms);
}

}  // namespace client
