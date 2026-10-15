#pragma once
// Front ends of the client: an SDL3 window (GPU backend, CPU fallback) with
// keyboard, mouse and sound, and a headless runner for scripted smoke tests that
// writes screenshots.

#include <cstdint>
#include <string>
#include <vector>

#include "client/game_client.h"

namespace client {

struct ScriptedInput {
    int frame = 0;  // applied before this frame's pass
    InputEvent event;
};

struct FrontOptions {
    int scale = 2;                    // window pixels per frame pixel
    bool headless = false;            // no window, no sound: a fixed 30 frames per second clock
    int frames = 0;                   // headless: frames to run (window: 0 = until closed)
    std::vector<ScriptedInput> script;
    std::vector<int> screenshot_frames;  // frames to save (headless and window)
    std::string screenshot_prefix;       // <prefix>NNNN.bmp
    bool gpu = true;                     // window: GPU backend (else CPU frames through an SDL texture)
    bool gpu_check = false;              // headless: also draw screenshots on the GPU and compare
    bool sound = true;
    bool draw_pointer = true;            // false: frames without the pointer (comparisons with the original's screen surface)
    bool realtime = false;               // headless: pace the frames to the wall clock (network games between processes)
};

// Runs the client until it quits (or for the given frames). Returns a process exit code.
int run_client(GameClient& client, Content& content, const FrontOptions& options);

// Parses a script file: one event per line, "<frame> <event> <args...>" with events
// move X Y | down X Y BUTTON | up X Y BUTTON | click X Y BUTTON (down and up) |
// key K | keyup K | shift/ctrl/alt modifiers as a trailing "+shift" etc.
// K is a character or one of left right up down pause escape enter f1..f12.
bool parse_script(const std::string& text, std::vector<ScriptedInput>& out, std::string& error);

} // namespace client
