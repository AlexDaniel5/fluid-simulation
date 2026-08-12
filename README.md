# fluid-simulation

A real-time 2D fluid, sand, lava, ice, and solid sandbox built in C++ with
[SDL2](https://www.libsdl.org/). Draw obstacles, pour sand, fling water, drop ice, and lay down
molten rock on an interactive grid — the materials interact with each other under gravity.

## Quick start

```sh
sudo apt install build-essential pkg-config libsdl2-dev
make
./build/fluid-simulation
```

Then: **Space** cycles the material (`SOLID` → `SAND` → `FLUID` → `WIND` → `LAVA` → `ICE`),
**left-drag** draws it, **hold E** erases, **`[`** / **`]`** resize the brush, and **C** clears the
screen. Try drawing a bowl in `SOLID`, filling it with `FLUID`, then pouring `LAVA` in to build an
island — or float a raft of `ICE` on the water and drop lava on it to melt it back out.

## Features

- **PIC/FLIP free-surface liquid solver.** Water is carried by marker particles, while a
  staggered MAC grid handles the velocity field and pressure projection. This gives splashy,
  incompressible liquid with a real free surface (not just a diffused density field).
- **Cellular-automaton sand** coupled to the liquid. Grains fall under gravity (terminal-capped,
  and slower underwater) and *displace* water rather than destroying it — water swaps into the
  cell a grain vacates. Displaced water also inherits part of the grain's fall momentum, so
  fast sand splashes into a pool and sinking grains churn the water around them.
- **Static solid obstacles** that both sand and water collide with.
- **Lava.** A viscous cellular automaton, and the only material that carries state which
  changes on its own: every molten cell holds a *heat*, and that single number is both its
  viscosity and its clock. Hot lava is runny and self-levels into a sheet; as it dims the same
  code stiffens it into a standing mound, and at zero it sets into rock. Heat escapes only
  through faces touching air, so a thick pour *crusts from the outside in* and keeps a molten
  core long after its skin has hardened. Water is a wall to it rather than something it sinks
  through — lava hisses, freezes on contact and stacks up, which is what lets repeated pours
  build an island out of a pool. It eats through sand too, but pays for each grain out of its
  own heat, so a flow carves a channel into a dune and then sets rather than chain-reacting
  through the whole thing. Set lava becomes rock, which is dark basalt rather than the white of
  a drawn wall, so land you poured is distinguishable from geometry you built.
- **Ice.** The one *rigid* material that moves. It falls under gravity like sand, but a supported
  block simply stops — no diagonal slide, no slumping, no rolling downwind — so ice stacks into
  vertical columns and holds whatever shape it lands in, where sand can only make a 45° pile. It
  also floats: water counts as support, so a block dropped into a pool comes to rest on the
  surface and starts falling again by itself once the water is drained or displaced. Lava melts it
  straight back into water and is quenched hard doing so, so a flow poured onto an ice sheet
  floods a pocket and then sets into rock instead of eating through.
- **Wind.** A brush-painted air-velocity field that *stands* — a gust keeps blowing until you
  erase or clear it, so you can lay down a permanent current and then build in it. Neither
  solver models air, so wind is applied as a drag force on surfaces that are exposed to it:
  the liquid feels it only where it touches air (the free surface and airborne droplets, never
  buried water), and it goes in *before* the pressure projection, so a gust across a pool
  becomes a wave rather than a rigid shove. Dry surface grains roll and saltate downwind, a
  strong updraft plucks them off the pile, and wet sand clings. Moving air also carries heat off
  exposed lava far faster than still air does, so a gust freezes a flow where it stands. Solids,
  sand, ice, lava and rock all block a gust, so a wall casts a wind shadow. Since air has nothing
  to draw of its own, the field is shown twice over: streaks along the flow, which map where the
  current stands, and a pool of tracers advected through it, which is what actually makes the air
  read as moving — a still vector field is easy to miss, a moving one isn't. Tracer count scales
  with how much of the screen is blowing, and both layers batch their lines by brightness so the
  overlay costs a couple of dozen draw-state changes a frame instead of thousands.
- **Metaball rendering.** Water, sand and lava are splatted into a half-resolution blob field and
  thresholded for a smooth liquid/granular surface. Water is shaded by depth (deeper = darker)
  and speed (fast = foamy), and sand darkens when wet, drying out slowly over time. Lava is
  shaded by heat instead — orange through deep red into a dark mottled crust — and is the one
  material that *emits*: a blurred copy of it is added back over the scene, so a flow lights the
  ground and the air around it. That halo is built at an eighth of the window and only over the
  flow's own bounding box, which keeps the two extra passes proportional to how much lava is on
  screen rather than to the size of the grid. Ice deliberately opts out: it is drawn as crisp
  per-cell facets in pale cyan, because the blockiness is what makes it read as crystal next to
  the rounded silhouettes around it.

## Simulation size

The window is 1600×900 with a 5-pixel cell, giving a 320×180 grid. The sim is drawn at that
fixed logical size and scaled to fill the window, so resizing or going fullscreen zooms the
whole simulation instead of leaving the extra area blank — and because 1600×900 is 16:9, a
fullscreen 16:9 display has no letterbox bars. `CELL_SIZE` in `grid.h` is
the zoom control: the physics and shading constants are authored in cell units against a
10-pixel reference cell and scaled by `CELL_SCALE`, so shrinking the cell pulls the camera back
— more world in the same window — without changing how fast anything falls, flows, or shades.
Halving `CELL_SIZE` quadruples the cell count, and the solver cost scales with it, so this is
also the dial to turn if the framerate is too low.

1600×900 is the size the sim is *drawn* at, not a size every desktop can show — with a window
frame around it, it doesn't fit a 1536×864 display, and a window bigger than the screen has its
title bar pushed off the top edge where the close and maximise buttons can't be reached. So at
startup the window is shrunk to fit the display's usable area (16:9 preserved) and placed inside
it. On a large display nothing changes; on a small one you get a smaller window showing the same
world, since the render scales to the window either way.

Cost tracks how much water is on screen rather than how big the grid is: the pressure solve
sweeps a list of the fluid cells, velocity extrapolation only scans the band it can reach, and
a screen with no liquid skips the velocity pipeline entirely.

## Building

### Linux

Requires `g++`, `make`, `pkg-config`, and the SDL2 development libraries.

On Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libsdl2-dev
```

Then build:

```sh
make
```

The binary is produced at `build/fluid-simulation`.

### Windows

`make windows` cross-compiles a Windows executable with MinGW-w64 (this works from WSL too):

```sh
sudo apt install mingw-w64
make windows
```

The executable is produced at `build/fluid-simulation.exe`. It is a GUI-subsystem binary, so
launching it opens the window with no console behind it, and everything except SDL2 is linked
statically — `SDL2.dll` is its only runtime dependency.

> The `src/include` headers and `build/SDL2.dll` in the repo are a bundled SDL2 copy; the
> `windows` target compiles against those headers and links directly against that DLL. The
> default Linux target instead uses `pkg-config` to locate a system-installed SDL2.

## Running

```sh
./build/fluid-simulation
```

On Windows, double-click `build\fluid-simulation.exe` in Explorer. It loads `SDL2.dll` from its
own folder, so the two files have to travel together — copy or move the whole folder rather than
the `.exe` on its own.

## Controls

| Input | Action |
|-------|--------|
| **Left-click (or drag)** | Draw the currently selected material (or fling water in fluid mode) |
| **Space** | Cycle draw mode: `SOLID` → `SAND` → `FLUID` → `WIND` → `LAVA` → `ICE` |
| **Draw in `LAVA` mode** | Pour molten rock. It only stamps into open space — walls and set rock stand, and sand and ice have to be melted through — so the brush can't dissolve terrain for free. Pour it into water to build an island, across a dune to carve a channel, or leave it alone and watch it crust over and set |
| **Draw in `ICE` mode** | Drop solid blocks. They fall straight down and stop the moment something supports them, so ice stacks vertically instead of sliding into a pile, and it rides on water rather than sinking. Like lava, it only stamps into open space |
| **Drag in `WIND` mode** | Blow air along the stroke. The jet keeps its heading, so holding still leaves a steady stream; stroke a new direction to re-aim it. The brush radius sets the gust width, and faint streaks show where the air is moving. Painted wind stays put until erased |
| **Hold E** | Erase: while held, cells under the cursor are continuously rubbed out (solids, sand, ice, lava, rock, any water that flows in, and any wind blowing there) — no clicking needed |
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
| `lava.h` / `lava.cpp` | Viscous cellular-automaton lava: heat, cooling, quenching and melting |
| `ice.h` / `ice.cpp` | Rigid falling ice: gravity, stacking, and floating on water |
| `input.h` / `input.cpp` | Mouse/keyboard handling and draw modes |
| `renderer.h` / `renderer.cpp` | Metaball splatting and shading |
| `fluidSimulation.cpp` | Earlier single-file prototype (not part of the `make` build) |
