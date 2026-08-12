#pragma once

// 16:9, so scaling up to a fullscreen 16:9 display leaves no letterbox bars.
// The window is not all simulation any more: the tool menu takes a strip off the
// left edge and the sim gets what is left.
const int WIDTH = 1600, HEIGHT = 900;
const int CELL_SIZE = 5;

// Tool menu sidebar, along the left edge, in window pixels.
//
// 160 is not arbitrary — it is the width that leaves no rounding anywhere
// downstream. The sim gets 1440, which is exactly 288 cells at a 5-pixel cell,
// exactly 720 wide at the renderer's half-resolution metaball field, and exactly
// 180 at the eighth-resolution glow field, whose 8x upscale then lands back on
// 1440 on the nose. A width that is not a multiple of 40 leaves one of those
// fields a fraction of a cell short of the sim's right edge.
const int PANEL_W = 160;
const int SIM_W = WIDTH - PANEL_W;

const int GRID_WIDTH = SIM_W / CELL_SIZE;
const int GRID_HEIGHT = HEIGHT / CELL_SIZE;

// The physics and shading constants are authored in cell units (cells/sec,
// cells/sec^2, depth in cells) against a 10-pixel cell. Multiplying them by
// CELL_SCALE keeps them fixed in *screen* terms, so shrinking CELL_SIZE pulls
// the camera back — more world in the same window — instead of changing how
// fast everything falls and flows.
const int REF_CELL_SIZE = 10;
const float CELL_SCALE = (float)REF_CELL_SIZE / (float)CELL_SIZE;
const int PADDED_W = GRID_WIDTH + 2;
const int PADDED_H = GRID_HEIGHT + 2;
const int TOTAL = PADDED_W * PADDED_H;

#define IX(x, y) ((x) + PADDED_W * (y))

// LAVA is molten and moves; ROCK is what it freezes into. ROCK behaves exactly
// like SOLID everywhere in the physics — it exists so that land you poured is
// visually distinguishable from a wall you drew.
//
// ICE is the one rigid material that moves: it falls under gravity like sand but
// never slides or slumps, so it stacks into vertical columns, and it rides on
// water rather than sinking through it.
enum CellType { EMPTY = 0, SOLID = 1, SAND = 2, LAVA = 3, ROCK = 4, ICE = 5 };

extern int grid[GRID_HEIGHT][GRID_WIDTH];
