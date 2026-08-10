#include "sand.h"
#include "wind.h"
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

// ---- tuning (feel parameters) ----
// Speeds and accelerations carry CELL_SCALE so they stay fixed on screen when
// the cell size changes; the probabilities are per-frame and need no scaling.
static const float SAND_GRAVITY       = 25.0f * CELL_SCALE;  // matches fluid GRAVITY
static const float TERMINAL_DRY       = 40.0f * CELL_SCALE;  // max fall speed in air
static const float TERMINAL_WET       = 6.0f * CELL_SCALE;   // max fall speed in water
static const float SLIDE_PROB         = 0.2f;   // per-frame diagonal-slide chance when blocked
static const float IMPACT_SLIDE_BONUS = 0.6f;   // extra slide chance at terminal-speed impact
static const float SLUMP_PROB         = 0.015f; // per-frame sideways slump chance underwater
static const float PUSH_VEL           = 6.0f * CELL_SCALE;  // flow that drags sand
static const float PUSH_PROB          = 0.17f;  // per-frame drag chance at threshold flow
                                                // (scales up with flow strength)
static const float SPLASH_K           = 0.5f;   // fraction of a grain's fall speed
                                                // handed to the water it displaces
static const float WIND_PUSH_VEL      = 8.0f * CELL_SCALE;   // air that rolls a grain
static const float WIND_LIFT_VEL      = 20.0f * CELL_SCALE;  // updraft that lifts one
static const float WIND_PUSH_PROB     = 0.25f;  // per-frame chance at threshold wind
                                                // (scales up with wind strength)
static const float WIND_WET_GRIP      = 0.25f;  // wet grains cling, so wind barely
                                                // budges them

// Per-grain fall state, parallel to grid[][]. Carried along when a grain
// moves; zeroed when it lands or vacates a cell.
static float fallVel[GRID_HEIGHT][GRID_WIDTH];
static float fallFrac[GRID_HEIGHT][GRID_WIDTH];

// Per-cell linked list of particle indices, rebuilt each update and kept in
// sync as cells are swapped, so water occupancy reads stay current mid-pass.
static int head[GRID_WIDTH * GRID_HEIGHT];
static std::vector<int> nxt;
static unsigned char movedFlag[GRID_HEIGHT][GRID_WIDTH];
// Cells that received displaced water this frame. Sand won't move into them
// until next frame, so one grain sweep can't re-displace the same water.
static unsigned char waterMoved[GRID_HEIGHT][GRID_WIDTH];

static inline int cidx(int x, int y) { return x + GRID_WIDTH * y; }
static inline int uidx(int x, int y) { return x + (GRID_WIDTH + 1) * y; }
static inline bool in_grid(int x, int y) {
    return x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT;
}
static inline bool has_water(int x, int y) {
    return in_grid(x, y) && head[cidx(x, y)] != -1;
}
static inline bool is_empty(int x, int y) {
    return in_grid(x, y) && grid[y][x] == EMPTY;
}
static inline bool can_take(int x, int y) {
    return is_empty(x, y) && !waterMoved[y][x];
}
static inline bool is_wet(int x, int y) {
    return has_water(x, y - 1) || has_water(x - 1, y) ||
           has_water(x + 1, y) || has_water(x, y + 1);
}
static inline float rand01() { return (float)std::rand() / (float)RAND_MAX; }

static void build_particle_index(const FluidState& f) {
    for (int c = 0; c < GRID_WIDTH * GRID_HEIGHT; ++c) head[c] = -1;
    nxt.assign(f.particles.size(), -1);
    for (int k = 0; k < (int)f.particles.size(); ++k) {
        int i = std::min((int)f.particles[k].x, GRID_WIDTH - 1);
        int j = std::min((int)f.particles[k].y, GRID_HEIGHT - 1);
        nxt[k] = head[cidx(i, j)];
        head[cidx(i, j)] = k;
    }
}

// Move sand from (x,y) to (nx,ny), carrying its fall state. Water in the
// destination is displaced (sub-cell offsets preserved), never destroyed:
// a sinking grain vents it sideways when a neighbor is open, falling back to
// the vacated source cell only when boxed in. Always swapping upward would
// let a falling column bucket-brigade the same water to its top — one cell
// per grain, all in one sweep — pumping the pool up to the pour point.
static void move_sand(FluidState& f, int x, int y, int nx, int ny) {
    int wlist = head[cidx(nx, ny)];
    if (wlist != -1) {
        int wx = x, wy = y;
        if (ny > y) {
            int side = (std::rand() & 1) ? 1 : -1;
            if (can_take(nx + side, ny))      { wx = nx + side; wy = ny; }
            else if (can_take(nx - side, ny)) { wx = nx - side; wy = ny; }
        }
        // Splash: hand the water a share of the grain's fall momentum, along
        // the vent direction. Horizontal only — an upward kick is the geyser
        // direction, and the boxed-in (swap-up) case gets none by design.
        float kick = SPLASH_K * fallVel[y][x] * (float)(wx - nx);
        int last = wlist;
        for (int k = wlist; k != -1; k = nxt[k]) {
            f.particles[k].x += (float)(wx - nx);
            f.particles[k].y += (float)(wy - ny);
            f.particles[k].vx += kick;
            last = k;
        }
        // Splice into the target's list — the target may already hold water;
        // the drift compensation decompresses the doubled-up cell.
        nxt[last] = head[cidx(wx, wy)];
        head[cidx(wx, wy)] = wlist;
        head[cidx(nx, ny)] = -1;
        waterMoved[wy][wx] = 1;
    }
    fallVel[ny][nx]  = fallVel[y][x];
    fallFrac[ny][nx] = fallFrac[y][x];
    fallVel[y][x]  = 0.0f;
    fallFrac[y][x] = 0.0f;
    grid[ny][nx] = SAND;
    grid[y][x]  = EMPTY;
    movedFlag[ny][nx] = 1;
}

