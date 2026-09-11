#include "renderer.h"
#include "wind.h"
#include "lava.h"
#include "ice.h"
#include "font_silkscreen.h"
#include <SDL_ttf.h>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>

// Water and sand are rendered as metaballs: each particle (or sand cell)
// splats a radial blob into a half-resolution field, which is thresholded
// into a smooth surface. Solids stay as crisp rectangles.
static const int   RW = SIM_W / 2;
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

// ---------------------------------------------------------------- text -----
//
// The menu is the only thing here made of glyphs rather than fields, but it
// still lives in the renderer, which is what owns SDL resource lifetime.
//
// Silkscreen is a pixel font: its glyphs are drawn on an 8px grid, so it is
// crisp at multiples of 8 and mush at anything else. Everything below exists to
// stop something from softening it on the way to the screen — the size is 3x
// native, hinting is mono (on/off coverage, not greyscale), kerning is off so
// spacing stays on the pixel grid, and glyphs are rendered Solid rather than
// Blended because Blended anti-aliases.
static const int UI_FONT_PX = 24;  // 3x Silkscreen's 8px native grid

static TTF_Font* uiFont = nullptr;

// Labels are a fixed, tiny set that never changes, so each string is rasterised
// once and then blitted. Rendered white and tinted at draw time with a color
// mod, so the same texture serves the active and idle colors.
struct TextEntry {
    // A copy, not the caller's pointer: some callers build their string in a
    // stack buffer that is gone by the next frame, and keying on that pointer
    // would leave the cache comparing freed memory.
    char key[24];
    SDL_Texture* tex;
    int w, h;
};
static TextEntry textCache[64];
static int textCacheCount = 0;

static TextEntry* text_entry(SDL_Renderer* renderer, const char* s) {
    for (int i = 0; i < textCacheCount; ++i)
        if (std::strcmp(textCache[i].key, s) == 0) return &textCache[i];
    if (!uiFont || textCacheCount >= (int)(sizeof(textCache) / sizeof(textCache[0])))
        return nullptr;

    const SDL_Color white = {255, 255, 255, 255};
    SDL_Surface* surf = TTF_RenderUTF8_Solid(uiFont, s, white);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (!tex) {
        SDL_FreeSurface(surf);
        return nullptr;
    }
    // Per-texture, deliberately: the global scale-quality hint is "1" (linear)
    // because the metaball water and sand want a smooth upscale. Flipping that
    // globally to get crisp text would visibly pixelate them instead.
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    TextEntry& e = textCache[textCacheCount++];
    std::snprintf(e.key, sizeof(e.key), "%s", s);
    e.tex = tex;
    e.w = surf->w;
    e.h = surf->h;
    SDL_FreeSurface(surf);
    return &e;
}

void renderer_text_draw(SDL_Renderer* renderer, int x, int y, const char* s,
                        SDL_Color c) {
    TextEntry* e = text_entry(renderer, s);
    if (!e) return;
    SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(e->tex, c.a);
    SDL_Rect dst = {x, y, e->w, e->h};
    SDL_RenderCopy(renderer, e->tex, nullptr, &dst);
}

void renderer_text_size(const char* s, int* w, int* h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (uiFont) TTF_SizeUTF8(uiFont, s, w, h);
}

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

    // The font is compiled in (font_silkscreen.h), so this reads from memory
    // rather than disk and cannot fail for a missing file. SDL_RWFromConstMem's
    // stream is handed to SDL_ttf with freesrc=1 so it closes it for us.
    if (TTF_Init() == 0) {
        SDL_RWops* rw = SDL_RWFromConstMem(SILKSCREEN_TTF, (int)SILKSCREEN_TTF_LEN);
        uiFont = TTF_OpenFontRW(rw, 1, UI_FONT_PX);
        if (uiFont) {
            TTF_SetFontHinting(uiFont, TTF_HINTING_MONO);
            TTF_SetFontKerning(uiFont, 0);
        }
    }
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
    SDL_Rect dst = {0, 0, SIM_W, HEIGHT};
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
    SDL_Rect dst = {0, 0, SIM_W, HEIGHT};
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

