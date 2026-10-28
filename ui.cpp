#include "ui.h"
#include "renderer.h"
#include "sweep.h"
#include <cstdio>

// ------------------------------------------------------------- layout ------
//
// All in logical pixels, inside the PANEL_W strip. The buttons are laid out on
// a single pitch so the whole column moves by editing one number.
static const int BTN_X    = 12;
static const int BTN_W    = PANEL_W - BTN_X * 2;   // 136
static const int BTN_H    = 58;
static const int BTN_PITCH = BTN_H + 8;
static const int TOOLS_Y  = 56;                    // first tool button
static const int SWATCH_W = 8;                     // material color bar, left edge

// The brush stepper, anchored to the bottom of the panel: a label, the current
// radius, and a square button either side of it.
static const int STEP_W    = 38;
static const int BRUSH_LABEL_Y = HEIGHT - 116;
static const int BRUSH_ROW_Y   = HEIGHT - 84;

// ------------------------------------------------------------- palette -----
//
// Swatches are lifted from what each material actually renders as, so the
// button reads as a sample of the thing it draws rather than a decoration.
static const SDL_Color C_SOLID = {200, 200, 200, 255};  // flat wall
static const SDL_Color C_SAND  = {215, 180, 105, 255};  // dry surface grain
static const SDL_Color C_WATER = {40, 110, 220, 255};   // shallow water
static const SDL_Color C_WIND  = {190, 215, 235, 255};  // streak
static const SDL_Color C_LAVA  = {255, 120, 25, 255};   // hottest band
static const SDL_Color C_ICE   = {150, 214, 240, 255};  // body
static const SDL_Color C_ERASE = {150, 150, 158, 255};
static const SDL_Color C_CLEAR = {200, 70, 60, 255};

static const SDL_Color PANEL_BG   = {18, 18, 20, 255};
static const SDL_Color PANEL_EDGE = {48, 48, 54, 255};
static const SDL_Color BTN_IDLE   = {32, 32, 36, 255};
static const SDL_Color BTN_HOVER  = {48, 48, 54, 255};
static const SDL_Color BTN_ACTIVE = {62, 62, 70, 255};
static const SDL_Color TEXT_IDLE  = {158, 158, 166, 255};
static const SDL_Color TEXT_ON    = {236, 236, 242, 255};
static const SDL_Color TEXT_DIM   = {104, 104, 112, 255};

enum UiAction { UI_TOOL, UI_ERASE, UI_CLEAR, UI_BRUSH_DEC, UI_BRUSH_INC };

// Two shapes of button: a full-width row with a material color bar down its
// left edge, and a small square for the brush steppers.
enum UiStyle { STYLE_ROW, STYLE_STEP };

struct UiButton {
    SDL_Rect r;
    const char* label;
    SDL_Color swatch;
    UiAction action;
    UiStyle style;
    DrawMode mode;  // meaningful only when action is UI_TOOL
};

// The single source of truth: ui_draw and ui_click both walk this, so a button
// cannot be drawn somewhere other than where it is clickable.
//
// FLUID is labelled WATER because that is what it looks like on screen; the
// enum name is a solver detail.
#define TOOL_ROW(i) {BTN_X, TOOLS_Y + BTN_PITCH * (i), BTN_W, BTN_H}
static const UiButton BUTTONS[] = {
    {TOOL_ROW(0), "SOLID", C_SOLID, UI_TOOL, STYLE_ROW, MODE_SOLID},
    {TOOL_ROW(1), "SAND",  C_SAND,  UI_TOOL, STYLE_ROW, MODE_SAND},
    {TOOL_ROW(2), "WATER", C_WATER, UI_TOOL, STYLE_ROW, MODE_FLUID},
    {TOOL_ROW(3), "WIND",  C_WIND,  UI_TOOL, STYLE_ROW, MODE_WIND},
    {TOOL_ROW(4), "LAVA",  C_LAVA,  UI_TOOL, STYLE_ROW, MODE_LAVA},
    {TOOL_ROW(5), "ICE",   C_ICE,   UI_TOOL, STYLE_ROW, MODE_ICE},
    // Below the rule: not materials, but the same shape so they are obviously
    // the same kind of control.
    {{BTN_X, TOOLS_Y + BTN_PITCH * 6 + 26, BTN_W, BTN_H}, "ERASE", C_ERASE, UI_ERASE, STYLE_ROW, MODE_SOLID},
    {{BTN_X, TOOLS_Y + BTN_PITCH * 7 + 26, BTN_W, BTN_H}, "CLEAR", C_CLEAR, UI_CLEAR, STYLE_ROW, MODE_SOLID},
    // The brush steppers sit either side of the radius they change, so which
    // number they act on needs no explaining.
    {{BTN_X, BRUSH_ROW_Y, STEP_W, STEP_W}, "-", C_ERASE, UI_BRUSH_DEC, STYLE_STEP, MODE_SOLID},
    {{BTN_X + BTN_W - STEP_W, BRUSH_ROW_Y, STEP_W, STEP_W}, "+", C_ERASE, UI_BRUSH_INC, STYLE_STEP, MODE_SOLID},
};
#undef TOOL_ROW
static const int BUTTON_COUNT = (int)(sizeof(BUTTONS) / sizeof(BUTTONS[0]));

