#pragma once
#include <SDL.h>
#include "grid.h"
#include "fluid.h"

void renderer_init(SDL_Renderer* renderer);
// Draws the simulation only, into its own viewport, and leaves the viewport as
// it found it so the menu can draw afterwards in plain logical coordinates.
void renderer_draw(SDL_Renderer* renderer, const FluidState& fluid, bool showGrid);
void renderer_destroy();

// Text, for the tool menu. The renderer owns the font because it already owns
// every other SDL resource with a lifetime. Draws with (x, y) as the top-left
// corner; strings are rasterised once and cached, so passing a handful of fixed
// labels every frame costs a blit each. Nothing is drawn if the font failed to
// load, which is the only way this can fail.
void renderer_text_draw(SDL_Renderer* renderer, int x, int y, const char* s,
                        SDL_Color c);
void renderer_text_size(const char* s, int* w, int* h);

// There are three coordinate spaces here, and mixing them up is the classic bug
// in this file:
//
//   window  — raw pixels of the actual window, whatever size the user dragged it
//             to. What SDL_GetMouseState returns.
//   logical — the fixed WIDTH x HEIGHT frame the whole UI is authored in. SDL
//             scales it to fill the window, letterboxed to keep the aspect.
//             What arrives on a mouse event, because SDL undoes the scale itself.
//   sim     — logical minus the tool menu strip, so (0,0) is the simulation's
//             top-left, not the window's. The only space that names a cell.
//
// Map a *logical* pixel to a simulation pixel. This is the one place PANEL_W is
// subtracted; applying it anywhere else too drags the drawing away from the
// cursor by the width of the menu. Returns false for logical pixels that are not
// simulation — the menu strip — which name no cell at all.
bool renderer_logical_to_sim(int lx, int ly, int* sx, int* sy);

// The same, starting from *raw* window coordinates: undoes the window scale
// first, then defers to the above. Use this for SDL_GetMouseState only. Putting
// a mouse event through here scales it twice, because events are already
// logical. Returns false in the letterbox bars as well as over the menu.
bool renderer_window_to_sim(int wx, int wy, int* sx, int* sy);
