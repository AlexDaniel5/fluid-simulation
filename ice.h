#pragma once
#include "fluid.h"

// Rigid falling solid. Ice is the counterpart to sand: both fall under gravity,
// but a grain slides off a pile and an ice block does not — supported ice simply
// stops, so it stacks into vertical columns and holds whatever shape it lands in.
//
// It also floats. Water occupancy is read straight out of FluidState (the solver
// already classifies it every step) and counted as support, so a block dropped
// into a pool rides the surface instead of sinking; drain the pool and it falls
// again on its own.
//
// Ice now carries its own heat state, the counterpart to lava's: every block
// holds an absorbed-heat total in [0, ICE_FUSION]. Lava touching a block conducts
// heat into it (lava owns the transfer; see lava.cpp), the block accumulates it,
// and once it passes the latent-heat threshold the cell melts back into water —
// resolved here, in ice_update, which is where FluidState is in hand. A block
// that stops being heated sheds what it absorbed and re-freezes, so a flow that
// separates from a sheet doesn't keep melting it from a distance.
//
// Call once per frame, after lava_update and before sand_update.
void ice_update(FluidState& fluid, int frame, float dt);
// Stamp a block. Refuses anything that isn't open space, so a stroke pours
// around walls, rock, sand and blocks already placed instead of dissolving them.
void ice_add(int x, int y);
// Lava's half of the melt: pour `amount` of heat into the ice at (x, y). Silently
// ignores non-ice cells, so a stale coordinate costs nothing.
void ice_add_heat(int x, int y, float amount);
// Melt progress of the block at (x, y) in [0,1] — absorbed heat over the fusion
// threshold. Zero for solid ice and for anything that isn't ice, so the renderer
// can tint a block from crystal toward slush as it climbs.
float ice_melt_progress(int x, int y);