// The rule separating tools from the destructive pair.
static const int RULE_Y = TOOLS_Y + BTN_PITCH * 6 + 12;

static bool hit(const SDL_Rect& r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// A button is "on" when it describes the current state, not when it was last
// clicked: ERASE lights up for a held E too, and the tool buttons stop looking
// active while erasing, because erasing is what the next click will do.
static bool button_is_on(const UiButton& b, const InputState& s) {
    switch (b.action) {
        case UI_TOOL:  return !input_erasing(s) && s.mode == b.mode;
        case UI_ERASE: return input_erasing(s);
        default:       return false;  // the rest are momentary; never "on"
    }
}

// A stepper that can no longer move is drawn faded, so the radius sitting at its
// limit is visible rather than something you discover by clicking and seeing
// nothing happen.
static bool button_is_dead(const UiButton& b, const InputState& s) {
    if (b.action == UI_BRUSH_DEC) return s.brushRadius <= BRUSH_MIN;
    if (b.action == UI_BRUSH_INC) return s.brushRadius >= BRUSH_MAX;
    return false;
}

static void fill(SDL_Renderer* r, const SDL_Rect& rect, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r, &rect);
}

// Outline of a circle, one point per octant step (midpoint algorithm), so the
// ring stays one pixel thick at every radius.
static void ring(SDL_Renderer* r, int cx, int cy, int rad) {
    int x = rad, y = 0, err = 1 - rad;
    while (x >= y) {
        const SDL_Point pts[8] = {
            {cx + x, cy + y}, {cx - x, cy + y}, {cx + x, cy - y}, {cx - x, cy - y},
            {cx + y, cy + x}, {cx - y, cy + x}, {cx + y, cy - x}, {cx - y, cy - x},
        };
        SDL_RenderDrawPoints(r, pts, 8);
        ++y;
        if (err < 0) err += 2 * y + 1;
        else { --x; err += 2 * (y - x) + 1; }
    }
}

// The eraser's footprint, drawn over the sim wherever the pointer is while
// erasing is on. It marks the same disc paint_at clears: centred on the middle
// of the cell under the cursor, reaching the edge of the cells brushRadius away.
// A dark ring around a light one, so it reads over water, sand, and empty
// background alike. While cells are actually being removed — a held E, or the
// button down with ERASE latched — the inside is tinted too.
static void draw_erase_cursor(SDL_Renderer* r, const InputState& s) {
    if (!input_erasing(s)) return;
    int sx, sy;
    if (!renderer_logical_to_sim(s.mouseX, s.mouseY, &sx, &sy)) return;

    const int cx = PANEL_W + (sx / CELL_SIZE) * CELL_SIZE + CELL_SIZE / 2;
    const int cy = (sy / CELL_SIZE) * CELL_SIZE + CELL_SIZE / 2;
    const int rad = s.brushRadius * CELL_SIZE + CELL_SIZE / 2;

    if (s.eraseHeld || s.isDrawing) {
        SDL_SetRenderDrawColor(r, C_ERASE.r, C_ERASE.g, C_ERASE.b, 50);
        for (int dy = -rad; dy <= rad; ++dy) {
            int half = 0;
            while ((half + 1) * (half + 1) + dy * dy <= rad * rad) ++half;
            SDL_RenderDrawLine(r, cx - half, cy + dy, cx + half, cy + dy);
        }
    }
    SDL_SetRenderDrawColor(r, 0, 0, 0, 160);
    ring(r, cx, cy, rad + 1);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 220);
    ring(r, cx, cy, rad);
}