// Ice is drawn here rather than as a metaball because the blockiness is the
// point: crisp cells with per-cell facets read as crystal next to the rounded
// silhouettes of water and sand, and a pale cyan sits well clear of both the
// deep blue of water and the flat grey of a drawn wall.
// Lip thickness is a fraction of the cell, not a fixed pixel count and not a
// CELL_SCALE quantity: it wants to stay the same *share* of a block whatever the
// zoom, so a stack looks the same at any cell size. Never less than one pixel.
static const int ICE_LIP = CELL_SIZE / 5 > 0 ? CELL_SIZE / 5 : 1;

static void draw_solids(SDL_Renderer* renderer) {
    for (int y = 0; y < GRID_HEIGHT; ++y) {
        for (int x = 0; x < GRID_WIDTH; ++x) {
            int c = grid[y][x];
            if (c != SOLID && c != ROCK && c != ICE) continue;
            SDL_Rect cell = {x * CELL_SIZE, y * CELL_SIZE, CELL_SIZE, CELL_SIZE};

            if (c == ICE) {
                // Same stable position hash the sand grain and the lava crust
                // use, so the facets are pinned to the world and never shimmer
                // as a block falls past.
                float facet = 0.92f + 0.16f * hash01(x, y);
                // Melt progress shades a block from crisp pale cyan toward the
                // deeper wet blue of water, so an ice cell taking on heat visibly
                // slushes over before it finally turns to a fluid particle. The
                // lit lip fades out on the same curve — a melting surface stops
                // catching light like a facet and reads as wet.
                float m = ice_melt_progress(x, y);
                float base_r = 150.0f + (70.0f  - 150.0f) * m;
                float base_g = 214.0f + (150.0f - 214.0f) * m;
                float base_b = 240.0f + (210.0f - 240.0f) * m;
                SDL_SetRenderDrawColor(renderer,
                                       (Uint8)std::min(base_r * facet, 255.0f),
                                       (Uint8)std::min(base_g * facet, 255.0f),
                                       (Uint8)std::min(base_b * facet, 255.0f), 255);
                SDL_RenderFillRect(renderer, &cell);
                // A lit top edge wherever the block is open to the air, so a
                // stack reads as a surface catching light rather than a slab.
                if (y == 0 || grid[y - 1][x] != ICE) {
                    Uint8 lip_r = (Uint8)(228 + (120 - 228) * m);
                    Uint8 lip_g = (Uint8)(246 + (170 - 246) * m);
                    Uint8 lip_b = (Uint8)(255 + (220 - 255) * m);
                    SDL_SetRenderDrawColor(renderer, lip_r, lip_g, lip_b, 255);
                    SDL_Rect lip = {cell.x, cell.y, CELL_SIZE, ICE_LIP};
                    SDL_RenderFillRect(renderer, &lip);
                }
                continue;
            }

            // Drawn walls stay bright; rock you poured is dark basalt, so an
            // island built by quenching lava reads as its own material.
            if (c == SOLID) SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
            else            SDL_SetRenderDrawColor(renderer, 70, 62, 58, 255);
            SDL_RenderFillRect(renderer, &cell);
        }
    }
}

// Moving air has nothing to draw of its own, so the overlay is two layers: a
// streak per sampled cell, which maps the standing shape of the field, and a pool
// of tracers advected through that field, which is what actually makes the flow
// read. A static vector field is easy to miss on a busy screen; something moving
// through it is not.
//
// Speed drawn at full brightness. Well under the brush's own speed on purpose:
// the gust is feathered to nothing at its rim and blended in over several
// frames, so the field a stroke actually leaves behind mostly sits far below the
// brush's peak. Referencing the peak made the whole overlay render at a fraction
// of its intended contrast.
static const float WIND_VIS_REF = 12.0f * CELL_SCALE;
static const float WIND_VIS_MIN = 2.0f * CELL_SCALE;   // slower air isn't drawn
static const int   WIND_VIS_STEP = (int)(2 * CELL_SCALE);  // cells between streaks

