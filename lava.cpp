#include "lava.h"
#include "wind.h"
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>

// ---- tuning (feel parameters) ----
// Speeds and accelerations carry CELL_SCALE so they stay fixed on screen when
// the cell size changes; the rates and probabilities below are per-frame and
// need no scaling, matching the convention in sand.cpp.
static const float LAVA_GRAVITY  = 25.0f * CELL_SCALE;  // matches sand and fluid
static const float TERMINAL_LAVA = 8.0f * CELL_SCALE;   // far under sand's 40 —
                                                        // a low terminal speed is
                                                        // what reads as molten ooze
                                                        // rather than falling grit
static const float SPREAD_HOT    = 0.28f;   // sideways-step chance at full heat.
                                            // Scaled by heat, this one number is
                                            // the whole viscosity model, and it is
                                            // the main dial for how lava feels:
                                            // higher self-levels into a thin sheet,
                                            // lower piles into a mound.
static const float COOL_AIR      = 0.0012f; // per frame, fully exposed (~14 s)
static const float COOL_BASE     = 0.0006f; // per frame regardless of exposure (~28 s).
                                            // Buried cells shed heat only through this
                                            // term, so it sets how long a thick pile
                                            // takes to set all the way through — and
                                            // it guarantees lava sealed inside sand or
                                            // rock still freezes instead of glowing
                                            // there forever.
static const float QUENCH_RATE   = 0.15f;   // per frame, per water face (~7 frames)
static const float BOIL_PROB     = 0.15f;   // chance a touched water cell boils off
static const float WIND_COOL     = 0.010f;  // per frame in a full gust (~1.7 s)
static const float WIND_COOL_REF = 30.0f * CELL_SCALE;  // gust speed that cools flat out
static const float MELT_MIN      = 0.35f;   // heat below which lava can't melt sand
static const float MELT_PROB     = 0.08f;   // per-frame chance per touched grain
static const float MELT_COST     = 0.25f;   // heat spent melting one grain — this is
                                            // what keeps melting self-limiting
static const float MELT_INHERIT  = 0.5f;    // heat the new melt starts with

// Per-cell state, parallel to grid[][]. Carried along when a cell moves and
// zeroed at the source, the same way sand carries its fall state.
static float heat[GRID_HEIGHT][GRID_WIDTH];
static float fallVel[GRID_HEIGHT][GRID_WIDTH];
static float fallFrac[GRID_HEIGHT][GRID_WIDTH];
static unsigned char movedFlag[GRID_HEIGHT][GRID_WIDTH];
static bool active = false;

static const int NEIGH[4][2] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};

static inline int cidx(int x, int y) { return x + GRID_WIDTH * y; }
static inline bool in_grid(int x, int y) {
    return x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT;
}
static inline float rand01() { return (float)std::rand() / (float)RAND_MAX; }

// Water occupancy, straight from the solver's own per-step classification:
// CELL_FLUID means "not blocked and holding at least one particle", which is
// exactly the question lava asks. It is one frame stale — lava runs ahead of
// fluid_step — just as sand's particle index is.
static inline bool has_water(const FluidState& f, int x, int y) {
    return in_grid(x, y) && f.cell[cidx(x, y)] == CELL_FLUID;
}

// Somewhere a molten cell can flow into. Water counts as a wall: lava crusts
// against a pool instead of sinking through it, which is what lets repeated
// pours stack up into an island rather than settling on the bottom.
static inline bool can_take(const FluidState& f, int x, int y) {
    return in_grid(x, y) && grid[y][x] == EMPTY && !has_water(f, x, y);
}

static void move_lava(int x, int y, int nx, int ny) {
    heat[ny][nx]     = heat[y][x];
    fallVel[ny][nx]  = fallVel[y][x];
    fallFrac[ny][nx] = fallFrac[y][x];
    heat[y][x]     = 0.0f;
    fallVel[y][x]  = 0.0f;
    fallFrac[y][x] = 0.0f;
    grid[ny][nx] = LAVA;
    grid[y][x]   = EMPTY;
    movedFlag[ny][nx] = 1;
}

static void freeze(int x, int y) {
    grid[y][x]     = ROCK;
    heat[y][x]     = 0.0f;
    fallVel[y][x]  = 0.0f;
    fallFrac[y][x] = 0.0f;
}

void lava_clear() {
    memset(heat, 0, sizeof(heat));
    memset(fallVel, 0, sizeof(fallVel));
    memset(fallFrac, 0, sizeof(fallFrac));
    active = false;
}

void lava_add(int x, int y) {
    if (!in_grid(x, y)) return;
    // Only open space takes a stamp. Walls and set rock stand; sand has to be
    // melted through, which costs heat, so the brush can't dissolve a dune for
    // free; and re-stamping molten lava would let a held brush pin it at white
    // heat forever.
    if (grid[y][x] != EMPTY) return;
    grid[y][x]     = LAVA;
    heat[y][x]     = 1.0f;
    fallVel[y][x]  = 0.0f;
    fallFrac[y][x] = 0.0f;
    active = true;
}

void lava_erase(int x, int y) {
    if (!in_grid(x, y)) return;
    heat[y][x]     = 0.0f;
    fallVel[y][x]  = 0.0f;
    fallFrac[y][x] = 0.0f;
}

bool lava_active() { return active; }

