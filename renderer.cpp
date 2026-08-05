#include "renderer.h"
#include "wind.h"
#include <vector>
#include <cmath>
#include <algorithm>

// Water and sand are rendered as metaballs: each particle (or sand cell)
// splats a radial blob into a half-resolution field, which is thresholded
// into a smooth surface. Solids stay as crisp rectangles.
static const int   RW = WIDTH / 2;
static const int   RH = HEIGHT / 2;
static const float PSCALE = CELL_SIZE * 0.5f;  // half-res pixels per cell
static const float BLOB_R = 0.9f * PSCALE;     // particle splat radius

// Sand cells sit a full cell apart (vs 0.5 for particles), so their blobs
// need a proportionally larger radius to merge into one silhouette.
static const float SAND_BLOB_R = 1.25f * PSCALE;

// Sand wetness dynamics (per frame, at ~60 fps)
static const float WET_RISE = 0.5f;    // approach rate toward 1 under water
static const float DRY_RATE = 0.008f;  // decay rate in air (~2 s to dry out)

static SDL_Renderer* viewRenderer = nullptr;  // kept for the window->sim mapping
static SDL_Texture* fluidTex = nullptr;
static SDL_Texture* sandTex = nullptr;
static std::vector<float> blobF;   // water blob coverage
static std::vector<float> speedF;  // blob-weighted particle speed
static std::vector<float> sandF;   // sand blob coverage
static std::vector<float> wetF;    // persistent per-pixel sand wetness

void renderer_init(SDL_Renderer* renderer) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");  // smooth upscale
    viewRenderer = renderer;
    // Draw in fixed simulation pixels and let SDL scale them to the window, so
    // resizing or going fullscreen zooms the whole sim instead of leaving the
    // extra window area untouched. The aspect ratio is preserved, so a window
    // shaped differently than the sim gets letterbox bars rather than a stretch.
    SDL_RenderSetLogicalSize(renderer, WIDTH, HEIGHT);
    fluidTex = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        RW, RH
    );
    SDL_SetTextureBlendMode(fluidTex, SDL_BLENDMODE_BLEND);
    sandTex = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        RW, RH
    );
    SDL_SetTextureBlendMode(sandTex, SDL_BLENDMODE_BLEND);
    blobF.resize(RW * RH);
    speedF.resize(RW * RH);
    sandF.resize(RW * RH);
    wetF.assign(RW * RH, 0.0f);
}

static void draw_background(SDL_Renderer* renderer) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
}

static float smoothstep(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = std::max(0.0f, std::min(t, 1.0f));
    return t * t * (3.0f - 2.0f * t);
}

static Uint8 lerp8(int a, int b, float t) {
    return (Uint8)(a + (b - a) * t);
}

// Stable per-coordinate hash in [0,1]; depends only on position, so the
// grain pattern never shimmers between frames.
static float hash01(int x, int y) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xffffu) / 65535.0f;
}

static void splat_particles(const FluidState& fluid) {
    std::fill(blobF.begin(), blobF.end(), 0.0f);
    std::fill(speedF.begin(), speedF.end(), 0.0f);

    const float invR2 = 1.0f / (BLOB_R * BLOB_R);
    for (const Particle& pt : fluid.particles) {
        float fx = pt.x * PSCALE;
        float fy = pt.y * PSCALE;
        float speed = std::sqrt(pt.vx * pt.vx + pt.vy * pt.vy);

        int x0 = std::max(0, (int)(fx - BLOB_R));
        int x1 = std::min(RW - 1, (int)(fx + BLOB_R));
        int y0 = std::max(0, (int)(fy - BLOB_R));
        int y1 = std::min(RH - 1, (int)(fy + BLOB_R));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                float dx = x + 0.5f - fx;
                float dy = y + 0.5f - fy;
                float d2 = (dx * dx + dy * dy) * invR2;
                if (d2 >= 1.0f) continue;
                float w = 1.0f - d2;
                w *= w;
                blobF[y * RW + x] += w;
                speedF[y * RW + x] += w * speed;
            }
        }
    }
}

static void splat_sand() {
    std::fill(sandF.begin(), sandF.end(), 0.0f);

    const float invR2 = 1.0f / (SAND_BLOB_R * SAND_BLOB_R);
    for (int cy = 0; cy < GRID_HEIGHT; ++cy) {
        for (int cx = 0; cx < GRID_WIDTH; ++cx) {
            if (grid[cy][cx] != SAND) continue;
            float fx = (cx + 0.5f) * PSCALE;
            float fy = (cy + 0.5f) * PSCALE;

            int x0 = std::max(0, (int)(fx - SAND_BLOB_R));
            int x1 = std::min(RW - 1, (int)(fx + SAND_BLOB_R));
            int y0 = std::max(0, (int)(fy - SAND_BLOB_R));
            int y1 = std::min(RH - 1, (int)(fy + SAND_BLOB_R));
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    float dx = x + 0.5f - fx;
                    float dy = y + 0.5f - fy;
                    float d2 = (dx * dx + dy * dy) * invR2;
                    if (d2 >= 1.0f) continue;
                    float w = 1.0f - d2;
                    w *= w;
                    sandF[y * RW + x] += w;
                }
            }
        }
    }
}

