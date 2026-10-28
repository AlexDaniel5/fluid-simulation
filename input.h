#pragma once
#include <SDL.h>
#include "grid.h"
#include "fluid.h"

enum DrawMode { MODE_SOLID = 0, MODE_SAND, MODE_FLUID, MODE_WIND, MODE_LAVA, MODE_ICE, MODE_COUNT };

// Brush radius in cells (0 = single cell). Adjusted with [ and ]. Scaled so the
// brush covers the same patch of screen whatever the cell size is.
const int BRUSH_MIN = 0;
const int BRUSH_MAX = (int)(5 * CELL_SCALE);
const int BRUSH_DEFAULT = (int)(1 * CELL_SCALE);

struct InputState {
    bool isDrawing;
    bool showGrid;   // debug grid-line overlay, toggled with G
    // Two independent ways to be erasing, because they suit different jobs.
    // eraseHeld is E held down — momentary, for a quick rub-out mid-stroke
    // without losing the tool you were using. eraseSticky is the menu's ERASE
    // button — latched, so erasing behaves like picking a tool and stays on
    // until another tool is picked. Read them through input_erasing().
    bool eraseHeld;
    bool eraseSticky;
    DrawMode mode;
    int brushRadius;
    int prevMouseX;
    int prevMouseY;
    // Cursor position in *logical* (whole-window) coordinates, tracked on every
    // motion event whether or not a button is down, so the menu can highlight
    // the button under the pointer.
    int mouseX;
    int mouseY;
    // Unit vector the wind brush blows along. Aimed by stroking the mouse and
    // kept afterwards, so holding still leaves a steady jet rather than dead
    // air. Starts pointing right so a plain click already does something.
    float windDirX;
    float windDirY;
};

// True while drawing should rub cells out rather than lay material down, by
// either route. Everything that paints asks this rather than reading the two
// flags, so the momentary and latched cases can never disagree.
inline bool input_erasing(const InputState& s) { return s.eraseHeld || s.eraseSticky; }

// Wipe the scene: every particle, every cell, the heat behind the lava and the
// standing wind, all at once. The C key and the menu's CLEAR button don't call
// this directly any more — they start the sweep (sweep.h), which finishes with
// it, so the animated wipe always ends in exactly this state.
void input_clear_all(FluidState& fluid);

void input_init(InputState& state);
// Returns false when the user requests quit.
bool input_handle_event(InputState& state, const SDL_Event& event, FluidState& fluid);
// Per-frame, position-based actions. While E is held this continuously erases
// the cells under the cursor, so material that flows into the mouse is removed
// even without clicking or moving.
void input_update(InputState& state, FluidState& fluid);