float lava_heat(int x, int y) { return in_grid(x, y) ? heat[y][x] : 0.0f; }

void lava_update(FluidState& f, int frame, float dt) {
    if (!active) return;
    memset(movedFlag, 0, sizeof(movedFlag));

    bool anyLeft = false;
    const bool windy = wind_active();  // hoisted: this can rescan the whole field

    // Bottom-up so a falling column stays ordered, with the horizontal sweep
    // alternating so sideways spread picks up no directional bias — the same
    // sweep sand uses.
    bool ltr = frame & 1;
    for (int y = GRID_HEIGHT - 1; y >= 0; --y) {
        for (int xx = 0; xx < GRID_WIDTH; ++xx) {
            int x = ltr ? xx : GRID_WIDTH - 1 - xx;
            if (grid[y][x] != LAVA || movedFlag[y][x]) continue;

            // Survey the four faces once: how much of this cell is open to air,
            // whether it is touching water, and whether there is sand to eat.
            int exposed = 0;
            float quench = 0.0f;
            float gust = 0.0f;      // strongest air speed on an open face
            int wx = -1, wy = -1;   // a water neighbour to boil, if any
            int sx = -1, sy = -1;   // a sand neighbour to melt, if any
            for (int k = 0; k < 4; ++k) {
                int ax = x + NEIGH[k][0], ay = y + NEIGH[k][1];
                if (!in_grid(ax, ay)) continue;
                if (has_water(f, ax, ay))      { quench += QUENCH_RATE; wx = ax; wy = ay; }
                else if (grid[ay][ax] == SAND)  { sx = ax; sy = ay; }
                else if (grid[ay][ax] == EMPTY) {
                    ++exposed;
                    // Sample the air in the open cell, not in this one: a gust
                    // is never stamped into a blocked cell, so lava's own
                    // position always reads still.
                    if (windy) {
                        float wu = wind_u(ax, ay), wv = wind_v(ax, ay);
                        gust = std::max(gust, std::sqrt(wu * wu + wv * wv));
                    }
                }
            }

            // Heat leaves only through open faces, so a thick flow crusts from
            // the outside in and keeps a molten core long after its skin sets.
            float h = heat[y][x];
            h -= COOL_BASE + COOL_AIR * (float)exposed * 0.25f;
            h -= quench;

            // Moving air carries heat off an open face far faster than still
            // air does, so a gust can freeze a flow where it stands.
            if (gust > WIND_EPS)
                h -= WIND_COOL * std::min(gust / WIND_COOL_REF, 1.0f);

            // Boil off some of the water doing the quenching. The lava sets
            // within a few frames, so only a little is ever consumed — a pour
            // hisses and crusts over instead of draining the pool.
            if (wx >= 0 && rand01() < BOIL_PROB) fluid_remove(f, wx, wy);

            if (h <= 0.0f) { freeze(x, y); continue; }

            // Melting is paid for out of this cell's own heat, and that cost is
            // what stops a flow from chain-reacting through a whole dune: a cell
            // can eat two or three grains before it has nothing left and sets.
            if (sx >= 0 && h > MELT_MIN && rand01() < MELT_PROB) {
                grid[sy][sx]      = LAVA;
                heat[sy][sx]      = h * MELT_INHERIT;
                fallVel[sy][sx]   = 0.0f;
                fallFrac[sy][sx]  = 0.0f;
                movedFlag[sy][sx] = 1;   // just formed; let it move next frame
                anyLeft = true;
                h -= MELT_COST;
                if (h <= 0.0f) { freeze(x, y); continue; }
            }

            heat[y][x] = h;
            anyLeft = true;

            // Integrate gravity against a low terminal speed, then fall
            // floor(travel) cells carrying the fractional remainder, stepping
            // one cell at a time so nothing teleports through an obstacle.
            float v = std::min(fallVel[y][x] + LAVA_GRAVITY * dt, TERMINAL_LAVA);
            fallVel[y][x] = v;

            float travel = v * dt + fallFrac[y][x];
            int steps = (int)travel;
            fallFrac[y][x] = travel - steps;

            int cy = y;
            while (steps > 0 && can_take(f, x, cy + 1)) {
                move_lava(x, cy, x, cy + 1);
                ++cy;
                --steps;
            }
            if (can_take(f, x, cy + 1)) continue;  // still falling freely

            // Landed. Unlike a grain, a liquid keeps looking for somewhere to go.
            fallVel[cy][x]  = 0.0f;
            fallFrac[cy][x] = 0.0f;

            int dir = (std::rand() & 1) ? 1 : -1;
            if (can_take(f, x + dir, cy + 1)) { move_lava(x, cy, x + dir, cy + 1); continue; }
            if (can_take(f, x - dir, cy + 1)) { move_lava(x, cy, x - dir, cy + 1); continue; }

            // Nothing below to fall into, so spread out instead. The chance
            // scales with heat: a bright flow self-levels into a wide sheet,
            // while the same code leaves a dim one standing as a mound.
            if (rand01() < SPREAD_HOT * heat[cy][x]) {
                if (can_take(f, x + dir, cy)) { move_lava(x, cy, x + dir, cy); continue; }
                if (can_take(f, x - dir, cy)) { move_lava(x, cy, x - dir, cy); continue; }
            }
        }
    }

    active = anyLeft;
}
