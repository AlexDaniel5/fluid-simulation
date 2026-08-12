#include "ice.h"
#include <cstring>
#include <algorithm>

// ---- tuning (feel parameters) ----
// Speeds and accelerations carry CELL_SCALE so they stay fixed on screen when
// the cell size changes, matching the convention in sand.cpp and lava.cpp.
static const float ICE_GRAVITY  = 25.0f * CELL_SCALE;  // matches sand, lava and the solver
static const float TERMINAL_ICE = 30.0f * CELL_SCALE;  // a shade under sand's dry 40:
                                                       // a block falls a little heavier
                                                       // than loose grit, but not so fast
                                                       // that it reads as weightless

// Per-cell fall state, parallel to grid[][]. Carried along when a block moves
// and zeroed at the source, the same way sand and lava carry theirs.
static float fallVel[GRID_HEIGHT][GRID_WIDTH];
static float fallFrac[GRID_HEIGHT][GRID_WIDTH];
static unsigned char movedFlag[GRID_HEIGHT][GRID_WIDTH];

static inline int cidx(int x, int y) { return x + GRID_WIDTH * y; }
static inline bool in_grid(int x, int y) {
    return x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT;
}

// Water occupancy, straight from the solver's own per-step classification —
// the same read lava makes, and one frame stale for the same reason (ice runs
// ahead of fluid_step).
static inline bool has_water(const FluidState& f, int x, int y) {
    return in_grid(x, y) && f.cell[cidx(x, y)] == CELL_FLUID;
}

// Somewhere a block can fall into. Water counts as a wall, and that one clause
// is the whole floating model: ice comes to rest on the surface of a pool, and
// starts falling again by itself once the cell reads empty.
static inline bool can_take(const FluidState& f, int x, int y) {
    return in_grid(x, y) && grid[y][x] == EMPTY && !has_water(f, x, y);
}

static void move_ice(int x, int y, int nx, int ny) {
    fallVel[ny][nx]  = fallVel[y][x];
    fallFrac[ny][nx] = fallFrac[y][x];
    fallVel[y][x]  = 0.0f;
    fallFrac[y][x] = 0.0f;
    grid[ny][nx] = ICE;
    grid[y][x]   = EMPTY;
    movedFlag[ny][nx] = 1;
}

void ice_add(int x, int y) {
    if (!in_grid(x, y)) return;
    if (grid[y][x] != EMPTY) return;
    grid[y][x]     = ICE;
    fallVel[y][x]  = 0.0f;
    fallFrac[y][x] = 0.0f;
}

void ice_update(FluidState& f, int frame, float dt) {
    memset(movedFlag, 0, sizeof(movedFlag));

    // Bottom-up so a falling column stays ordered, with the horizontal sweep
    // alternating so nothing picks up a directional bias — the same sweep sand
    // and lava use. The moved flag keeps multi-cell falls from being processed
    // twice in one frame.
    bool ltr = frame & 1;
    for (int y = GRID_HEIGHT - 1; y >= 0; --y) {
        for (int xx = 0; xx < GRID_WIDTH; ++xx) {
            int x = ltr ? xx : GRID_WIDTH - 1 - xx;
            if (grid[y][x] != ICE || movedFlag[y][x]) continue;

            // Integrate gravity against the terminal speed, then fall
            // floor(travel) cells carrying the fractional remainder so a slow
            // block still creeps, stepping one cell at a time so nothing
            // teleports through an obstacle.
            float v = std::min(fallVel[y][x] + ICE_GRAVITY * dt, TERMINAL_ICE);
            fallVel[y][x] = v;

            float travel = v * dt + fallFrac[y][x];
            int steps = (int)travel;
            fallFrac[y][x] = travel - steps;

            int cy = y;
            while (steps > 0 && can_take(f, x, cy + 1)) {
                move_ice(x, cy, x, cy + 1);
                ++cy;
                --steps;
            }
            if (can_take(f, x, cy + 1)) continue;  // still falling freely

            // Supported, so it stops — and that is the entire difference from
            // sand. No diagonal slide, no slump, no wind roll and no drag from
            // the current: a block holds its position and its shape, which is
            // what lets ice stack into a vertical column where sand can only
            // make a 45-degree pile.
            fallVel[cy][x]  = 0.0f;
            fallFrac[cy][x] = 0.0f;
        }
    }
}