// Horizontal water velocity around a sand cell. The faces of the sand cell
// itself are zeroed by the solid boundary, so look at neighboring water cells.
static float water_flow_u(const FluidState& f, int x, int y) {
    static const int off[4][2] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
    float sum = 0.0f;
    int n = 0;
    for (int k = 0; k < 4; ++k) {
        int wx = x + off[k][0], wy = y + off[k][1];
        if (!has_water(wx, wy)) continue;
        sum += 0.5f * (f.u[uidx(wx, wy)] + f.u[uidx(wx + 1, wy)]);
        ++n;
    }
    return n ? sum / n : 0.0f;
}

void sand_update(FluidState& f, int frame, float dt) {
    build_particle_index(f);
    memset(movedFlag, 0, sizeof(movedFlag));
    memset(waterMoved, 0, sizeof(waterMoved));

    // Bottom-up so a falling column stays ordered; alternate the horizontal
    // sweep so sideways moves don't pick up a directional bias. The moved
    // flag keeps multi-cell falls from being processed again this frame.
    bool ltr = frame & 1;
    for (int y = GRID_HEIGHT - 1; y >= 0; --y) {
        for (int xx = 0; xx < GRID_WIDTH; ++xx) {
            int x = ltr ? xx : GRID_WIDTH - 1 - xx;
            if (grid[y][x] != SAND || movedFlag[y][x]) continue;

            // Integrate gravity; water drag caps submerged grains far lower.
            float term = is_wet(x, y) ? TERMINAL_WET : TERMINAL_DRY;
            float v = std::min(fallVel[y][x] + SAND_GRAVITY * dt, term);
            fallVel[y][x] = v;

            // Fall floor(travel) cells, carrying the fractional remainder so
            // slow grains still creep. Step cell by cell, stopping at the
            // first blocked cell instead of teleporting through obstacles.
            float travel = v * dt + fallFrac[y][x];
            int steps = (int)travel;
            fallFrac[y][x] = travel - steps;

            int cy = y;
            while (steps > 0 && can_take(x, cy + 1)) {
                move_sand(f, x, cy, x, cy + 1);
                ++cy;
                --steps;
            }
            // Still falling freely — including when only freshly displaced
            // water blocks the way; keep the fall speed and retry next frame.
            if (is_empty(x, cy + 1)) continue;

            // Landed (or resting on a pile): kill the fall motion. A hard
            // impact splashes outward by raising the diagonal-slide chance.
            float impact = v;
            fallVel[cy][x] = 0.0f;
            fallFrac[cy][x] = 0.0f;

            int dir = (std::rand() & 1) ? 1 : -1;
            float slideP = SLIDE_PROB + IMPACT_SLIDE_BONUS * (impact / TERMINAL_DRY);
            if (rand01() < slideP) {
                if (can_take(x + dir, cy + 1)) { move_sand(f, x, cy, x + dir, cy + 1); continue; }
                if (can_take(x - dir, cy + 1)) { move_sand(f, x, cy, x - dir, cy + 1); continue; }
            }

            // Submerged piles slump: an occasional purely sideways step
            // (always onto support, or the diagonal above would have hit)
            // spreads slopes shallower than the dry 45 degrees.
            if (is_wet(x, cy) && rand01() < SLUMP_PROB && can_take(x + dir, cy)) {
                move_sand(f, x, cy, x + dir, cy);
                continue;
            }

            // Wind rolls grains whose top face is open to the air: sideways
            // where it can, scrambling up a step where the side is blocked,
            // and a strong updraft plucks one clear of the pile altogether.
            // Buried grains are shielded and wet ones cling together.
            if (wind_active() && is_empty(x, cy - 1) && !has_water(x, cy - 1)) {
                float wu = wind_u(x, cy), wv = wind_v(x, cy);
                float grip = is_wet(x, cy) ? WIND_WET_GRIP : 1.0f;
                if (std::fabs(wu) > WIND_PUSH_VEL &&
                    rand01() < WIND_PUSH_PROB * grip * std::fabs(wu) / WIND_PUSH_VEL) {
                    int wdir = wu > 0.0f ? 1 : -1;
                    if (can_take(x + wdir, cy)) {
                        move_sand(f, x, cy, x + wdir, cy);
                        continue;
                    }
                    if (can_take(x + wdir, cy - 1)) {
                        move_sand(f, x, cy, x + wdir, cy - 1);
                        continue;
                    }
                }
                // +y is down, so an updraft is negative v.
                if (wv < -WIND_LIFT_VEL &&
                    rand01() < WIND_PUSH_PROB * grip * -wv / WIND_LIFT_VEL &&
                    can_take(x, cy - 1)) {
                    move_sand(f, x, cy, x, cy - 1);
                    continue;
                }
            }

            // Strong horizontal current drags exposed sand sideways; faster
            // flow drags proportionally more often.
            float flow = water_flow_u(f, x, cy);
            if (std::fabs(flow) > PUSH_VEL &&
                rand01() < PUSH_PROB * std::fabs(flow) / PUSH_VEL) {
                dir = flow > 0.0f ? 1 : -1;
                if (can_take(x + dir, cy))
                    move_sand(f, x, cy, x + dir, cy);
            }
        }
    }
}
