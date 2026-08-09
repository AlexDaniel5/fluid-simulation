#include "renderer.h"
#include "wind.h"
#include "lava.h"
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

// Lava is emissive, so it gets a bloom halo on top of its silhouette. The halo
// is built at a quarter of the blob field and stretched back up: blurring 200x112
// instead of 800x450 is ~16x less work, and the smooth upscale SDL is already
// configured for is exactly the softness a glow wants anyway.
static const int   GW = RW / 4;
static const int   GH = RH / 4;
// Blur strides, in glow texels, applied in order. One texel spans ~1.6 cells, so
// a fixed-width blur would need dozens of passes to throw light any real distance;
// doubling the stride and coming back down reaches ~20 texels in eight passes and
// still lands on a smooth falloff.
static const int   GLOW_STRIDE[] = {1, 2, 4, 8, 8, 4, 2, 1};
static const int   GLOW_PASSES = (int)(sizeof(GLOW_STRIDE) / sizeof(GLOW_STRIDE[0]));
// Peak added brightness. A blur conserves energy, so spreading the halo this wide
// drops its peak hard and the gain has to make it back up. The lava surface is the
// clamp that keeps this safe: the added light saturates long before the core does.
static const float GLOW_GAIN = 420.0f;
// Glow texels of margin kept around the lava so the blur has somewhere to spread
// into. Must comfortably exceed the reach of GLOW_STRIDE, or the halo gets
// clipped into a visible box around the flow.
static const int   GLOW_PAD = 34;

static SDL_Renderer* viewRenderer = nullptr;  // kept for the window->sim mapping
static SDL_Texture* fluidTex = nullptr;
static SDL_Texture* sandTex = nullptr;
static SDL_Texture* lavaTex = nullptr;
static SDL_Texture* glowTex = nullptr;
static std::vector<float> blobF;   // water blob coverage
static std::vector<float> speedF;  // blob-weighted particle speed
static std::vector<float> sandF;   // sand blob coverage
static std::vector<float> wetF;    // persistent per-pixel sand wetness
static std::vector<float> lavaF;   // lava blob coverage
static std::vector<float> lavaHF;  // blob-weighted lava heat
static std::vector<float> glowF;   // quarter-res emission, blurred
static std::vector<float> glowT;   // scratch for the separable blur
// Bounding box of the lava this frame, in half-res field pixels (inclusive),
// and the same box in glow texels with room for the halo to spread.
static int lbX0, lbY0, lbX1, lbY1;
static int gbX0, gbY0, gbX1, gbY1;

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
    lavaTex = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        RW, RH
    );
    SDL_SetTextureBlendMode(lavaTex, SDL_BLENDMODE_BLEND);
    // Additive, so the halo lights the black background instead of covering it.
    glowTex = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        GW, GH
    );
    SDL_SetTextureBlendMode(glowTex, SDL_BLENDMODE_ADD);
    blobF.resize(RW * RH);
    speedF.resize(RW * RH);
    sandF.resize(RW * RH);
    wetF.assign(RW * RH, 0.0f);
    lavaF.resize(RW * RH);
    lavaHF.resize(RW * RH);
    glowF.resize(GW * GH);
    glowT.resize(GW * GH);
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

// Lava cells sit a full cell apart like sand, so they reuse SAND_BLOB_R. The
// heat is splatted alongside the coverage and divided back out at shading time,
// the same weighted-average trick water uses for particle speed.
//
// Unlike water and sand, every lava pass works inside the flow's bounding box
// rather than sweeping the whole field. Lava is usually a small part of the
// scene but needs two more full-screen textures than they do, so paying for the
// whole window on every frame would cost more than the material is worth.
// Returns false when there is nothing to draw.
static bool splat_lava() {
    int cx0 = GRID_WIDTH, cy0 = GRID_HEIGHT, cx1 = -1, cy1 = -1;
    for (int cy = 0; cy < GRID_HEIGHT; ++cy)
        for (int cx = 0; cx < GRID_WIDTH; ++cx)
            if (grid[cy][cx] == LAVA) {
                if (cx < cx0) cx0 = cx;
                if (cx > cx1) cx1 = cx;
                if (cy < cy0) cy0 = cy;
                if (cy > cy1) cy1 = cy;
            }
    if (cx1 < 0) return false;

    const int pad = (int)SAND_BLOB_R + 2;
    lbX0 = std::max(0,      (int)(cx0 * PSCALE) - pad);
    lbY0 = std::max(0,      (int)(cy0 * PSCALE) - pad);
    lbX1 = std::min(RW - 1, (int)((cx1 + 1) * PSCALE) + pad);
    lbY1 = std::min(RH - 1, (int)((cy1 + 1) * PSCALE) + pad);

    for (int y = lbY0; y <= lbY1; ++y) {
        int row = y * RW;
        std::fill(lavaF.begin()  + row + lbX0, lavaF.begin()  + row + lbX1 + 1, 0.0f);
        std::fill(lavaHF.begin() + row + lbX0, lavaHF.begin() + row + lbX1 + 1, 0.0f);
    }

    const float invR2 = 1.0f / (SAND_BLOB_R * SAND_BLOB_R);
    for (int cy = cy0; cy <= cy1; ++cy) {
        for (int cx = cx0; cx <= cx1; ++cx) {
            if (grid[cy][cx] != LAVA) continue;
            float h = lava_heat(cx, cy);
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
                    lavaF[y * RW + x] += w;
                    lavaHF[y * RW + x] += w * h;
                }
            }
        }
    }
    return true;
}

