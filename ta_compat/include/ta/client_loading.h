#pragma once
// The battle loading screen as the original draws it (0x00497F40; docs/RE_MEMORY.md,
// "Client", "Loading screen (exact)").

#include "client/front_end.h"
#include "render/image.h"

namespace ta {

class MenuArt;

// One redraw of the loading screen onto `out` (the menus' 640 x 480 frame), with the state's
// rows, map line and (network games) players; updates the state's highlights as the original
// does at each redraw, then (network games) this machine's players' progress (the average of
// the six rows, shown from the next redraw).
void draw_loading_screen(MenuArt& art, render::Image8& out, client::LoadingState& state);

}  // namespace ta
