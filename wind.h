#pragma once
#include "grid.h"

// A standing air-velocity field, one vector per cell, in cells/sec.
//
// Neither solver models the air phase, so wind is an external field: the brush
// stamps gusts into it, and the liquid and sand read it as a drag force on
// whatever surfaces are exposed to air. Painted wind stays put — a stamped gust
// blows forever until it is erased or cleared, so you can set up a standing
// current and then work in it. Solids and sand block a gust, so a wall casts a
// wind shadow behind it.

// Speeds below this count as still air, so callers can skip the coupling work.
const float WIND_EPS = 0.05f * CELL_SCALE;

void wind_clear();
// Stamp a gust of air velocity (wvx, wvy) over a disc of radius r cells
// centered on (cx, cy), feathered to nothing at the rim. Cells with no line of
// sight to the center are sheltered and left alone.
void wind_gust(int cx, int cy, int r, float wvx, float wvy);
// Return cell (x, y) to still air — the erase brush's half of a field that
// otherwise never fades.
void wind_erase(int x, int y);
// True while any cell still holds moving air.
bool wind_active();
// Air velocity at the center of cell (x, y); still outside the grid.
float wind_u(int x, int y);
float wind_v(int x, int y);
