#include "sweep.h"
#include "input.h"
#include "wind.h"
#include "lava.h"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

namespace {

const int   SWEEP_FRAMES = 32;    // bar's crossing time, ~half a second
const int   TRAIL_PX     = 160;   // glow left behind the bar
const int   LEAD_PX      = 24;    // softer bloom just ahead of it
const int   CORE_PX      = 3;     // the bright line itself
const int   SPARK_MAX    = 900;
const int   SPARK_LIFE   = 28;    // frames
const float SPARK_GRAV   = 0.35f; // px/frame^2

struct Spark {
    float x, y, vx, vy;
    int age;
    SDL_Color c;
};

bool running = false;
int frame = 0;
int clearedCols = 0;   // columns [0, clearedCols) have been swept at least once
std::vector<Spark> sparks;

float frand() { return (float)std::rand() / (float)RAND_MAX; }

// Ease in and out, so the bar gathers speed off the left edge and coasts out
// the right rather than moving like a progress bar.
float ease(float t) {
    return t < 0.5f ? 4.0f * t * t * t
                    : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) *
                                 (-2.0f * t + 2.0f) * 0.5f;
}

// Roughly the renderer's palette — sparks only need to read as "that was sand",
// not match the shading exactly.
SDL_Color material_color(int c) {
    switch (c) {
        case SOLID: return {150, 150, 160, 255};
        case SAND:  return {225, 190, 120, 255};
        case LAVA:  return {255, 130, 40, 255};
        case ROCK:  return {110, 95, 90, 255};
        case ICE:   return {190, 240, 255, 255};
        default:    return {255, 255, 255, 255};
    }
}

void spawn(float x, float y, SDL_Color c) {
    if ((int)sparks.size() >= SPARK_MAX) return;
    // Flung forward and up off the bar, as if scraped loose by it.
    sparks.push_back({x, y, 2.0f + frand() * 7.0f, -1.5f - frand() * 5.0f, 0, c});
}

// Bar position in sim pixels. Overshoots the right edge by the trail's length
// so the glow slides off screen instead of stopping dead on the border.
int front_px() {
    float t = std::min(1.0f, (float)frame / SWEEP_FRAMES);
    return (int)(ease(t) * (SIM_W + TRAIL_PX));
}

} // namespace

void sweep_start() {
    if (running) return;
    running = true;
    frame = 0;
    clearedCols = 0;
}

bool sweep_active() { return running || !sparks.empty(); }

void sweep_update(FluidState& fluid) {
    for (Spark& s : sparks) {
        s.x += s.vx;
        s.y += s.vy;
        s.vy += SPARK_GRAV;
        s.vx *= 0.96f;
        s.age++;
    }
    sparks.erase(std::remove_if(sparks.begin(), sparks.end(),
                                [](const Spark& s) { return s.age >= SPARK_LIFE; }),
                 sparks.end());

    if (!running) return;
    frame++;

    const int frontCol = std::min(GRID_WIDTH, front_px() / CELL_SIZE);

    // Columns the bar crossed this frame: throw sparks from what was in them,
    // and drop their wind and heat. Only the newly crossed ones, since those
    // fields don't flow back.
    for (int x = clearedCols; x < frontCol; ++x) {
        for (int y = 0; y < GRID_HEIGHT; ++y) {
            if (grid[y][x] != EMPTY && (std::rand() % 6) == 0)
                spawn((x + 0.5f) * CELL_SIZE, (y + 0.5f) * CELL_SIZE,
                      material_color(grid[y][x]));
            wind_erase(x, y);
            lava_erase(x, y);
        }
    }
    for (const Particle& p : fluid.particles) {
        if ((int)p.x >= clearedCols && (int)p.x < frontCol && (std::rand() % 5) == 0)
            spawn(p.x * CELL_SIZE, p.y * CELL_SIZE, {90, 160, 255, 255});
    }
    clearedCols = std::max(clearedCols, frontCol);

    // Everything behind the bar, every frame: sand slumping or water running
    // back across the line gets caught rather than left stranded.
    for (int y = 0; y < GRID_HEIGHT; ++y)
        std::memset(grid[y], 0, sizeof(int) * frontCol);
    fluid.particles.erase(
        std::remove_if(fluid.particles.begin(), fluid.particles.end(),
                       [frontCol](const Particle& p) { return (int)p.x < frontCol; }),
        fluid.particles.end());

    if (frame >= SWEEP_FRAMES) {
        input_clear_all(fluid);  // leave exactly what an instant clear would
        running = false;
    }
}

void sweep_draw(SDL_Renderer* renderer) {
    if (!sweep_active()) return;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);

    if (running) {
        const int fx = front_px();

        // Trail: one-pixel slices fading out behind the bar, quadratically so
        // most of the light hugs the line.
        for (int i = 0; i < TRAIL_PX; i += 2) {
            float k = 1.0f - (float)i / TRAIL_PX;
            Uint8 a = (Uint8)(110.0f * k * k);
            SDL_SetRenderDrawColor(renderer, 80, 170, 255, a);
            SDL_Rect r = {fx - i - 2, 0, 2, HEIGHT};
            SDL_RenderFillRect(renderer, &r);
        }
        for (int i = 0; i < LEAD_PX; i += 2) {
            float k = 1.0f - (float)i / LEAD_PX;
            SDL_SetRenderDrawColor(renderer, 140, 210, 255, (Uint8)(90.0f * k * k));
            SDL_Rect r = {fx + i, 0, 2, HEIGHT};
            SDL_RenderFillRect(renderer, &r);
        }
        SDL_SetRenderDrawColor(renderer, 235, 250, 255, 255);
        SDL_Rect core = {fx - CORE_PX / 2, 0, CORE_PX, HEIGHT};
        SDL_RenderFillRect(renderer, &core);
    }

    for (const Spark& s : sparks) {
        float k = 1.0f - (float)s.age / SPARK_LIFE;
        SDL_SetRenderDrawColor(renderer, s.c.r, s.c.g, s.c.b, (Uint8)(255.0f * k));
        int size = s.age < SPARK_LIFE / 3 ? 3 : 2;
        SDL_Rect r = {(int)s.x - size / 2, (int)s.y - size / 2, size, size};
        SDL_RenderFillRect(renderer, &r);
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}
