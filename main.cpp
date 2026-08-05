#include <iostream>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <SDL.h>
#include "grid.h"
#include "fluid.h"
#include "sand.h"
#include "wind.h"
#include "renderer.h"
#include "input.h"

// Single definition of the shared grid
int grid[GRID_HEIGHT][GRID_WIDTH] = {0};

static const char* mode_name(DrawMode m) {
    switch (m) {
        case MODE_SOLID: return "SOLID";
        case MODE_SAND:  return "SAND";
        case MODE_FLUID: return "FLUID";
        case MODE_WIND:  return "WIND";
        default:         return "?";
    }
}

int main(int argc, char* argv[]) {
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
        std::cout << "Could not initialize SDL: " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "FluidSimulation [SOLID]",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        WIDTH, HEIGHT,
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        std::cout << "Could not create window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return -1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        std::cout << "Could not create renderer: " << SDL_GetError() << std::endl;
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