// Emission averaged down to the glow grid, then blurred there. Squaring the
// heat makes the halo fall away much faster than the surface colour does, so a
// fresh pour floods the room with light while a cooling one only smoulders.
static void build_glow() {
    // The lava box in glow texels, grown by GLOW_PAD so the blur has empty
    // margin to bleed into. Everything below stays inside this window, and the
    // blur clamps to its edges — which is why the padding has to be generous.
    gbX0 = std::max(0,      lbX0 / 4 - GLOW_PAD);
    gbY0 = std::max(0,      lbY0 / 4 - GLOW_PAD);
    gbX1 = std::min(GW - 1, lbX1 / 4 + GLOW_PAD);
    gbY1 = std::min(GH - 1, lbY1 / 4 + GLOW_PAD);

    for (int gy = gbY0; gy <= gbY1; ++gy) {
        for (int gx = gbX0; gx <= gbX1; ++gx) {
            float sum = 0.0f;
            for (int j = 0; j < 4; ++j) {
                int sy = gy * 4 + j;
                if (sy >= RH) break;
                for (int i = 0; i < 4; ++i) {
                    int sx = gx * 4 + i;
                    if (sx >= RW) break;
                    if (sx < lbX0 || sx > lbX1 || sy < lbY0 || sy > lbY1) continue;
                    int k = sy * RW + sx;
                    float c = lavaF[k];
                    if (c <= 0.0f) continue;
                    float h = lavaHF[k] / c;
                    sum += smoothstep(0.3f, 0.6f, c) * h * h;
                }
            }
            glowF[gy * GW + gx] = sum * (1.0f / 16.0f);
        }
    }

    // Separable 1-2-1 passes at a widening stride, clamped to the window so a
    // flow up against a wall still lights it instead of fading into the border.
    for (int p = 0; p < GLOW_PASSES; ++p) {
        int s = GLOW_STRIDE[p];
        for (int y = gbY0; y <= gbY1; ++y) {
            int row = y * GW;
            for (int x = gbX0; x <= gbX1; ++x) {
                int xl = x - s < gbX0 ? gbX0 : x - s;
                int xr = x + s > gbX1 ? gbX1 : x + s;
                glowT[row + x] = 0.25f * glowF[row + xl] + 0.5f * glowF[row + x]
                               + 0.25f * glowF[row + xr];
            }
        }
        for (int y = gbY0; y <= gbY1; ++y) {
            int row = y * GW;
            int up = (y - s < gbY0 ? gbY0 : y - s) * GW;
            int dn = (y + s > gbY1 ? gbY1 : y + s) * GW;
            for (int x = gbX0; x <= gbX1; ++x)
                glowF[row + x] = 0.25f * glowT[up + x] + 0.5f * glowT[row + x]
                               + 0.25f * glowT[dn + x];
        }
    }
}

