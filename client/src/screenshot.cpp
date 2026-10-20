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

void GameClient::film_written(std::uint32_t now_ms) {
    // (the battle loop then restarts the step budget's clock: the writing's time is not caught up)
    film_due_ = false;
    budget_.last_scaled_tick = sim::StepBudget::scale_ms(now_ms);
}

}  // namespace client
