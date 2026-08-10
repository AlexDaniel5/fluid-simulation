#include "wind.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {

const float GUST_BLEND = 0.6f;  // per-frame approach toward the brush velocity,
                                // so a held brush settles instead of running away

float windU[GRID_HEIGHT][GRID_WIDTH];
float windV[GRID_HEIGHT][GRID_WIDTH];
bool active = false;
bool dirty = false;  // an erase may have emptied the field; recheck on demand

inline bool in_grid(int x, int y) {
    return x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT;
}

// Blocked cells: air flows around solids, packed sand, and lava (molten or set),
// not through. So a lava flow casts a wind shadow of its own once it builds up.
inline bool blocks_wind(int x, int y) {
    int c = grid[y][x];
    return c == SOLID || c == SAND || c == LAVA || c == ROCK;
}

// Walk the segment from the gust center to (x, y) in half-cell steps. Anything
// solid on the way means this cell sits in the shadow of an obstacle.
bool visible(int cx, int cy, int x, int y) {
    float dx = (float)(x - cx), dy = (float)(y - cy);
    int steps = (int)(2.0f * std::max(std::fabs(dx), std::fabs(dy)));
    for (int k = 1; k < steps; ++k) {
        float t = (float)k / steps;
        int sx = (int)(cx + dx * t + 0.5f);
        int sy = (int)(cy + dy * t + 0.5f);
        if (!in_grid(sx, sy) || (sx == x && sy == y)) continue;
        if (blocks_wind(sx, sy)) return false;
    }
    return true;
}

} // namespace

void wind_clear() {
    memset(windU, 0, sizeof(windU));
    memset(windV, 0, sizeof(windV));
    active = false;
    dirty = false;
}

void wind_gust(int cx, int cy, int r, float wvx, float wvy) {
    if (wvx == 0.0f && wvy == 0.0f) return;
    float rim = (float)r + 1.0f;  // distance at which the gust reaches zero
    for (int y = cy - r; y <= cy + r; ++y) {
        for (int x = cx - r; x <= cx + r; ++x) {
            if (!in_grid(x, y) || blocks_wind(x, y)) continue;
            float dx = (float)(x - cx), dy = (float)(y - cy);
            float d = std::sqrt(dx * dx + dy * dy);
            if (d > (float)r + 0.5f) continue;  // disc, not square
            if (!visible(cx, cy, x, y)) continue;
            float fall = 1.0f - d / rim;
            float u = windU[y][x] + (wvx * fall - windU[y][x]) * GUST_BLEND;
            float v = windV[y][x] + (wvy * fall - windV[y][x]) * GUST_BLEND;
            // Snap the feathered rim to still air; nothing fades these out
            // later, so sub-threshold dust would linger in the field forever.
            windU[y][x] = std::fabs(u) < WIND_EPS ? 0.0f : u;
            windV[y][x] = std::fabs(v) < WIND_EPS ? 0.0f : v;
        }
    }
    active = true;
}

void wind_erase(int x, int y) {
    if (!in_grid(x, y)) return;
    if (windU[y][x] == 0.0f && windV[y][x] == 0.0f) return;
    windU[y][x] = 0.0f;
    windV[y][x] = 0.0f;
    dirty = true;
}

bool wind_active() {
    if (dirty) {
        active = false;
        for (int y = 0; y < GRID_HEIGHT && !active; ++y)
            for (int x = 0; x < GRID_WIDTH; ++x)
                if (windU[y][x] != 0.0f || windV[y][x] != 0.0f) { active = true; break; }
        dirty = false;
    }
    return active;
}

float wind_u(int x, int y) { return in_grid(x, y) ? windU[y][x] : 0.0f; }
float wind_v(int x, int y) { return in_grid(x, y) ? windV[y][x] : 0.0f; }
