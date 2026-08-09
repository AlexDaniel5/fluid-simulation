#include "input.h"
#include "wind.h"
#include "lava.h"
#include "renderer.h"  // window -> simulation pixel mapping
#include <cstring>
#include <cmath>

// Wind brush feel parameters.
static const float WIND_SPEED   = 30.0f * CELL_SCALE;      // air the brush blows
static const int   WIND_SPREAD  = (int)(1 * CELL_SCALE);   // gust radius past the brush
static const float WIND_AIM_MIN = 2.0f;   // px of mouse travel needed to re-aim
static const float WIND_AIM_LERP = 0.35f; // how fast the jet swings to a new aim

void input_init(InputState& state) {
    state.isDrawing   = false;
    state.showGrid    = false;
    state.eraseHeld   = false;
    state.mode        = MODE_SOLID;
    state.brushRadius = BRUSH_DEFAULT;
    state.prevMouseX  = 0;
    state.prevMouseY  = 0;
    state.windDirX    = 1.0f;
    state.windDirY    = 0.0f;
}

// Swing the wind jet toward the current stroke direction. Ignores the jitter of
// a near-stationary mouse so the aim survives holding the brush in place.
static void aim_wind(InputState& state, float dx, float dy) {
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < WIND_AIM_MIN) return;
    float nx = state.windDirX + (dx / len - state.windDirX) * WIND_AIM_LERP;
    float ny = state.windDirY + (dy / len - state.windDirY) * WIND_AIM_LERP;
    float n = std::sqrt(nx * nx + ny * ny);
    if (n < 1e-4f) return;  // aim reversed through zero; keep the old heading
    state.windDirX = nx / n;
    state.windDirY = ny / n;
}

// Stamp the current material over a disc of cells centered on the pixel
// (px, py). dx/dy are the mouse delta this event, used as a fling impulse in
// fluid mode. A radius of 0 paints just the single cell under the cursor.
static void paint_at(InputState& state, FluidState& fluid, int px, int py,
                     float dx, float dy) {
    // Wind stamps no cells; its gust is applied per frame from input_update so
    // that a held brush keeps blowing. (Erase still works in wind mode.)
    if (state.mode == MODE_WIND && !state.eraseHeld) return;

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
                wind_erase(cx, cy);  // wind never fades on its own
                lava_erase(cx, cy);  // drop the heat behind the cleared cell
            } else if (state.mode == MODE_SOLID) {
                fluid_displace(fluid, cx, cy);
                grid[cy][cx] = SOLID;
            } else if (state.mode == MODE_SAND) {
                // A wall is not somewhere sand can go: brushing over one
                // leaves it standing instead of dissolving it into grains.
                if (grid[cy][cx] == SOLID || grid[cy][cx] == ROCK) continue;
                fluid_displace(fluid, cx, cy);
                grid[cy][cx] = SAND;
            } else if (state.mode == MODE_LAVA) {
                // Same rule as sand: pour around walls and set rock, not over
                // them. Sand underneath is left alone — the flow melts its way
                // in from lava_update instead, and pays heat for the privilege.
                if (grid[cy][cx] == SOLID || grid[cy][cx] == ROCK) continue;
                fluid_displace(fluid, cx, cy);
                lava_add(cx, cy);
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
        // Event coordinates are already simulation pixels — SDL scales them out
        // of window space itself, so they only need the bounds check.
        const int mx = event.button.x, my = event.button.y;
        if (renderer_sim_contains(mx, my)) {
            state.isDrawing  = true;
            state.prevMouseX = mx;
            state.prevMouseY = my;
            // Paint on press so a single click (no drag) still draws.
            paint_at(state, fluid, mx, my, 0.0f, 0.0f);
        }
    } else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
        state.isDrawing = false;
    } else if (event.type == SDL_MOUSEMOTION && state.isDrawing) {
        const int mx = event.motion.x, my = event.motion.y;
        if (renderer_sim_contains(mx, my)) {
            // Deltas are taken in simulation pixels too, so a fling imparts the
            // same impulse whatever the window is scaled to.
            float dx = (float)(mx - state.prevMouseX);
            float dy = (float)(my - state.prevMouseY);
            if (state.mode == MODE_WIND) aim_wind(state, dx, dy);
            paint_at(state, fluid, mx, my, dx, dy);
            state.prevMouseX = mx;
            state.prevMouseY = my;
        }
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
            case SDLK_w:
                wind_clear();  // still the air everywhere, leaving the scene
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
                wind_clear();
                lava_clear();
                memset(grid, 0, sizeof(int) * GRID_HEIGHT * GRID_WIDTH);
                break;
        }
    }

    return true;
}

void input_update(InputState& state, FluidState& fluid) {
    if (!state.eraseHeld && !(state.isDrawing && state.mode == MODE_WIND)) return;

    int wx, wy, mx, my;
    SDL_GetMouseState(&wx, &wy);
    if (!renderer_window_to_sim(wx, wy, &mx, &my)) return;  // cursor off the sim

    if (state.eraseHeld) {
        // Erase a disc wherever the cursor currently is, every frame. The
        // eraseHeld branch in paint_at does the actual removal.
        paint_at(state, fluid, mx, my, 0.0f, 0.0f);
        return;
    }

    // Blow a gust along the aimed heading, every frame the button is held.
    wind_gust(mx / CELL_SIZE, my / CELL_SIZE, state.brushRadius + WIND_SPREAD,
              state.windDirX * WIND_SPEED, state.windDirY * WIND_SPEED);
}