void ui_draw(SDL_Renderer* renderer, const InputState& state) {
    // draw_wind leaves the draw blend mode at NONE and draw_grid_lines leaves
    // the color set, so set both rather than inheriting whatever the sim left.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Before the panel, so a ring overhanging the menu's edge is covered by it
    // rather than drawn across the buttons.
    draw_erase_cursor(renderer, state);

    const SDL_Rect panel = {0, 0, PANEL_W, HEIGHT};
    fill(renderer, panel, PANEL_BG);
    const SDL_Rect edge = {PANEL_W - 1, 0, 1, HEIGHT};
    fill(renderer, edge, PANEL_EDGE);

    renderer_text_draw(renderer, BTN_X, 22, "TOOLS", TEXT_DIM);

    for (int i = 0; i < BUTTON_COUNT; ++i) {
        const UiButton& b = BUTTONS[i];
        const bool on = button_is_on(b, state);
        const bool dead = button_is_dead(b, state);
        const bool hover = !dead && hit(b.r, state.mouseX, state.mouseY);

        fill(renderer, b.r, on ? BTN_ACTIVE : (hover ? BTN_HOVER : BTN_IDLE));

        // Only the full-width rows carry a material color bar; a stepper has no
        // material to sample, and a bar down its edge would read as one.
        int textLeft = b.r.x;
        if (b.style == STYLE_ROW) {
            // Full strength when the button is on or under the pointer, dimmed
            // otherwise, so the active row reads at a glance.
            SDL_Color sw = b.swatch;
            if (!on && !hover) {
                sw.r = (Uint8)(sw.r * 0.55f);
                sw.g = (Uint8)(sw.g * 0.55f);
                sw.b = (Uint8)(sw.b * 0.55f);
            }
            const SDL_Rect bar = {b.r.x, b.r.y, SWATCH_W, b.r.h};
            fill(renderer, bar, sw);
            textLeft = b.r.x + SWATCH_W;

            // A full-height edge in the material color marks the active button,
            // which is legible even where the fill difference is subtle.
            if (on) {
                const SDL_Rect right = {b.r.x + b.r.w - 3, b.r.y, 3, b.r.h};
                fill(renderer, right, b.swatch);
            }
        }

        // Centered in whatever space the color bar left.
        int tw, th;
        renderer_text_size(b.label, &tw, &th);
        const int tx = textLeft + (b.r.x + b.r.w - textLeft - tw) / 2;
        const int ty = b.r.y + (b.r.h - th) / 2;
        renderer_text_draw(renderer, tx, ty, b.label,
                           dead ? TEXT_DIM : (on ? TEXT_ON : TEXT_IDLE));
    }

    const SDL_Rect rule = {BTN_X, RULE_Y, BTN_W, 1};
    fill(renderer, rule, PANEL_EDGE);

    // The brush radius, between its two steppers. Drawn here rather than as a
    // button label because it is the value being changed, not a control.
    int lw, lh;
    renderer_text_size("BRUSH", &lw, &lh);
    renderer_text_draw(renderer, BTN_X + (BTN_W - lw) / 2, BRUSH_LABEL_Y,
                       "BRUSH", TEXT_DIM);

    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", state.brushRadius);
    int vw, vh;
    renderer_text_size(buf, &vw, &vh);
    renderer_text_draw(renderer, BTN_X + (BTN_W - vw) / 2,
                       BRUSH_ROW_Y + (STEP_W - vh) / 2, buf, TEXT_ON);
}

bool ui_click(InputState& state, FluidState& fluid, int lx, int ly) {
    // Anything landing on the strip belongs to the menu, button or not — a click
    // on the panel background must still be swallowed, or it would fall through
    // and paint into the simulation hidden behind it.
    if (lx < 0 || lx >= PANEL_W || ly < 0 || ly >= HEIGHT) return false;

    for (int i = 0; i < BUTTON_COUNT; ++i) {
        const UiButton& b = BUTTONS[i];
        if (!hit(b.r, lx, ly)) continue;

        switch (b.action) {
            case UI_TOOL:
                state.mode = b.mode;
                // Picking a material turns erasing off. Without this the tool
                // would appear to change while clicks kept rubbing out, which
                // reads as the menu being broken.
                state.eraseSticky = false;
                break;
            case UI_ERASE:
                // A toggle, so the button can be un-picked without having to
                // choose some other tool to escape it.
                state.eraseSticky = !state.eraseSticky;
                break;
            case UI_CLEAR:
                sweep_start();
                break;
            case UI_BRUSH_DEC:
                if (state.brushRadius > BRUSH_MIN) state.brushRadius--;
                break;
            case UI_BRUSH_INC:
                if (state.brushRadius < BRUSH_MAX) state.brushRadius++;
                break;
        }
        break;
    }
    return true;
}