// Lava gets the same metaball silhouette as sand and water, but shaded by heat
// rather than by depth: white-hot through orange to deep red, then a dark
// mottled crust as it sets. The mottling is faded in only at the cool end, so
// molten lava stays smooth and glowing and only the crust looks like rock.
static bool draw_lava(SDL_Renderer* renderer) {
    if (!lavaTex) return false;
    if (!splat_lava()) return false;

    SDL_Rect box = {lbX0, lbY0, lbX1 - lbX0 + 1, lbY1 - lbY0 + 1};
    void* pixels;
    int pitch;
    // Only the box is locked, so only the box is uploaded — and only the box is
    // copied out below, which is what keeps a small flow cheap.
    SDL_LockTexture(lavaTex, &box, &pixels, &pitch);
    Uint32* px = static_cast<Uint32*>(pixels);
    int rowStride = pitch / 4;

    for (int j = 0; j < box.h; ++j) {
        for (int i = 0; i < box.w; ++i) {
            int x = lbX0 + i, y = lbY0 + j;
            int k = y * RW + x;
            float c = lavaF[k];
            float a = smoothstep(0.3f, 0.6f, c);
            if (a < 0.02f) {
                px[j * rowStride + i] = 0;
                continue;
            }
            float h = lavaHF[k] / c;

            // Weighted to orange rather than yellow across most of the range:
            // only the very hottest lava goes pale, and the bloom on top of it
            // supplies the white-hot impression. A yellower ramp shades within a
            // few points of dry sand, which makes the two hard to tell apart.
            Uint8 r, g, b;
            if (h > 0.6f) {          // hottest
                float t = (h - 0.6f) / 0.4f;
                r = lerp8(255, 255, t); g = lerp8(120, 175, t); b = lerp8(25, 70, t);
            } else if (h > 0.3f) {   // orange
                float t = (h - 0.3f) / 0.3f;
                r = lerp8(170, 255, t); g = lerp8(35, 120, t); b = lerp8(15, 25, t);
            } else {                 // deep red down to dark crust
                float t = h / 0.3f;
                r = lerp8(55, 170, t); g = lerp8(38, 35, t); b = lerp8(36, 15, t);
            }

            // The mottle is keyed off field coordinates, not the box-local ones,
            // so the crust pattern stays pinned to the world as the box moves.
            float crust = 1.0f - smoothstep(0.15f, 0.55f, h);
            float mott = 1.0f + (0.86f + 0.28f * hash01(x >> 1, y >> 1) - 1.0f) * crust;
            Uint8 r8 = (Uint8)std::min(r * mott, 255.0f);
            Uint8 g8 = (Uint8)std::min(g * mott, 255.0f);
            Uint8 b8 = (Uint8)std::min(b * mott, 255.0f);
            Uint8 al = (Uint8)(255.0f * a);
            px[j * rowStride + i] =
                ((Uint32)al << 24) | ((Uint32)r8 << 16) | ((Uint32)g8 << 8) | b8;
        }
    }

    SDL_UnlockTexture(lavaTex);
    // The field is half resolution, so the box doubles into simulation pixels.
    SDL_Rect dst = {box.x * 2, box.y * 2, box.w * 2, box.h * 2};
    SDL_RenderCopy(renderer, lavaTex, &box, &dst);
    return true;
}

// The bloom itself: a small blurred emission map stretched over the whole
// window and added on top, so lava lights the air and the ground around it.
static void draw_glow(SDL_Renderer* renderer) {
    if (!glowTex) return;

    build_glow();

    SDL_Rect box = {gbX0, gbY0, gbX1 - gbX0 + 1, gbY1 - gbY0 + 1};
    void* pixels;
    int pitch;
    SDL_LockTexture(glowTex, &box, &pixels, &pitch);
    Uint32* px = static_cast<Uint32*>(pixels);
    int rowStride = pitch / 4;

    for (int j = 0; j < box.h; ++j) {
        for (int i = 0; i < box.w; ++i) {
            float v = glowF[(gbY0 + j) * GW + gbX0 + i] * GLOW_GAIN;
            if (v < 1.0f) {
                px[j * rowStride + i] = 0;
                continue;
            }
            v = std::min(v, 255.0f);
            // Strongly weighted to red so the halo tints the scene warm instead
            // of washing the lava underneath it out to white.
            Uint8 r = (Uint8)v;
            Uint8 g = (Uint8)(v * 0.38f);
            Uint8 b = (Uint8)(v * 0.09f);
            px[j * rowStride + i] =
                (0xffu << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | b;
        }
    }

    SDL_UnlockTexture(glowTex);
    // The glow grid is an eighth of the window on each axis.
    SDL_Rect dst = {box.x * 8, box.y * 8, box.w * 8, box.h * 8};
    SDL_RenderCopy(renderer, glowTex, &box, &dst);
}

static void draw_solids(SDL_Renderer* renderer) {
    for (int y = 0; y < GRID_HEIGHT; ++y) {
        for (int x = 0; x < GRID_WIDTH; ++x) {
            int c = grid[y][x];
            if (c != SOLID && c != ROCK) continue;
            // Drawn walls stay bright; rock you poured is dark basalt, so an
            // island built by quenching lava reads as its own material.
            if (c == SOLID) SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
            else            SDL_SetRenderDrawColor(renderer, 70, 62, 58, 255);
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
    // After the solids, which are opaque rects that would otherwise cover it,
    // and skipped outright when nothing is molten.
    if (lava_active() && draw_lava(renderer))
        draw_glow(renderer);
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
    return renderer_sim_contains(*sx, *sy);
}

bool renderer_sim_contains(int sx, int sy) {
    return sx >= 0 && sx < WIDTH && sy >= 0 && sy < HEIGHT;
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
    if (lavaTex) {
        SDL_DestroyTexture(lavaTex);
        lavaTex = nullptr;
    }
    if (glowTex) {
        SDL_DestroyTexture(glowTex);
        glowTex = nullptr;
    }
}
