#pragma once
#include <SDL.h>
#include "grid.h"
#include "fluid.h"

void renderer_init(SDL_Renderer* renderer);
void renderer_draw(SDL_Renderer* renderer, const FluidState& fluid, bool showGrid);
void renderer_destroy();

// Map a window pixel to a simulation pixel. The sim is drawn at a fixed logical
// size and scaled to fill whatever the window currently is (fullscreen included,
// letterboxed to keep the aspect), so anything reading the mouse has to undo
// that scale before it can name a cell. Returns false for window pixels outside
// the sim — the letterbox bars — which name no cell at all.
//
// This is for *raw* window coordinates, which is what SDL_GetMouseState returns.
// Coordinates that arrive on a mouse event are not raw: SDL rescales those into
// logical space itself whenever the renderer has a logical size, so they are
// already simulation pixels and need renderer_sim_contains instead. Putting an
// event through here scales it twice, which drags the drawing away from the
// cursor by however much the window differs from WIDTH x HEIGHT.
bool renderer_window_to_sim(int wx, int wy, int* sx, int* sy);

// The bounds half of the mapping on its own, for coordinates that are already
// in simulation space.
bool renderer_sim_contains(int sx, int sy);
