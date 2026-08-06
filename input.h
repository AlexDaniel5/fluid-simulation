#pragma once
#include <SDL.h>
#include "grid.h"
#include "fluid.h"

enum DrawMode { MODE_SOLID = 0, MODE_SAND, MODE_FLUID, MODE_WIND, MODE_LAVA, MODE_COUNT };

// Brush radius in cells (0 = single cell). Adjusted with [ and ]. Scaled so the
// brush covers the same patch of screen whatever the cell size is.
const int BRUSH_MIN = 0;
const int BRUSH_MAX = (int)(5 * CELL_SCALE);
const int BRUSH_DEFAULT = (int)(1 * CELL_SCALE);

struct InputState {
    bool isDrawing;
    bool showGrid;   // debug grid-line overlay, toggled with G
    bool eraseHeld;  // E held down: drawing rubs out cells instead of the active material
    DrawMode mode;
    int brushRadius;
    int prevMouseX;
    int prevMouseY;
    // Unit vector the wind brush blows along. Aimed by stroking the mouse and
    // kept afterwards, so holding still leaves a steady jet rather than dead
    // air. Starts pointing right so a plain click already does something.
    float windDirX;
    float windDirY;
};

void input_init(InputState& state);
// Returns false when the user requests quit.
bool input_handle_event(InputState& state, const SDL_Event& event, FluidState& fluid);
// Per-frame, position-based actions. While E is held this continuously erases
// the cells under the cursor, so material that flows into the mouse is removed
// even without clicking or moving.
void input_update(InputState& state, FluidState& fluid);
