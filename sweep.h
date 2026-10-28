#pragma once
#include <SDL.h>
#include "fluid.h"

// The CLEAR wipe. Rather than the scene blinking out, a bright bar sweeps across
// the simulation from left to right and clears every column it passes, throwing
// off sparks in the colour of whatever it is scouring away. Everything behind
// the bar is kept empty while it travels, so material that flows or slides back
// into the cleared side is caught too, and the scene is wiped outright once the
// bar leaves the far edge — the end state is exactly input_clear_all's.

// Begin a wipe. Ignored while one is already running, so mashing CLEAR doesn't
// restart the bar from the left edge.
void sweep_start();
// Advance the bar one frame and clear what it has passed. Call once per frame,
// before the solvers, so nothing steps on cells that are about to vanish.
void sweep_update(FluidState& fluid);
// Draw the bar and its sparks, in sim-local pixels (the sim viewport).
void sweep_draw(SDL_Renderer* renderer);
bool sweep_active();
