#pragma once
#include "fluid.h"

// Cellular-automaton sand coupled to the liquid solver. Grains carry a fall
// velocity (gravity-accelerated, terminal-capped, much lower underwater) and
// move multiple cells per frame. Reads water occupancy and velocity from
// FluidState; water it moves into is displaced (vented sideways around the
// grain, or swapped into the vacated cell when boxed in), never destroyed,
// and picks up part of the grain's fall momentum so impacts splash.
// Call once per frame.
void sand_update(FluidState& fluid, int frame, float dt);
