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
- **Metaball rendering.** Water and sand are splatted into a half-resolution blob field and
  thresholded for a smooth liquid/granular surface. Water is shaded by depth (deeper = darker)
  and speed (fast = foamy), and sand darkens when wet, drying out slowly over time.

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
| **Space** | Cycle draw mode: `SOLID` → `SAND` → `FLUID` |
| **Hold E** | Erase: while held, cells under the cursor are continuously rubbed out (solids, sand, and any water that flows in) — no clicking needed |
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
| `input.h` / `input.cpp` | Mouse/keyboard handling and draw modes |
| `renderer.h` / `renderer.cpp` | Metaball splatting and shading |
| `fluidSimulation.cpp` | Earlier single-file prototype (not part of the `make` build) |
