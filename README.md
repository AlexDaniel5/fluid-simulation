# FluidSimulation

A real-time 2D fluid, sand, and solid sandbox built in C++ with [SDL2](https://www.libsdl.org/).
Draw obstacles, pour sand, and fling water around an interactive grid — the materials
interact with each other under gravity.

## Features

- **PIC/FLIP free-surface liquid solver.** Water is carried by marker particles, while a
  staggered MAC grid handles the velocity field and pressure projection. This gives splashy,
  incompressible liquid with a real free surface (not just a diffused density field).
- **Cellular-automaton sand** coupled to the liquid. Grains fall under gravity (terminal-capped,
  and slower underwater) and *displace* water rather than destroying it — water swaps into the
  cell a grain vacates. Displaced water also inherits part of the grain's fall momentum, so
  fast sand splashes into a pool and sinking grains churn the water around them.
- **Static solid obstacles** that both sand and water collide with.
- **Wind.** A brush-painted air-velocity field that *stands* — a gust keeps blowing until you
  erase or clear it, so you can lay down a permanent current and then build in it. Neither
  solver models air, so wind is applied as a drag force on surfaces that are exposed to it:
  the liquid feels it only where it touches air (the free surface and airborne droplets, never
  buried water), and it goes in *before* the pressure projection, so a gust across a pool
  becomes a wave rather than a rigid shove. Dry surface grains roll and saltate downwind, a
  strong updraft plucks them off the pile, and wet sand clings. Solids and sand block a gust,
  so a wall casts a wind shadow.
- **Metaball rendering.** Water and sand are splatted into a half-resolution blob field and
  thresholded for a smooth liquid/granular surface. Water is shaded by depth (deeper = darker)
  and speed (fast = foamy), and sand darkens when wet, drying out slowly over time.

## Simulation size

The window is 1600×1000 with a 5-pixel cell, giving a 320×200 grid. `CELL_SIZE` in `grid.h` is
the zoom control: the physics and shading constants are authored in cell units against a
10-pixel reference cell and scaled by `CELL_SCALE`, so shrinking the cell pulls the camera back
— more world in the same window — without changing how fast anything falls, flows, or shades.
Halving `CELL_SIZE` quadruples the cell count, and the solver cost scales with it, so this is
also the dial to turn if the framerate is too low.

Cost tracks how much water is on screen rather than how big the grid is: the pressure solve
sweeps a list of the fluid cells, velocity extrapolation only scans the band it can reach, and
a screen with no liquid skips the velocity pipeline entirely.

## Building

Requires `g++`, `make`, `pkg-config`, and the SDL2 development libraries.

On Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libsdl2-dev
```

Then build:

```sh
make
```

The binary is produced at `build/fluidSimulation`.

> The `src/include` headers and `build/SDL2.dll` in the repo are a bundled SDL2 copy for
> Windows builds. The `Makefile` itself uses `pkg-config` to locate a system-installed SDL2.

## Running

```sh
./build/fluidSimulation
```

## Controls

| Input | Action |
|-------|--------|
| **Left-click (or drag)** | Draw the currently selected material (or fling water in fluid mode) |
| **Space** | Cycle draw mode: `SOLID` → `SAND` → `FLUID` → `WIND` |
| **Drag in `WIND` mode** | Blow air along the stroke. The jet keeps its heading, so holding still leaves a steady stream; stroke a new direction to re-aim it. The brush radius sets the gust width, and faint streaks show where the air is moving. Painted wind stays put until erased |
| **Hold E** | Erase: while held, cells under the cursor are continuously rubbed out (solids, sand, any water that flows in, and any wind blowing there) — no clicking needed |
| **W** | Still the air everywhere, leaving the scene itself alone |
| **`[`** | Shrink the brush radius |
| **`]`** | Grow the brush radius |
| **G** | Toggle the debug grid-line overlay |
| **R** | Reset the fluid (clears particles, keeps solids/sand) |
| **C** | Clear everything (fluid *and* the obstacle grid) |

The current draw mode is shown in the window title bar.

## Project layout

| File | Responsibility |
|------|----------------|
| `main.cpp` | Window/renderer setup and the main loop |
| `grid.h` | Shared grid dimensions, cell types, and the global obstacle grid |
| `fluid.h` / `fluid.cpp` | PIC/FLIP liquid solver (particles, MAC grid, pressure solve) |
| `sand.h` / `sand.cpp` | Cellular-automaton sand, coupled to the liquid |
| `wind.h` / `wind.cpp` | Decaying air-velocity field read by both solvers |
| `input.h` / `input.cpp` | Mouse/keyboard handling and draw modes |
| `renderer.h` / `renderer.cpp` | Metaball splatting and shading |
| `fluidSimulation.cpp` | Earlier single-file prototype (not part of the `make` build) |