// One tracer per this many windy cells, so a brush-sized gust gets a readable
// sprinkle instead of a solid mat, clamped at both ends: a floor so a small
// gust is still obvious, a ceiling so a screen-wide current stays cheap to draw.
static const int   TRACER_PER_CELLS = 6;
static const int   TRACER_MIN = 40;
static const int   TRACER_MAX = 700;
static const int   TRACER_SEED_STEP = 2;  // cells between candidate spawn points
static const int   TRAIL_PTS = 6;         // trail positions kept per tracer
static const int   TRACER_SUBSTEPS = 3;   // advection steps per frame
static const int   TRACER_LIFE = 120;     // frames before a forced respawn
static const float TRACER_FADE = 10.0f;   // frames of fade at each end of life
// The renderer has no clock of its own; this is the step main passes the solvers,
// so a tracer covers the same ground per frame as the air it is drawn from.
static const float TRACER_DT = 0.1f;

// A trail is a ring of recent positions in simulation pixels, newest at head.
struct WindTracer {
    float px[TRAIL_PTS], py[TRAIL_PTS];
    int   head;    // index of the newest point
    int   filled;  // points written so far, capped at TRAIL_PTS
    int   age;
    int   life;
    bool  alive;
};
static std::vector<WindTracer> tracers;
static std::vector<int> windySeeds;  // packed cell indices, rebuilt each frame

static inline float wrand01() { return (float)std::rand() / (float)RAND_MAX; }

// SDL folds consecutive primitives into one GPU draw, but only while the render
// state holds still — so setting a colour per streak would flush the pipeline
// thousands of times a frame, which cost more than the geometry did. Both layers
// instead quantise alpha into a few tiers, collect their geometry, and emit one
// tier at a time: the same lines and dots for a couple of dozen state changes.
static const int WIND_TIERS = 8;

struct WindSeg { int x0, y0, x1, y1; };
struct WindBatch {
    std::vector<WindSeg>  segs[WIND_TIERS];  // flow lines, in the pale colour
    std::vector<SDL_Rect> dots[WIND_TIERS];  // heads and tips, in the bright one
};
static WindBatch fieldBatch;   // the standing streaks
static WindBatch tracerBatch;  // the moving comets

static inline int wind_tier(float alpha) {
    int t = (int)(alpha * WIND_TIERS / 256.0f);
    return t < 0 ? 0 : (t >= WIND_TIERS ? WIND_TIERS - 1 : t);
}
// The alpha a whole tier is drawn at: the middle of its band. Eight bands is
// coarse arithmetically and invisible at these opacities.
static inline Uint8 wind_tier_alpha(int tier) {
    return (Uint8)((tier * 2 + 1) * 128 / WIND_TIERS);
}

static void batch_clear(WindBatch& b) {
    for (int i = 0; i < WIND_TIERS; ++i) { b.segs[i].clear(); b.dots[i].clear(); }
}

// Queue a 2-pixel-wide line as two 1-pixel lines a pixel apart across the
// direction of travel. Hairlines vanish against water and sand; this is what
// keeps a streak readable without a second render target to thicken it in.
static void batch_thick_seg(WindBatch& b, int tier, float x0, float y0,
                            float x1, float y1) {
    int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)x1, iy1 = (int)y1;
    b.segs[tier].push_back({ ix0, iy0, ix1, iy1 });
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.5f) return;
    // Perpendicular, snapped to a whole pixel: at this width anything finer just
    // lands on the same row of pixels twice.
    int ox = (int)std::lround(-dy / len), oy = (int)std::lround(dx / len);
    if (ox == 0 && oy == 0) return;
    b.segs[tier].push_back({ ix0 + ox, iy0 + oy, ix1 + ox, iy1 + oy });
}

static void batch_flush(SDL_Renderer* renderer, WindBatch& b,
                        Uint8 sr, Uint8 sg, Uint8 sb,
                        Uint8 dr, Uint8 dg, Uint8 db) {
    for (int t = 0; t < WIND_TIERS; ++t) {
        if (b.segs[t].empty()) continue;
        SDL_SetRenderDrawColor(renderer, sr, sg, sb, wind_tier_alpha(t));
        for (const WindSeg& s : b.segs[t])
            SDL_RenderDrawLine(renderer, s.x0, s.y0, s.x1, s.y1);
    }
    for (int t = 0; t < WIND_TIERS; ++t) {
        if (b.dots[t].empty()) continue;
        SDL_SetRenderDrawColor(renderer, dr, dg, db, wind_tier_alpha(t));
        SDL_RenderFillRects(renderer, b.dots[t].data(), (int)b.dots[t].size());
    }
}

