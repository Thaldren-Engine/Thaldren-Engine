// The front end's movie player (client/movie.h).

#include "client/movie.h"

#include <algorithm>
#include <cstring>

#include "client/menu.h"

namespace client {

namespace {
constexpr int kScreenW = 640, kScreenH = 480;  // the player's screen (0x47C3A0 places frames on 480 rows)
}

MovieScreen::MovieScreen(std::vector<std::uint8_t> file, bool repeat, std::function<void(FrontEnd&)> after)
    : bytes_(std::move(file)), repeat_(repeat), after_(std::move(after)) {
    std::string error;
    ok_ = movie_.open(bytes_, error);
    screen_.assign(static_cast<std::size_t>(kScreenW) * kScreenH, 0);
    if (ok_ && movie_.audio_present(0)) {
        sound_.rate = movie_.audio_rate(0);
        sound_.channels = movie_.audio_channels(0);
        movie_.decode_track(0, sound_.pcm);
        sound_.name = "movie";
    }
}

bool MovieScreen::typed_character(const InputEvent& e) {
    if (e.type == InputEvent::Type::Text) return true;
    if (e.type != InputEvent::Type::KeyDown) return false;
    if (e.alt && !e.ctrl) return false;  // a system key: its character is a system character
    for (int c : menu_key_codes(e))
        if (c < 0x80) return true;
    return false;  // (a character key whose character comes as a Text event: that event ends it)
}

void MovieScreen::enter(FrontEnd& fe) {
    if (started_) return;
    started_ = true;
    // Every sound stops, the screen is cleared (0x426780 before the player starts).
    fe.stop_sounds();
    if (!ok_) {
        finish(fe);
        return;
    }
    start_ms_ = fe.now_ms();
    if (!sound_.pcm.empty()) fe.play_narration_sample(&sound_);
    show_next();
}

void MovieScreen::show_next() {
    if (!movie_.next_frame()) {
        done_ = true;
        return;
    }
    if (movie_.palette_changed())
        for (int i = 0; i < 256; ++i)
            palette_[static_cast<std::size_t>(i)] = {movie_.palette()[static_cast<std::size_t>(i) * 3], movie_.palette()[static_cast<std::size_t>(i) * 3 + 1],
                                                     movie_.palette()[static_cast<std::size_t>(i) * 3 + 2]};
    movie_.show(screen_.data(), kScreenW, kScreenH, (kScreenH - movie_.shown_height()) >> 1);
    shown_ = movie_.frame_index();
    // The player stops once the last frame is shown (0x47C3A0).
    if (shown_ >= movie_.frames() - 1) done_ = true;
}

void MovieScreen::handle(FrontEnd& fe, const InputEvent& e) {
    // The keystroke's own events all stay here (the original empties its key queue after a
    // movie): a character key ends the movie with its character.
    if (finished_) return;
    if (typed_character(e)) {
        repeat_ = false;
        done_ = true;
        finish(fe);
    }
}

void MovieScreen::frame(FrontEnd& fe, std::uint32_t now_ms) {
    if (finished_ || !ok_) return;
    if (done_) {
        if (repeat_) {
            // The same movie again from its start (a fresh player; the screen is cleared).
            std::string error;
            movie_ = SmackerFile();
            ok_ = movie_.open(bytes_, error);
            std::fill(screen_.begin(), screen_.end(), 0);
            done_ = false;
            shown_ = -1;
            start_ms_ = now_ms;
            fe.stop_narration();
            if (!sound_.pcm.empty()) fe.play_narration_sample(&sound_);
            if (ok_) show_next();
            return;
        }
        finish(fe);
        return;
    }
    // One frame a pass when its time has come (the player's wait).
    const double due = static_cast<double>(shown_ + 1) * movie_.frame_ms();
    if (static_cast<double>(now_ms - start_ms_) >= due) show_next();
}

void MovieScreen::draw(FrontEnd&, render::Image8& out) {
    for (int y = 0; y < out.height && y < kScreenH; ++y)
        std::memcpy(&out.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width)], &screen_[static_cast<std::size_t>(y) * kScreenW],
                    static_cast<std::size_t>(std::min(out.width, kScreenW)));
}

void MovieScreen::finish(FrontEnd& fe) {
    if (finished_) return;
    finished_ = true;
    fe.stop_narration();
    std::function<void(FrontEnd&)> after = std::move(after_);
    after_ = nullptr;
    ok_ = false;
    fe.pop();
    if (after) after(fe);
}

}  // namespace client
