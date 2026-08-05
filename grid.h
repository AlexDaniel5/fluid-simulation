#pragma once

const int WIDTH = 1600, HEIGHT = 1000;
const int CELL_SIZE = 5;
const int GRID_WIDTH = WIDTH / CELL_SIZE;
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

enum CellType { EMPTY = 0, SOLID = 1, SAND = 2 };

extern int grid[GRID_HEIGHT][GRID_WIDTH];