// Bilinear sample of the cell-centered wind field at a point in sim pixels.
// Nearest-cell sampling makes a tracer jerk as it crosses a cell boundary; the
// interpolation is what turns the pool into smooth streamlines. Outside the grid
// wind_u/wind_v read as still air, so edges need no special case.
static void wind_sample(float x, float y, float* u, float* v) {
    float gx = x / CELL_SIZE - 0.5f, gy = y / CELL_SIZE - 0.5f;
    int x0 = (int)std::floor(gx), y0 = (int)std::floor(gy);
    float fx = gx - x0, fy = gy - y0;
    float u0 = wind_u(x0, y0) * (1 - fx) + wind_u(x0 + 1, y0) * fx;
    float u1 = wind_u(x0, y0 + 1) * (1 - fx) + wind_u(x0 + 1, y0 + 1) * fx;
    float v0 = wind_v(x0, y0) * (1 - fx) + wind_v(x0 + 1, y0) * fx;
    float v1 = wind_v(x0, y0 + 1) * (1 - fx) + wind_v(x0 + 1, y0 + 1) * fx;
    *u = u0 * (1 - fy) + u1 * fy;
    *v = v0 * (1 - fy) + v1 * fy;
}

// Drop a tracer at a random point in a random windy cell. Tracers that find no
// seed go dormant rather than sitting in still air where they'd never move.
static void tracer_spawn(WindTracer& t) {
    if (windySeeds.empty()) { t.alive = false; return; }
    int c = windySeeds[std::rand() % (int)windySeeds.size()];
    t.px[0] = (c % GRID_WIDTH + wrand01()) * CELL_SIZE;
    t.py[0] = (c / GRID_WIDTH + wrand01()) * CELL_SIZE;
    t.head = 0;
    t.filled = 1;
    t.age = 0;
    // Jittered, or every tracer spawned in the same frame would expire together
    // and the overlay would blink.
    t.life = TRACER_LIFE / 2 + (int)(wrand01() * TRACER_LIFE);
    t.alive = true;
}

