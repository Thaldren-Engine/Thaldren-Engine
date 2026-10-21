#pragma once
// A movie on the front end (docs/RE_MEMORY.md, "Movies"): the player of the original's front
// end. Every sound stops and the screen goes black first; the pointer is not shown; the frames
// come at the movie's own rate (the first one a frame time after the start, each a frame time
// after the last, in whole milliseconds; none while the window lacks the focus), each placed at x 0, y (480 - its shown height) / 2 of the
// 640 x 480 screen (rows between interlaced lines stay black), with the palette the movie
// carries; its sound plays on the narration channel (the effect volume applies, as the wave
// output's volume does to the original's sound buffers). A typed character — any key that
// makes a character, not the arrows, function keys, Shift or Alt combinations — ends it; the
// mouse does nothing. "Repeat" (the intro started with Shift held) starts it again after its
// last frame until a typed character. Presentation only.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "client/content.h"
#include "client/front_end.h"
#include "client/smacker.h"

namespace client {

class MovieScreen : public Screen {
public:
    // `file` is the movie's bytes; `after` runs once it has ended (after the screen closed).
    MovieScreen(std::vector<std::uint8_t> file, bool repeat, std::function<void(FrontEnd&)> after = {});
    void enter(FrontEnd& fe) override;
    void handle(FrontEnd& fe, const InputEvent& e) override;
    void frame(FrontEnd& fe, std::uint32_t now_ms) override;
    void draw(FrontEnd& fe, render::Image8& out) override;
    const render::Palette* palette() const override { return &palette_; }
    bool hides_pointer() const override { return true; }
    // The frame shown last (-1 before the first).
    int shown_frame() const { return shown_; }

    // Whether a key event makes a typed character (the window's character message).
    static bool typed_character(const InputEvent& e);

private:
    void finish(FrontEnd& fe);
    void show_next();
    std::uint32_t interval() const;

    std::vector<std::uint8_t> bytes_;
    SmackerFile movie_;
    bool ok_ = false, repeat_ = false, started_ = false, done_ = false, finished_ = false;
    std::uint32_t next_due_ = 0;  // when the next frame is due (the main loop's clock)
    int shown_ = -1;
    std::vector<std::uint8_t> screen_;  // 640 x 480, kept from frame to frame
    render::Palette palette_{};
    Sample sound_;
    std::function<void(FrontEnd&)> after_;
};

}  // namespace client
