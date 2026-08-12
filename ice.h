#pragma once
#include "fluid.h"

// Rigid falling solid. Ice is the counterpart to sand: both fall under gravity,
// but a grain slides off a pile and an ice block does not — supported ice simply
// stops, so it stacks into vertical columns and holds whatever shape it lands in.
//
// It also floats. Water occupancy is read straight out of FluidState (the solver
// already classifies it every step) and counted as support, so a block dropped
// into a pool rides the surface instead of sinking; drain the pool and it falls
// again on its own. Lava melts it back into water — that half lives in lava.cpp,
// which owns the heat it costs.
//
// Call once per frame, after lava_update and before sand_update.
void ice_update(FluidState& fluid, int frame, float dt);
// Stamp a block. Refuses anything that isn't open space, so a stroke pours
// around walls, rock, sand and blocks already placed instead of dissolving them.
void ice_add(int x, int y);
