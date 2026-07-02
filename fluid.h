#pragma once
#include "grid.h"
#include <vector>

// PIC/FLIP free-surface liquid solver.
// Marker particles carry the fluid; the MAC grid carries velocity for the
// pressure solve. Positions are in grid-cell units, velocities in cells/sec.

struct Particle {
    float x, y;
    float vx, vy;
};

// MAC grid: u lives on vertical faces, v on horizontal faces.
const int NU = (GRID_WIDTH + 1) * GRID_HEIGHT;
const int NV = GRID_WIDTH * (GRID_HEIGHT + 1);
const int NC = GRID_WIDTH * GRID_HEIGHT;

// Per-cell material, rebuilt from particles + obstacle grid every step.
enum MatType { CELL_AIR = 0, CELL_FLUID = 1, CELL_SOLID = 2 };

struct FluidState {
    std::vector<Particle> particles;

    float u[NU], v[NV];        // MAC velocities
    float uOld[NU], vOld[NV];  // pre-force snapshot for the FLIP delta
    float uTmp[NU], vTmp[NV];  // P2G weights, then FLIP deltas
    unsigned char uValid[NU], vValid[NV];

    float p[NC];
    float rhs[NC];
    unsigned char cell[NC];    // MatType
    int count[NC];             // particles per cell

    float dens[TOTAL];         // render-only density, padded IX layout
};

void fluid_init(FluidState& state);
void fluid_reset(FluidState& state);
void fluid_step(FluidState& state, float dt);
void fluid_add_density(FluidState& state, int x, int y, float amount);
void fluid_add_velocity(FluidState& state, int x, int y, float vx, float vy);
// Remove every fluid particle currently inside cell (x, y).
void fluid_remove(FluidState& state, int x, int y);
// Shift every particle in cell (x, y) into an adjacent open cell (sideways
// preferred), e.g. before stamping solid material over them. Particles stay
// put if all four neighbors are blocked.
void fluid_displace(FluidState& state, int x, int y);