static void draw_fluid(SDL_Renderer* renderer, const FluidState& fluid) {
    if (!fluidTex) return;

    splat_particles(fluid);

    void* pixels;
    int pitch;
    SDL_LockTexture(fluidTex, nullptr, &pixels, &pitch);
    Uint32* px = static_cast<Uint32*>(pixels);
    int rowStride = pitch / 4;

    // Walk each column top-down so a running counter gives depth below the
    // local surface (it resets across gaps, so droplets shade independently).
    for (int x = 0; x < RW; ++x) {
        int wetRun = 0;
        for (int y = 0; y < RH; ++y) {
            float b = blobF[y * RW + x];
            float a = smoothstep(0.35f, 0.7f, b);
            if (a < 0.02f) {
                wetRun = 0;
                px[y * rowStride + x] = 0;
                continue;
            }
            wetRun++;

            // Depth and speed are in cell units, so both thresholds carry
            // CELL_SCALE — water shades by how deep it looks, not how many
            // cells happen to fit in that depth.
            float depth = wetRun / PSCALE;                       // in cells
            float deep = std::min(depth / (15.0f * CELL_SCALE), 1.0f);
            float speed = speedF[y * RW + x] / b;
            float foam = smoothstep(8.0f * CELL_SCALE, 30.0f * CELL_SCALE, speed) * 0.8f;

            Uint8 r = lerp8(lerp8(40, 8, deep),   210, foam);
            Uint8 g = lerp8(lerp8(110, 30, deep), 230, foam);
            Uint8 bl = lerp8(lerp8(220, 90, deep), 250, foam);
            Uint8 al = (Uint8)(235.0f * a);
            px[y * rowStride + x] =
                ((Uint32)al << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | bl;
        }
    }

    SDL_UnlockTexture(fluidTex);
    SDL_Rect dst = {0, 0, WIDTH, HEIGHT};
    SDL_RenderCopy(renderer, fluidTex, nullptr, &dst);
}

// Persistent wetness: rises quickly under water coverage, dries slowly in
// air, soaks down through contiguous sand, and is blurred horizontally so it
// is continuous in both space and time (no single-frame color flips).
static void update_wetness() {
    const int n = RW * RH;

    // 1) Soft water-coverage drive: wet fast under water, dry slowly in air.
    for (int k = 0; k < n; ++k) {
        float cov = smoothstep(0.2f, 0.6f, blobF[k]);
        float w = wetF[k];
        w += (1.0f - w) * cov * WET_RISE;
        w -= DRY_RATE * (1.0f - cov);
        wetF[k] = std::max(0.0f, std::min(w, 1.0f));
    }

    // 2) Soak downward through contiguous sand so submerged piles are wet
    //    all the way through, not just at the waterline.
    for (int x = 0; x < RW; ++x) {
        float carry = 0.0f;
        for (int y = 0; y < RH; ++y) {
            int k = y * RW + x;
            if (sandF[k] > 0.3f) {
                carry = std::max(carry, wetF[k]);
                wetF[k] = carry;
            } else {
                carry = 0.0f;
            }
        }
    }

    // 3) Small horizontal blur so adjacent columns can never differ sharply.
    static std::vector<float> tmp;
    tmp = wetF;
    for (int y = 0; y < RH; ++y) {
        int row = y * RW;
        for (int x = 1; x < RW - 1; ++x) {
            int k = row + x;
            wetF[k] = 0.25f * tmp[k - 1] + 0.5f * tmp[k] + 0.25f * tmp[k + 1];
        }
    }
}

// Sand uses the same metaball treatment as water: rounded silhouettes,
// depth-shaded, with stable grain noise. The persistent wetness field tints
// sand darker wherever water overlaps, touches, or recently touched it.
static void draw_sand(SDL_Renderer* renderer) {
    if (!sandTex) return;

    splat_sand();
    update_wetness();

    void* pixels;
    int pitch;
    SDL_LockTexture(sandTex, nullptr, &pixels, &pitch);
    Uint32* px = static_cast<Uint32*>(pixels);
    int rowStride = pitch / 4;

    for (int x = 0; x < RW; ++x) {
        int run = 0;
        for (int y = 0; y < RH; ++y) {
            float s = sandF[y * RW + x];
            float a = smoothstep(0.3f, 0.6f, s);
            if (a < 0.02f) {
                run = 0;
                px[y * rowStride + x] = 0;
                continue;
            }
            run++;

            float depth = run / PSCALE;                      // in cells
            float deep = std::min(depth / (6.0f * CELL_SCALE), 1.0f);  // buried = darker
            float wet = wetF[y * RW + x] * 0.85f;
            // ~4x4 screen-pixel grain clumps, +/-8% brightness, time-stable
            float grain = 0.92f + 0.16f * hash01(x >> 1, y >> 1);

            // dry tan: light surface over darker interior; wet: deep brown
            float r = lerp8(lerp8(235, 175, deep), 110, wet) * grain;
            float g = lerp8(lerp8(200, 140, deep),  85, wet) * grain;
            float b = lerp8(lerp8(130,  75, deep),  55, wet) * grain;
            Uint8 r8 = (Uint8)std::min(r, 255.0f);
            Uint8 g8 = (Uint8)std::min(g, 255.0f);
            Uint8 b8 = (Uint8)std::min(b, 255.0f);
            Uint8 al = (Uint8)(255.0f * a);
            px[y * rowStride + x] =
                ((Uint32)al << 24) | ((Uint32)r8 << 16) | ((Uint32)g8 << 8) | b8;
        }
    }

    SDL_UnlockTexture(sandTex);
    SDL_Rect dst = {0, 0, WIDTH, HEIGHT};
    SDL_RenderCopy(renderer, sandTex, nullptr, &dst);
}

static void draw_solids(SDL_Renderer* renderer) {
    SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
    for (int y = 0; y < GRID_HEIGHT; ++y) {
        for (int x = 0; x < GRID_WIDTH; ++x) {
            if (grid[y][x] != SOLID) continue;
            SDL_Rect cell = {x * CELL_SIZE, y * CELL_SIZE, CELL_SIZE, CELL_SIZE};
            SDL_RenderFillRect(renderer, &cell);
        }
    }
}

// Moving air has nothing to draw of its own, so show it as faint streaks along
// the flow — otherwise the wind brush is invisible and the water just twitches.
// Sampled every other cell to keep the overlay readable.
static const float WIND_VIS_REF = 30.0f * CELL_SCALE;  // speed drawn full-length
static const int   WIND_VIS_STEP = (int)(2 * CELL_SCALE);  // cells between streaks

static void draw_wind(SDL_Renderer* renderer) {
    if (!wind_active()) return;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (int y = 0; y < GRID_HEIGHT; y += WIND_VIS_STEP) {
        for (int x = 0; x < GRID_WIDTH; x += WIND_VIS_STEP) {
            float wu = wind_u(x, y), wv = wind_v(x, y);
            float speed = std::sqrt(wu * wu + wv * wv);
            if (speed < 2.0f * CELL_SCALE) continue;

            float t = std::min(speed / WIND_VIS_REF, 1.0f);
            // Streak length in pixels, so the overlay keeps its density and
            // scale on screen rather than shrinking with the cell.
            float len = (0.6f + 1.4f * t) * CELL_SIZE * CELL_SCALE;
            float cx = (x + 0.5f) * CELL_SIZE;
            float cy = (y + 0.5f) * CELL_SIZE;
            // Center the streak on the cell so it reads as flow, not as a ray
            // fired from the cell corner.
            float hx = 0.5f * len * wu / speed;
            float hy = 0.5f * len * wv / speed;

            SDL_SetRenderDrawColor(renderer, 190, 215, 235, (Uint8)(30 + 110 * t));
            SDL_RenderDrawLine(renderer, (int)(cx - hx), (int)(cy - hy),
                               (int)(cx + hx), (int)(cy + hy));
            // Brighter tip, so the streak reads directionally.
            SDL_SetRenderDrawColor(renderer, 235, 245, 255, (Uint8)(60 + 150 * t));
            SDL_RenderDrawPoint(renderer, (int)(cx + hx), (int)(cy + hy));
        }
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

static void draw_grid_lines(SDL_Renderer* renderer) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
    for (int x = 0; x < WIDTH; x += CELL_SIZE)
        SDL_RenderDrawLine(renderer, x, 0, x, HEIGHT);
    for (int y = 0; y < HEIGHT; y += CELL_SIZE)
        SDL_RenderDrawLine(renderer, 0, y, WIDTH, y);
}

void renderer_draw(SDL_Renderer* renderer, const FluidState& fluid, bool showGrid) {
    draw_background(renderer);
    draw_fluid(renderer, fluid);
    draw_sand(renderer);
    draw_solids(renderer);
    draw_wind(renderer);
    if (showGrid)
        draw_grid_lines(renderer);
}

bool renderer_window_to_sim(int wx, int wy, int* sx, int* sy) {
    float fx = (float)wx, fy = (float)wy;
    if (viewRenderer) SDL_RenderWindowToLogical(viewRenderer, wx, wy, &fx, &fy);
    // Floor rather than truncate: inside a letterbox bar the coordinate is
    // negative, and truncation would fold it onto the first row or column.
    *sx = (int)std::floor(fx);
    *sy = (int)std::floor(fy);
    return *sx >= 0 && *sx < WIDTH && *sy >= 0 && *sy < HEIGHT;
}

void renderer_destroy() {
    viewRenderer = nullptr;
    if (fluidTex) {
        SDL_DestroyTexture(fluidTex);
        fluidTex = nullptr;
    }
    if (sandTex) {
        SDL_DestroyTexture(sandTex);
        sandTex = nullptr;
    }
}
