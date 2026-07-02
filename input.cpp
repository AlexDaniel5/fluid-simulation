#include "input.h"
#include <cstring>

void input_init(InputState& state) {
    state.isDrawing   = false;
    state.showGrid    = false;
    state.eraseHeld   = false;
    state.mode        = MODE_SOLID;
    state.brushRadius = 1;
    state.prevMouseX  = 0;
    state.prevMouseY  = 0;
}

// Stamp the current material over a disc of cells centered on the pixel
// (px, py). dx/dy are the mouse delta this event, used as a fling impulse in
// fluid mode. A radius of 0 paints just the single cell under the cursor.
static void paint_at(InputState& state, FluidState& fluid, int px, int py,
                     float dx, float dy) {
    int ccx = px / CELL_SIZE;
    int ccy = py / CELL_SIZE;
    int r = state.brushRadius;
    for (int cy = ccy - r; cy <= ccy + r; ++cy) {
        for (int cx = ccx - r; cx <= ccx + r; ++cx) {
            if (cx < 0 || cx >= GRID_WIDTH || cy < 0 || cy >= GRID_HEIGHT) continue;
            int ox = cx - ccx, oy = cy - ccy;
            if (ox * ox + oy * oy > r * r) continue;  // disc, not square

            if (state.eraseHeld) {
                grid[cy][cx] = EMPTY;
                fluid_remove(fluid, cx, cy);
            } else if (state.mode == MODE_SOLID) {
                fluid_displace(fluid, cx, cy);
                grid[cy][cx] = SOLID;
            } else if (state.mode == MODE_SAND) {
                fluid_displace(fluid, cx, cy);
                grid[cy][cx] = SAND;
            } else {
                fluid_add_density(fluid, cx, cy, 0.5f);
                // Particle velocities are in cells/second; turn the per-event
                // mouse delta into a fling impulse.
                fluid_add_velocity(fluid, cx, cy,
                                   dx / CELL_SIZE * 4.0f, dy / CELL_SIZE * 4.0f);
            }
        }
    }
}

bool input_handle_event(InputState& state, const SDL_Event& event, FluidState& fluid) {
    if (event.type == SDL_QUIT) return false;

    if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
        state.isDrawing  = true;
        state.prevMouseX = event.button.x;
        state.prevMouseY = event.button.y;
        // Paint on press so a single click (no drag) still draws.
        paint_at(state, fluid, event.button.x, event.button.y, 0.0f, 0.0f);
    } else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
        state.isDrawing = false;
    } else if (event.type == SDL_MOUSEMOTION && state.isDrawing) {
        float dx = (float)(event.motion.x - state.prevMouseX);
        float dy = (float)(event.motion.y - state.prevMouseY);
        paint_at(state, fluid, event.motion.x, event.motion.y, dx, dy);
        state.prevMouseX = event.motion.x;
        state.prevMouseY = event.motion.y;
    }

    if (event.type == SDL_KEYUP && event.key.keysym.sym == SDLK_e) {
        state.eraseHeld = false;
    }

    if (event.type == SDL_KEYDOWN) {
        switch (event.key.keysym.sym) {
            case SDLK_SPACE:
                state.mode = static_cast<DrawMode>((state.mode + 1) % MODE_COUNT);
                break;
            case SDLK_e:
                state.eraseHeld = true;  // momentary: erase while held
                break;
            case SDLK_g:
                state.showGrid = !state.showGrid;
                break;
            case SDLK_LEFTBRACKET:
                if (state.brushRadius > BRUSH_MIN) state.brushRadius--;
                break;
            case SDLK_RIGHTBRACKET:
                if (state.brushRadius < BRUSH_MAX) state.brushRadius++;
                break;
            case SDLK_r:
                fluid_reset(fluid);
                break;
            case SDLK_c:
                fluid_reset(fluid);
                memset(grid, 0, sizeof(int) * GRID_HEIGHT * GRID_WIDTH);
                break;
        }
    }

    return true;
}

void input_update(InputState& state, FluidState& fluid) {
    if (!state.eraseHeld) return;
    // Erase a disc at wherever the cursor currently is, every frame. The
    // eraseHeld branch in paint_at does the actual removal.
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    paint_at(state, fluid, mx, my, 0.0f, 0.0f);
}