// Advect the pool one frame and draw each trail as a comet: dim tail, bright head.
static void draw_wind_tracers(SDL_Renderer* renderer) {
    // Rebuild the spawn pool. Strided, since these are only candidate spawn
    // points — the tracer picks a random spot inside the cell it lands on.
    windySeeds.clear();
    for (int y = 0; y < GRID_HEIGHT; y += TRACER_SEED_STEP) {
        for (int x = 0; x < GRID_WIDTH; x += TRACER_SEED_STEP) {
            float wu = wind_u(x, y), wv = wind_v(x, y);
            if (wu * wu + wv * wv >= WIND_VIS_MIN * WIND_VIS_MIN)
                windySeeds.push_back(x + GRID_WIDTH * y);
        }
    }
    if (windySeeds.empty()) { tracers.clear(); return; }

    // Seeds are strided, so scale back up to get the windy area in cells.
    int windyCells = (int)windySeeds.size() * TRACER_SEED_STEP * TRACER_SEED_STEP;
    int want = std::max(TRACER_MIN, std::min(TRACER_MAX, windyCells / TRACER_PER_CELLS));
    if ((int)tracers.size() > want) tracers.resize(want);
    while ((int)tracers.size() < want) {
        WindTracer t;
        tracer_spawn(t);
        tracers.push_back(t);
    }

    batch_clear(tracerBatch);
    for (WindTracer& t : tracers) {
        if (!t.alive) { tracer_spawn(t); if (!t.alive) continue; }

        float speed = 0.0f;
        for (int s = 0; s < TRACER_SUBSTEPS; ++s) {
            float x = t.px[t.head], y = t.py[t.head];
            float u, v;
            wind_sample(x, y, &u, &v);
            speed = std::sqrt(u * u + v * v);
            // Air here is still — the tracer has run off the end of the gust, or
            // into the wind shadow of a wall. Restart it somewhere useful.
            if (speed < WIND_VIS_MIN) { t.alive = false; break; }
            float nx = x + u * CELL_SIZE * TRACER_DT / TRACER_SUBSTEPS;
            float ny = y + v * CELL_SIZE * TRACER_DT / TRACER_SUBSTEPS;
            if (nx < 0 || nx >= SIM_W || ny < 0 || ny >= HEIGHT) { t.alive = false; break; }
            t.head = (t.head + 1) % TRAIL_PTS;
            t.px[t.head] = nx;
            t.py[t.head] = ny;
            if (t.filled < TRAIL_PTS) ++t.filled;
        }
        if (!t.alive) continue;
        if (++t.age >= t.life) { t.alive = false; continue; }

        // Fade in on spawn and out before the forced respawn, so tracers appear
        // and vanish instead of popping mid-flow.
        float fade = std::min(1.0f, std::min(t.age / TRACER_FADE,
                                             (t.life - t.age) / TRACER_FADE));
        // A generous floor on brightness: a slow tracer still has to be visible,
        // since its motion is the only thing telling you the air is moving at all.
        float lit = fade * (0.55f + 0.45f * std::min(speed / WIND_VIS_REF, 1.0f));

        // Oldest to newest, brightening toward the head. The two segments nearest
        // the head are drawn wide, so each tracer reads as a comet rather than a
        // stray pixel; the tail thins out behind it.
        for (int i = t.filled - 1; i > 0; --i) {
            int b = (t.head - i + TRAIL_PTS) % TRAIL_PTS;
            int a = (t.head - i + 1 + TRAIL_PTS) % TRAIL_PTS;
            float w = (float)(t.filled - i) / (float)t.filled;  // 0 tail, 1 head
            int tier = wind_tier(lit * w * 235.0f);
            if (i <= 2)
                batch_thick_seg(tracerBatch, tier, t.px[b], t.py[b], t.px[a], t.py[a]);
            else
                tracerBatch.segs[tier].push_back({ (int)t.px[b], (int)t.py[b],
                                                   (int)t.px[a], (int)t.py[a] });
        }
        // A bright head, which is what the eye tracks from frame to frame.
        int htier = wind_tier(lit * 250.0f);
        tracerBatch.dots[htier].push_back(
            { (int)t.px[t.head] - 1, (int)t.py[t.head] - 1, 3, 3 });
    }

    // Additive, so overlapping trails pile into a brighter airstream the way a
    // strong gust should look.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
    batch_flush(renderer, tracerBatch, 165, 205, 240, 235, 245, 255);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

static void draw_wind(SDL_Renderer* renderer) {
    if (!wind_active()) { tracers.clear(); return; }

    batch_clear(fieldBatch);
    for (int y = 0; y < GRID_HEIGHT; y += WIND_VIS_STEP) {
        for (int x = 0; x < GRID_WIDTH; x += WIND_VIS_STEP) {
            float wu = wind_u(x, y), wv = wind_v(x, y);
            float speed = std::sqrt(wu * wu + wv * wv);
            if (speed < WIND_VIS_MIN) continue;

            float t = std::min(speed / WIND_VIS_REF, 1.0f);
            // Streak length in pixels, so the overlay keeps its density and
            // scale on screen rather than shrinking with the cell. Long enough
            // that neighbouring streaks nearly touch, which is what makes a
            // painted current read as continuous lines of flow.
            float len = (1.0f + 1.2f * t) * CELL_SIZE * CELL_SCALE;
            float cx = (x + 0.5f) * CELL_SIZE;
            float cy = (y + 0.5f) * CELL_SIZE;
            // Center the streak on the cell so it reads as flow, not as a ray
            // fired from the cell corner.
            float hx = 0.5f * len * wu / speed;
            float hy = 0.5f * len * wv / speed;

            batch_thick_seg(fieldBatch, wind_tier(70 + 130 * t),
                            cx - hx, cy - hy, cx + hx, cy + hy);
            // Brighter tip, so the streak reads directionally.
            fieldBatch.dots[wind_tier(110 + 145 * t)].push_back(
                { (int)(cx + hx), (int)(cy + hy), 2, 2 });
        }
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    batch_flush(renderer, fieldBatch, 190, 215, 235, 235, 245, 255);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    draw_wind_tracers(renderer);
}

static void draw_grid_lines(SDL_Renderer* renderer) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
    for (int x = 0; x < SIM_W; x += CELL_SIZE)
        SDL_RenderDrawLine(renderer, x, 0, x, HEIGHT);
    for (int y = 0; y < HEIGHT; y += CELL_SIZE)
        SDL_RenderDrawLine(renderer, 0, y, SIM_W, y);
}

void renderer_draw(SDL_Renderer* renderer, const FluidState& fluid, bool showGrid) {
    draw_background(renderer);

    // Everything below draws the simulation, which lives to the right of the
    // tool menu. A viewport shifts it there for free: the passes keep working in
    // sim-local pixels (0..SIM_W) exactly as they did when the sim was the whole
    // window, and SDL does the translation. The alternative — adding PANEL_W to
    // every rect and every splat — would put the offset in a hundred places
    // instead of one.
    //
    // The offset has to be *relative to the viewport already installed*, not
    // absolute. SDL_RenderSetLogicalSize implements its letterboxing by setting
    // a viewport whose origin is the top-left of the letterboxed area, so on any
    // window that isn't exactly 16:9 the base viewport carries a bar offset
    // (measured: {0,103,1600,899} in a 1300x900 window). SDL_RenderSetViewport
    // *replaces* that rather than composing with it, and passing nullptr to
    // restore resets to the whole output surface, not to the letterbox. Either
    // mistake silently drags the whole frame into the top-left corner — and
    // fit_window_to_display rounds the window size, so a non-16:9 window is the
    // normal startup case, not a corner case.
    //
    // Re-reading it every frame is also what makes a window resize self-healing,
    // since SDL rewrites the viewport itself when the window changes size.
    SDL_Rect base;
    SDL_RenderGetViewport(renderer, &base);
    const SDL_Rect simView = {base.x + PANEL_W, base.y, SIM_W, HEIGHT};
    SDL_RenderSetViewport(renderer, &simView);

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

    // Back to the letterboxed base, so the menu draws in plain logical
    // coordinates. This must be the base rect and not nullptr — see above.
    //
    // It also has to happen before this function returns, and on every path:
    // SDL translates incoming mouse events, and SDL_RenderWindowToLogical reads,
    // whichever viewport is installed at the time. input_update runs between
    // frames (main.cpp), so it sees whatever the last draw left behind — leave
    // simView installed and every coordinate arrives already shifted, which
    // renderer_logical_to_sim would then shift a second time.
    SDL_RenderSetViewport(renderer, &base);
}

bool renderer_logical_to_sim(int lx, int ly, int* sx, int* sy) {
    // The sim is drawn through a viewport starting at PANEL_W (see
    // renderer_draw), so undoing that shift here is what keeps the cell under
    // the cursor the cell that gets painted.
    const int x = lx - PANEL_W;
    if (x < 0 || x >= SIM_W || ly < 0 || ly >= HEIGHT) return false;
    *sx = x;
    *sy = ly;
    return true;
}

bool renderer_window_to_sim(int wx, int wy, int* sx, int* sy) {
    float fx = (float)wx, fy = (float)wy;
    if (viewRenderer) SDL_RenderWindowToLogical(viewRenderer, wx, wy, &fx, &fy);
    // Floor rather than truncate: inside a letterbox bar the coordinate is
    // negative, and truncation would fold it onto the first row or column.
    return renderer_logical_to_sim((int)std::floor(fx), (int)std::floor(fy),
                                   sx, sy);
}

void renderer_destroy() {
    viewRenderer = nullptr;
    for (int i = 0; i < textCacheCount; ++i)
        SDL_DestroyTexture(textCache[i].tex);
    textCacheCount = 0;
    if (uiFont) {
        TTF_CloseFont(uiFont);
        uiFont = nullptr;
        TTF_Quit();
    }
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
