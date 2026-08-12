#pragma once
#include <SDL.h>
#include "input.h"
#include "fluid.h"

// The tool menu: the PANEL_W-wide strip down the left edge of the window.
//
// Every control here already existed as a key binding; what the menu adds is
// somewhere to see them. The active tool is the thing a user most needs to know
// and previously the only readout was the window title, which is easy to miss.
//
// Drawing and hit-testing both read the same button table (ui.cpp), so a button
// cannot move on screen without its clickable area moving with it.
//
// Coordinates here are *logical* — the whole-window space that mouse events
// arrive in, where the menu is x in [0, PANEL_W) and the simulation is
// x in [PANEL_W, WIDTH). Nothing in this file works in sim-local pixels.

// Draw the menu. Call after renderer_draw, which restores the base viewport.
void ui_draw(SDL_Renderer* renderer, const InputState& state);

// Offer a left-click at logical (lx, ly) to the menu. Returns true if the menu
// took it, in which case the caller must not also paint — otherwise clicking a
// button stamps material into the cell hidden behind it.
bool ui_click(InputState& state, FluidState& fluid, int lx, int ly);
