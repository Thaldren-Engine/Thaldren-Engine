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
    // The library's clock (movie_harness --wait): the first frame comes one frame time after
    // the open, each later one a frame time after the one before was shown.
    next_due_ = fe.now_ms() + interval();
}

std::uint32_t MovieScreen::interval() const {
    // Whole milliseconds, rounded up (33.33 ms frames come every 34 ms).
    const double ms = movie_.frame_ms();
    const auto whole = static_cast<std::uint32_t>(ms);
    return static_cast<double>(whole) < ms ? whole + 1 : std::max<std::uint32_t>(whole, 1);
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
            fe.stop_narration();
            next_due_ = now_ms + interval();
            return;
        }
        finish(fe);
        return;
    }
    // 0x47C3A0 shows (and advances) a frame only while the window has the focus; the library's
    // clock runs on, so the frame due comes at once when the focus is back, the next one a frame
    // time later.
    if (!fe.window_focused()) return;
    // One frame a pass when its time has come (the player's wait). (The original's loop checks
    // the clock continuously; a pass's own lateness is not carried into the next frame unless it
    // is a whole frame time or more, as after a stall.)
    if (static_cast<std::int32_t>(now_ms - next_due_) < 0) return;
    const std::uint32_t late = now_ms - next_due_;
    next_due_ = (late >= interval() ? now_ms : next_due_) + interval();
    // The movie's sound starts with its first frame (SmackDoFrame of frame 0).
    if (shown_ < 0 && !sound_.pcm.empty()) fe.play_narration_sample(&sound_);
    show_next();
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
