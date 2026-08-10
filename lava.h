#pragma once
#include "fluid.h"

// Viscous cellular-automaton lava — the first material here that carries state
// which changes on its own. Every molten cell holds a heat in [0,1]: it pours
// and self-levels while hot, stiffens as it dims, and freezes into ROCK when the
// heat runs out. Heat escapes only through faces touching air, so a thick flow
// crusts from the outside in and stays molten at the core.
//
// Water occupancy is read straight out of FluidState (the solver already
// classifies it every step) because lava never moves water — it quenches
// against it. Call once per frame, before sand_update.

void lava_update(FluidState& fluid, int frame, float dt);
// Stamp a fresh, fully molten cell. Refuses walls, and leaves lava that is
// already there alone so a held brush doesn't keep reheating the same puddle.
void lava_add(int x, int y);
// The erase brush's half: forget the heat behind a cell that was just cleared.
void lava_erase(int x, int y);
void lava_clear();
// True while any lava is still molten, so callers can skip the coupling work.
bool lava_active();
// Heat at cell (x, y) in [0,1]; zero outside the grid.
float lava_heat(int x, int y);
