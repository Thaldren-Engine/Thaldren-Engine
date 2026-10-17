#pragma once
// Front ends of the client: an SDL3 window (GPU backend, CPU fallback) with
// keyboard, mouse and sound, and a headless runner for scripted smoke tests that
// writes screenshots.

#include <cstdint>
#include <string>
#include <vector>

#include "client/game_client.h"

#ifdef CLIENT_HAVE_SDL
struct SDL_Window;
union SDL_Event;
#endif

namespace client {

struct ScriptedInput {
    int frame = 0;  // applied before this frame's pass
    InputEvent event;
};

struct FrontOptions {
    int scale = 1;                    // window pixels per frame pixel (1: the screen size is the window's pixel size)
    std::string title = "Battle";     // the window's title (the importer names it after the game)
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

// The pointer and the keyboard focus (docs/RE_MEMORY.md, "Client", "Pointer and focus"). The
// original keeps no pointer position of its own: its pointer is wherever the system pointer is
// (it reads it every frame), it does nothing to the pointer when it loses or gets the focus
// (no warp, no re-centring, no restoring) and it never confines it (full screen does).
//
// A motion of the system pointer to window pixel (x, y) of a win_w x win_h window that shows a
// frame_w x frame_h frame at `scale`: inside the window a MouseMove to that frame pixel (clamped
// to the frame), outside (only possible while the window does not hold the pointer) a
// PointerLeave: the pointer is off the picture.
InputEvent pointer_motion(float x, float y, int scale, int frame_w, int frame_h, int win_w, int win_h);
// Whether the window holds the pointer inside it (standing in for the full screen the original
// runs in): while it has the keyboard focus and is shown, once the system pointer is inside it.
// Holding never moves the pointer: after Alt+Tab and back with the pointer elsewhere it is held
// again only when it comes back into the window.
inline bool pointer_held(bool focused, bool shown, bool pointer_inside) { return focused && shown && pointer_inside; }

#ifdef CLIENT_HAVE_SDL
// Sizes the window to the frame times the scale, centred. A frame as large as the desktop
// takes the whole display instead (borderless, still one window pixel per screen pixel), the
// way the original's chosen display mode fills the screen.
void fit_window_to_frame(SDL_Window* window, int frame_w, int frame_h, int scale);
// Applies pointer_held() to the window, in the menus and in battles alike. Called with every
// event: the pointer goes free when the focus goes (Alt+Tab, the Windows key, Ctrl+Esc,
// Ctrl+Alt+Del, another window activated) or the window is minimised or hidden; with the focus
// back it is held as soon as it is inside the window (at once when it already is, else when it
// comes in or the window is clicked) — never pulled in from outside.
// `ev` null: only the window's present state is applied (after creating the window).
void confine_pointer(SDL_Window* window, const SDL_Event* ev);
// The window's mouse events as the client's: motion through pointer_motion(), the pointer
// leaving the window as PointerLeave, buttons at their frame pixel. False for other events.
bool pointer_event(SDL_Window* window, const SDL_Event& ev, int scale, int frame_w, int frame_h, InputEvent& out);
#endif

// Runs the client until it quits (or for the given frames). Returns a process exit code.
int run_client(GameClient& client, Content& content, const FrontOptions& options);

// Keys whose held-down repeats reach the client (the text fields' editing keys); other
// non-character keys come once per press.
inline bool key_repeats(int k) { return k == keys::kBackspace || k == keys::kDelete || k == keys::kLeft || k == keys::kRight; }

// Parses a script file: one event per line, "<frame> <event> <args...>" with events
// move X Y | down X Y BUTTON | up X Y BUTTON | click X Y BUTTON (down and up) |
// key K | keyup K | type TEXT (the rest of the line typed key by key) |
// wmove X Y W H [SCALE] (the system pointer at window pixel X, Y of a W x H window, possibly
// outside it: pointer_motion) | leave (the system pointer left the window) |
// shift/ctrl/alt modifiers as a trailing "+shift" etc.
// K is a character or one of left right up down pause escape enter f1..f12.
bool parse_script(const std::string& text, std::vector<ScriptedInput>& out, std::string& error);

} // namespace client
