#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <SDL.h>
#include "grid.h"
#include "fluid.h"
#include "sand.h"
#include "wind.h"
#include "lava.h"
#include "renderer.h"
#include "input.h"

// Single definition of the shared grid
int grid[GRID_HEIGHT][GRID_WIDTH] = {0};

// Fatal-startup reporting. The Windows build is linked as a GUI binary so that
// double-clicking it doesn't open a console, which means a printed message would
// go nowhere and a failed startup would look like the exe doing nothing.
static void fatal(const char* what) {
    char msg[256];
    std::snprintf(msg, sizeof(msg), "%s: %s", what, SDL_GetError());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FluidSimulation", msg, nullptr);
}

// WIDTH x HEIGHT is the size the sim is *drawn* at, not a size every display can
// show: on a 1536x864 desktop the 1600x900 client plus its frame is larger than
// the screen in both directions, so the window gets centred on nothing and its
// title bar — with the minimise/maximise/close buttons — ends up above the top
// edge, out of reach. Shrink the window until the whole frame fits inside the
// display's usable area (which already excludes the taskbar) and put it there.
// The 16:9 ratio is preserved so the render still fills the window edge to edge,
// and the sim itself is unaffected: SDL_RenderSetLogicalSize scales it to
// whatever the window ends up being.
static void fit_window_to_display(SDL_Window* window) {
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(window), &usable) != 0)
        return;

    // Frame thickness isn't known until the window exists, and isn't reported on
    // every platform; the fallback is a typical desktop-window border.
    int top = 32, left = 8, bottom = 8, right = 8;
    SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right);

    const int maxW = usable.w - left - right;
    const int maxH = usable.h - top - bottom;
    if (maxW <= 0 || maxH <= 0)
        return;

    int w = WIDTH, h = HEIGHT;
    if (w > maxW || h > maxH) {
        const float scale = SDL_min((float)maxW / WIDTH, (float)maxH / HEIGHT);
        w = (int)(WIDTH * scale);
        h = (int)(HEIGHT * scale);
        SDL_SetWindowSize(window, w, h);
    }

    // Position is the client area's top-left, so the frame's own top-left is
    // (x - left, y - top): offsetting by the border keeps the title bar on screen.
    SDL_SetWindowPosition(window,
                          usable.x + left + (maxW - w) / 2,
                          usable.y + top + (maxH - h) / 2);
}

static const char* mode_name(DrawMode m) {
    switch (m) {
        case MODE_SOLID: return "SOLID";
        case MODE_SAND:  return "SAND";
        case MODE_FLUID: return "FLUID";
        case MODE_WIND:  return "WIND";
        case MODE_LAVA:  return "LAVA";
        default:         return "?";
    }
}

int main(int argc, char* argv[]) {
    // The Windows build defines SDL_MAIN_HANDLED so that this stays the real
    // entry point instead of being renamed to SDL_main; SDL then needs to be
    // told the platform setup it would have done itself is accounted for.
    // A no-op everywhere else.
    SDL_SetMainReady();

    std::srand(static_cast<unsigned>(std::time(nullptr)));

    if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
        fatal("Could not initialize SDL");
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "FluidSimulation [SOLID]",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        WIDTH, HEIGHT,
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        fatal("Could not create window");
        SDL_Quit();
        return -1;
    }

    fit_window_to_display(window);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        fatal("Could not create renderer");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    // Static, not a local: at a 5-pixel cell the MAC grid and particle arrays
    // are a few megabytes, which overflows the default stack on some platforms.
    static FluidState fluid;
    fluid_init(fluid);

    InputState input;
    input_init(input);

    renderer_init(renderer);

    int frameCount = 0;
    bool running = true;
    SDL_Event event;
    DrawMode prevMode = input.mode;
    int prevBrush = -1;
    bool prevErase = false;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (!input_handle_event(input, event, fluid))
                running = false;
        }

        // Continuous, position-based erase (E held): runs every frame so
        // material flowing under the cursor is removed without clicking.
        input_update(input, fluid);

        // Update window title when draw mode, brush size, or erase state changes
        if (input.mode != prevMode || input.brushRadius != prevBrush ||
            input.eraseHeld != prevErase) {
            char title[64];
            std::snprintf(title, sizeof(title), "FluidSimulation [%s]  brush:%d",
                          input.eraseHeld ? "ERASE" : mode_name(input.mode),
                          input.brushRadius);
            SDL_SetWindowTitle(window, title);
            prevMode = input.mode;
            prevBrush = input.brushRadius;
            prevErase = input.eraseHeld;
        }

        frameCount++;
        // Lava first: it resolves melts and freezes before sand decides what it
        // can fall into, so a grain never lands in a cell that is about to
        // become rock underneath it.
        lava_update(fluid, frameCount, 0.1f);
        sand_update(fluid, frameCount, 0.1f);
        fluid_step(fluid, 0.1f);

        renderer_draw(renderer, fluid, input.showGrid);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    renderer_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
