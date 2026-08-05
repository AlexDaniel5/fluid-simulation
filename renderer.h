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
bool renderer_window_to_sim(int wx, int wy, int* sx, int* sy);
