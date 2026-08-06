#include "fluid.h"
#include "wind.h"
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <algorithm>

namespace {

const float GRAVITY       = 25.0f * CELL_SCALE;  // cells/s^2, uniform body force
const float FLIP_RATIO    = 0.9f;   // 1 = pure FLIP, 0 = pure PIC
const int   PRESSURE_ITER = 150;
const int   EXTRAP_LAYERS = 4;
const int   SEED_PER_CELL = 4;
const float PARTICLE_MASS = 0.3f;   // render-density contribution
const float MAX_SPEED     = 80.0f * CELL_SCALE;  // cells/s
const float REST_DENSITY  = (float)SEED_PER_CELL;  // mass per interior cell
const float DRIFT_STIFF   = 3.0f;   // density-error feedback into pressure rhs
const float DRIFT_MAX     = 2.0f;   // cap on density excess fed to drift, so a
                                    // cell crammed by displaced water (e.g. sand
                                    // placed into a pool) decompresses smoothly
                                    // instead of erupting upward
const int   PUSH_ITER     = 2;
const float PUSH_DIST     = 0.45f;  // min particle spacing (4/cell => 0.5)
const float WIND_DRAG     = 5.0f;   // 1/s, how hard moving air grabs the surface

inline int cidx(int i, int j) { return i + GRID_WIDTH * j; }
inline int uidx(int i, int j) { return i + (GRID_WIDTH + 1) * j; }
inline int vidx(int i, int j) { return i + GRID_WIDTH * j; }

// Out-of-domain counts as solid; SAND, LAVA and ROCK all block liquid like a
// wall. Lava is dense enough that water sits on it rather than seeping through,
// and it quenches on contact anyway.
inline bool solid_cell(int i, int j) {
    if (i < 0 || i >= GRID_WIDTH || j < 0 || j >= GRID_HEIGHT) return true;
    int c = grid[j][i];
    return c == SOLID || c == SAND || c == LAVA || c == ROCK;
}

inline int cell_type(const FluidState& s, int i, int j) {
    if (i < 0 || i >= GRID_WIDTH || j < 0 || j >= GRID_HEIGHT) return CELL_SOLID;
    return s.cell[cidx(i, j)];
}

inline float jitter() { return (float)std::rand() / (float)RAND_MAX; }

// Bilinear sample of an nx*ny array at (gx, gy) in its own index space.
float bilerp(const float* f, int nx, int ny, float gx, float gy) {
    gx = std::max(0.0f, std::min(gx, nx - 1.001f));
    gy = std::max(0.0f, std::min(gy, ny - 1.001f));
    int i = (int)gx, j = (int)gy;
    float fx = gx - i, fy = gy - j;
    const float* r0 = f + j * nx + i;
    const float* r1 = r0 + nx;
    return (1 - fx) * (1 - fy) * r0[0] + fx * (1 - fy) * r0[1]
         + (1 - fx) * fy       * r1[0] + fx * fy       * r1[1];
}

// u faces sit at (i, j+0.5), v faces at (i+0.5, j), in cell units.
float sample_u(const float* u, float x, float y) {
    return bilerp(u, GRID_WIDTH + 1, GRID_HEIGHT, x, y - 0.5f);
}
float sample_v(const float* v, float x, float y) {
    return bilerp(v, GRID_WIDTH, GRID_HEIGHT + 1, x - 0.5f, y);
}

void scatter(float* f, float* w, int nx, int ny, float gx, float gy, float val) {
    gx = std::max(0.0f, std::min(gx, nx - 1.001f));
    gy = std::max(0.0f, std::min(gy, ny - 1.001f));
    int i = (int)gx, j = (int)gy;
    float fx = gx - i, fy = gy - j;
    int b0 = j * nx + i, b1 = b0 + nx;
    float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
    float w01 = (1 - fx) * fy,       w11 = fx * fy;
    f[b0]     += w00 * val;  w[b0]     += w00;
    f[b0 + 1] += w10 * val;  w[b0 + 1] += w10;
    f[b1]     += w01 * val;  w[b1]     += w01;
    f[b1 + 1] += w11 * val;  w[b1 + 1] += w11;
}

// Move a particle to (nx, ny), blocked axis-by-axis at solid cells and walls.
void move_particle(Particle& pt, float nx, float ny) {
    const float eps = 0.001f;
    nx = std::max(eps, std::min(nx, GRID_WIDTH - eps));
    ny = std::max(eps, std::min(ny, GRID_HEIGHT - eps));
    if (!solid_cell((int)nx, (int)pt.y)) pt.x = nx;
    if (!solid_cell((int)pt.x, (int)ny)) pt.y = ny;
}

// RK2 advection through the grid field, substepped to ~1 cell of travel.
void advect_particles(FluidState& s, float dt) {
    float maxs = 0.0f;
    for (int k = 0; k < NU; ++k) maxs = std::max(maxs, std::fabs(s.u[k]));
    for (int k = 0; k < NV; ++k) maxs = std::max(maxs, std::fabs(s.v[k]));
    int sub = std::min((int)(maxs * dt) + 1, 8);
    float sdt = dt / sub;

    for (int step = 0; step < sub; ++step) {
        for (Particle& pt : s.particles) {
            float mx = pt.x + 0.5f * sdt * sample_u(s.u, pt.x, pt.y);
            float my = pt.y + 0.5f * sdt * sample_v(s.v, pt.x, pt.y);
            float nx = pt.x + sdt * sample_u(s.u, mx, my);
            float ny = pt.y + sdt * sample_v(s.v, mx, my);
            move_particle(pt, nx, ny);
        }
    }

    // If a solid was drawn on top of a particle, pop it out upward.
    for (Particle& pt : s.particles) {
        if (!solid_cell((int)pt.x, (int)pt.y)) continue;
        for (int j = (int)pt.y; j >= 0; --j) {
            if (!solid_cell((int)pt.x, j)) { pt.y = j + 0.5f; break; }
        }
    }
}

// Relax particle clumping: pairs closer than PUSH_DIST get separated, which
// keeps the marker distribution (and thus volume) roughly uniform.
void push_particles_apart(FluidState& s) {
    static std::vector<int> head, nxt;
    head.assign(NC, -1);
    nxt.assign(s.particles.size(), -1);
    for (int k = 0; k < (int)s.particles.size(); ++k) {
        const Particle& pt = s.particles[k];
        int i = std::min((int)pt.x, GRID_WIDTH - 1);
        int j = std::min((int)pt.y, GRID_HEIGHT - 1);
        nxt[k] = head[cidx(i, j)];
        head[cidx(i, j)] = k;
    }

    const float minDist2 = PUSH_DIST * PUSH_DIST;
    for (int it = 0; it < PUSH_ITER; ++it) {
        for (int k = 0; k < (int)s.particles.size(); ++k) {
            Particle& a = s.particles[k];
            int ci = std::min((int)a.x, GRID_WIDTH - 1);
            int cj = std::min((int)a.y, GRID_HEIGHT - 1);
            for (int j = std::max(cj - 1, 0); j <= std::min(cj + 1, GRID_HEIGHT - 1); ++j) {
                for (int i = std::max(ci - 1, 0); i <= std::min(ci + 1, GRID_WIDTH - 1); ++i) {
                    for (int q = head[cidx(i, j)]; q != -1; q = nxt[q]) {
                        if (q == k) continue;
                        Particle& b = s.particles[q];
                        float dx = b.x - a.x, dy = b.y - a.y;
                        float d2 = dx * dx + dy * dy;
                        if (d2 >= minDist2 || d2 < 1e-12f) continue;
                        float d = std::sqrt(d2);
                        float push = 0.5f * (PUSH_DIST - d) / d;
                        dx *= push;
                        dy *= push;
                        move_particle(a, a.x - dx, a.y - dy);
                        move_particle(b, b.x + dx, b.y + dy);
                    }
                }
            }
        }
    }
}

void classify_cells(FluidState& s) {
    for (int j = 0; j < GRID_HEIGHT; ++j)
        for (int i = 0; i < GRID_WIDTH; ++i) {
            int c = cidx(i, j);
            s.cell[c] = solid_cell(i, j) ? CELL_SOLID : CELL_AIR;
            s.count[c] = 0;
        }
    for (const Particle& pt : s.particles) {
        int i = std::min((int)pt.x, GRID_WIDTH - 1);
        int j = std::min((int)pt.y, GRID_HEIGHT - 1);
        int c = cidx(i, j);
        if (s.cell[c] != CELL_SOLID) {
            s.cell[c] = CELL_FLUID;
            s.count[c]++;
        }
    }
}

void particles_to_grid(FluidState& s) {
    memset(s.u, 0, sizeof(s.u));
    memset(s.v, 0, sizeof(s.v));
    memset(s.uTmp, 0, sizeof(s.uTmp));
    memset(s.vTmp, 0, sizeof(s.vTmp));
    for (const Particle& pt : s.particles) {
        scatter(s.u, s.uTmp, GRID_WIDTH + 1, GRID_HEIGHT, pt.x, pt.y - 0.5f, pt.vx);
        scatter(s.v, s.vTmp, GRID_WIDTH, GRID_HEIGHT + 1, pt.x - 0.5f, pt.y, pt.vy);
    }
    for (int k = 0; k < NU; ++k) {
        s.uValid[k] = s.uTmp[k] > 1e-6f;
        s.u[k] = s.uValid[k] ? s.u[k] / s.uTmp[k] : 0.0f;
    }
    for (int k = 0; k < NV; ++k) {
        s.vValid[k] = s.vTmp[k] > 1e-6f;
        s.v[k] = s.vValid[k] ? s.v[k] / s.vTmp[k] : 0.0f;
    }
}

// Grow valid face values outward layer by layer (average of valid neighbors),
// so particles just outside the liquid sample a sensible velocity.
//
// EXTRAP_LAYERS passes can only carry a value that far from an already-valid
// face, so faces beyond that margin are untouched however large the domain is.
// Scanning just the band keeps this proportional to the liquid rather than to
// the grid, which matters once the canvas is much bigger than the water on it.
void extrapolate(float* f, unsigned char* valid, int nx, int ny) {
    int loX = nx, hiX = -1, loY = ny, hiY = -1;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            if (valid[i + nx * j]) {
                if (i < loX) loX = i;
                if (i > hiX) hiX = i;
                if (j < loY) loY = j;
                if (j > hiY) hiY = j;
            }
    if (hiX < 0) return;  // nothing valid to grow outward from
    loX = std::max(0, loX - EXTRAP_LAYERS);
    hiX = std::min(nx - 1, hiX + EXTRAP_LAYERS);
    loY = std::max(0, loY - EXTRAP_LAYERS);
    hiY = std::min(ny - 1, hiY + EXTRAP_LAYERS);

    static std::vector<unsigned char> next;
    next.assign(valid, valid + nx * ny);
    for (int layer = 0; layer < EXTRAP_LAYERS; ++layer) {
        for (int j = loY; j <= hiY; ++j) {
            for (int i = loX; i <= hiX; ++i) {
                int k = i + nx * j;
                if (valid[k]) continue;
                float sum = 0.0f;
                int n = 0;
                if (i > 0      && valid[k - 1])  { sum += f[k - 1];  ++n; }
                if (i < nx - 1 && valid[k + 1])  { sum += f[k + 1];  ++n; }
                if (j > 0      && valid[k - nx]) { sum += f[k - nx]; ++n; }
                if (j < ny - 1 && valid[k + nx]) { sum += f[k + nx]; ++n; }
                if (n > 0) { f[k] = sum / n; next[k] = 1; }
            }
        }
        // Only band rows can have changed, so only they need copying back.
        memcpy(valid + loY * nx, next.data() + loY * nx, (hiY - loY + 1) * nx);
    }
}

void apply_gravity(FluidState& s, float dt) {
    for (int j = 0; j <= GRID_HEIGHT; ++j)
        for (int i = 0; i < GRID_WIDTH; ++i)
            if (cell_type(s, i, j - 1) == CELL_FLUID || cell_type(s, i, j) == CELL_FLUID)
                s.v[vidx(i, j)] += GRAVITY * dt;
}

// A fluid cell touching air feels the wind; one buried inside the liquid is
// shielded by the water between it and the surface.
inline bool exposed_fluid(const FluidState& s, int i, int j) {
    if (cell_type(s, i, j) != CELL_FLUID) return false;
    return cell_type(s, i - 1, j) == CELL_AIR || cell_type(s, i + 1, j) == CELL_AIR
        || cell_type(s, i, j - 1) == CELL_AIR || cell_type(s, i, j + 1) == CELL_AIR;
}

// Wind drags the liquid wherever it is in contact with air: the faces of
// surface cells and of airborne droplets relax toward the local air velocity.
// It goes in as a body force before the projection, so a gust across a pool
// becomes a surface wave the pressure solve resolves, not a rigid shove.
void apply_wind(FluidState& s, float dt) {
    if (!wind_active()) return;
    float k = std::min(WIND_DRAG * dt, 1.0f);
    for (int j = 0; j < GRID_HEIGHT; ++j)
        for (int i = 0; i <= GRID_WIDTH; ++i) {
            float w = 0.5f * (wind_u(i - 1, j) + wind_u(i, j));
            // Still air exerts no drag: only gusted faces are touched, so a
            // gust in one corner can't brake the free surface everywhere else.
            if (std::fabs(w) < WIND_EPS) continue;
            if (!exposed_fluid(s, i - 1, j) && !exposed_fluid(s, i, j)) continue;
            s.u[uidx(i, j)] += k * (w - s.u[uidx(i, j)]);
        }
    for (int j = 0; j <= GRID_HEIGHT; ++j)
        for (int i = 0; i < GRID_WIDTH; ++i) {
            float w = 0.5f * (wind_v(i, j - 1) + wind_v(i, j));
            if (std::fabs(w) < WIND_EPS) continue;
            if (!exposed_fluid(s, i, j - 1) && !exposed_fluid(s, i, j)) continue;
            s.v[vidx(i, j)] += k * (w - s.v[vidx(i, j)]);
        }
}

// Zero the normal velocity on every face touching a solid cell (free-slip).
void enforce_solid_faces(FluidState& s) {
    for (int j = 0; j < GRID_HEIGHT; ++j)
        for (int i = 0; i <= GRID_WIDTH; ++i)
            if (cell_type(s, i - 1, j) == CELL_SOLID || cell_type(s, i, j) == CELL_SOLID)
                s.u[uidx(i, j)] = 0.0f;
    for (int j = 0; j <= GRID_HEIGHT; ++j)
        for (int i = 0; i < GRID_WIDTH; ++i)
            if (cell_type(s, i, j - 1) == CELL_SOLID || cell_type(s, i, j) == CELL_SOLID)
                s.v[vidx(i, j)] = 0.0f;
}

// One cell's Gauss-Seidel row, built once per solve: the divisor (its count of
// non-solid neighbors) and the fluid neighbors whose pressure it averages.
struct PRow {
    int c;
    int diag;
    int nn;      // fluid neighbors in nb
    int nb[4];
};

// Incompressibility on fluid cells only. Air cells hold p = 0 (free surface);
// solid neighbors drop out of the stencil (zero normal flux). dt/rho/h^2 are
// folded into p, so the gradient subtraction below is a plain difference.
//
// The sweeps walk a list of the fluid cells rather than the whole domain, so
// the solve costs what the water costs and an empty screen is free. Hoisting
// the material tests into that list also lifts every bounds check out of the
// inner loop. Same cells, same order, same arithmetic as sweeping the grid.
void solve_pressure(FluidState& s) {
    memset(s.p, 0, sizeof(s.p));

    static std::vector<PRow> rows;
    rows.clear();
    for (int j = 0; j < GRID_HEIGHT; ++j) {
        for (int i = 0; i < GRID_WIDTH; ++i) {
            int c = cidx(i, j);
            if (s.cell[c] != CELL_FLUID) { s.rhs[c] = 0.0f; continue; }
            s.rhs[c] = -(s.u[uidx(i + 1, j)] - s.u[uidx(i, j)]
                       + s.v[vidx(i, j + 1)] - s.v[vidx(i, j)]);
            // Drift compensation: over-dense cells demand net outflow, which
            // decompresses particle pile-ups the plain solve can't see.
            float density = s.dens[IX(i + 1, j + 1)] / PARTICLE_MASS;
            if (density > REST_DENSITY)
                s.rhs[c] += DRIFT_STIFF * std::min(density - REST_DENSITY, DRIFT_MAX);

            PRow r;
            r.c = c;
            r.diag = 0;
            r.nn = 0;
            int tl = cell_type(s, i - 1, j);
            if (tl != CELL_SOLID) { ++r.diag; if (tl == CELL_FLUID) r.nb[r.nn++] = c - 1; }
            int tr = cell_type(s, i + 1, j);
            if (tr != CELL_SOLID) { ++r.diag; if (tr == CELL_FLUID) r.nb[r.nn++] = c + 1; }
            int tu = cell_type(s, i, j - 1);
            if (tu != CELL_SOLID) { ++r.diag; if (tu == CELL_FLUID) r.nb[r.nn++] = c - GRID_WIDTH; }
            int td = cell_type(s, i, j + 1);
            if (td != CELL_SOLID) { ++r.diag; if (td == CELL_FLUID) r.nb[r.nn++] = c + GRID_WIDTH; }
            if (r.diag > 0) rows.push_back(r);
        }
    }

    int n = (int)rows.size();
    for (int it = 0; it < PRESSURE_ITER; ++it) {
        // Alternate sweep direction so information propagates both ways.
        bool fwd = (it & 1) == 0;
        for (int k = 0; k < n; ++k) {
            const PRow& r = rows[fwd ? k : n - 1 - k];
            float sum = 0.0f;
            for (int q = 0; q < r.nn; ++q) sum += s.p[r.nb[q]];
            s.p[r.c] = (s.rhs[r.c] + sum) / r.diag;
        }
    }

    // Subtract the pressure gradient across fluid/fluid and fluid/air faces.
    for (int j = 0; j < GRID_HEIGHT; ++j) {
        for (int i = 1; i < GRID_WIDTH; ++i) {
            int tl = cell_type(s, i - 1, j), tr = cell_type(s, i, j);
            if (tl == CELL_SOLID || tr == CELL_SOLID) { s.u[uidx(i, j)] = 0.0f; continue; }
            if (tl == CELL_FLUID || tr == CELL_FLUID)
                s.u[uidx(i, j)] -= s.p[cidx(i, j)] - s.p[cidx(i - 1, j)];
        }
    }
    for (int j = 1; j < GRID_HEIGHT; ++j) {
        for (int i = 0; i < GRID_WIDTH; ++i) {
            int tu = cell_type(s, i, j - 1), td = cell_type(s, i, j);
            if (tu == CELL_SOLID || td == CELL_SOLID) { s.v[vidx(i, j)] = 0.0f; continue; }
            if (tu == CELL_FLUID || td == CELL_FLUID)
                s.v[vidx(i, j)] -= s.p[cidx(i, j)] - s.p[cidx(i, j - 1)];
        }
    }
}

void splat_density(FluidState& s) {
    memset(s.dens, 0, sizeof(s.dens));
    for (const Particle& pt : s.particles) {
        float gx = std::max(0.0f, std::min(pt.x - 0.5f, GRID_WIDTH - 1.001f));
        float gy = std::max(0.0f, std::min(pt.y - 0.5f, GRID_HEIGHT - 1.001f));
        int i = (int)gx, j = (int)gy;
        float fx = gx - i, fy = gy - j;
        s.dens[IX(i + 1, j + 1)]     += (1 - fx) * (1 - fy) * PARTICLE_MASS;
        s.dens[IX(i + 2, j + 1)]     += fx * (1 - fy)       * PARTICLE_MASS;
        s.dens[IX(i + 1, j + 2)]     += (1 - fx) * fy       * PARTICLE_MASS;
        s.dens[IX(i + 2, j + 2)]     += fx * fy             * PARTICLE_MASS;
    }
}

} // namespace

// ---- public API ----

void fluid_init(FluidState& s) {
    s.particles.reserve(SEED_PER_CELL * NC);
    fluid_reset(s);
}

void fluid_reset(FluidState& s) {
    s.particles.clear();
    memset(s.u, 0, sizeof(s.u));
    memset(s.v, 0, sizeof(s.v));
    memset(s.uOld, 0, sizeof(s.uOld));
    memset(s.vOld, 0, sizeof(s.vOld));
    memset(s.uTmp, 0, sizeof(s.uTmp));
    memset(s.vTmp, 0, sizeof(s.vTmp));
    memset(s.uValid, 0, sizeof(s.uValid));
    memset(s.vValid, 0, sizeof(s.vValid));
    memset(s.p, 0, sizeof(s.p));
    memset(s.rhs, 0, sizeof(s.rhs));
    memset(s.cell, 0, sizeof(s.cell));
    memset(s.count, 0, sizeof(s.count));
    memset(s.dens, 0, sizeof(s.dens));
}

void fluid_add_density(FluidState& s, int x, int y, float) {
    if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) return;
    if (solid_cell(x, y)) return;
    int c = cidx(x, y);
    while (s.count[c] < SEED_PER_CELL) {
        Particle pt;
        pt.x = x + 0.1f + 0.8f * jitter();
        pt.y = y + 0.1f + 0.8f * jitter();
        pt.vx = pt.vy = 0.0f;
        s.particles.push_back(pt);
        s.count[c]++;
    }
}

void fluid_remove(FluidState& s, int x, int y) {
    if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) return;
    // Compact in place, dropping any particle whose cell is (x, y).
    size_t w = 0;
    for (size_t r = 0; r < s.particles.size(); ++r) {
        if ((int)s.particles[r].x == x && (int)s.particles[r].y == y) continue;
        s.particles[w++] = s.particles[r];
    }
    s.particles.resize(w);
}

void fluid_displace(FluidState& s, int x, int y) {
    if (x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT) return;
    // Prefer sideways, then down, then up — pushing water above the stamped
    // cell is the geyser direction and only happens when boxed in.
    int side = (std::rand() & 1) ? 1 : -1;
    const int off[4][2] = {{side, 0}, {-side, 0}, {0, 1}, {0, -1}};
    int tx = x, ty = y;
    for (int k = 0; k < 4; ++k) {
        int cx = x + off[k][0], cy = y + off[k][1];
        if (cx < 0 || cx >= GRID_WIDTH || cy < 0 || cy >= GRID_HEIGHT) continue;
        if (grid[cy][cx] == EMPTY) { tx = cx; ty = cy; break; }
    }
    if (tx == x && ty == y) return;  // fully enclosed; advection pops them out
    for (Particle& pt : s.particles) {
        if ((int)pt.x == x && (int)pt.y == y) {
            pt.x += (float)(tx - x);
            pt.y += (float)(ty - y);
        }
    }
}

void fluid_add_velocity(FluidState& s, int x, int y, float vx, float vy) {
    float cx = x + 0.5f, cy = y + 0.5f;
    const float r2 = 1.5f * 1.5f;
    for (Particle& pt : s.particles) {
        float dx = pt.x - cx, dy = pt.y - cy;
        if (dx * dx + dy * dy < r2) {
            pt.vx += vx;
            pt.vy += vy;
        }
    }
}

void fluid_step(FluidState& s, float dt) {
    if (s.particles.empty()) {
        // No liquid: there is nothing to project, but the material grid still
        // has to be reclassified so a wall drawn this frame is visible to the
        // other systems, and the velocity/density fields must read as empty.
        classify_cells(s);
        memset(s.u, 0, sizeof(s.u));
        memset(s.v, 0, sizeof(s.v));
        memset(s.dens, 0, sizeof(s.dens));
        return;
    }

    advect_particles(s, dt);
    push_particles_apart(s);
    classify_cells(s);
    splat_density(s);  // feeds both the renderer and drift compensation
    particles_to_grid(s);
    extrapolate(s.u, s.uValid, GRID_WIDTH + 1, GRID_HEIGHT);
    extrapolate(s.v, s.vValid, GRID_WIDTH, GRID_HEIGHT + 1);

    memcpy(s.uOld, s.u, sizeof(s.u));
    memcpy(s.vOld, s.v, sizeof(s.v));

    apply_gravity(s, dt);
    apply_wind(s, dt);
    enforce_solid_faces(s);
    solve_pressure(s);
    enforce_solid_faces(s);

    // Projection only touched faces near liquid; extrapolate the result so
    // the FLIP delta and next frame's advection are smooth at the surface.
    for (int j = 0; j < GRID_HEIGHT; ++j)
        for (int i = 0; i <= GRID_WIDTH; ++i)
            s.uValid[uidx(i, j)] =
                cell_type(s, i - 1, j) == CELL_FLUID || cell_type(s, i, j) == CELL_FLUID;
    for (int j = 0; j <= GRID_HEIGHT; ++j)
        for (int i = 0; i < GRID_WIDTH; ++i)
            s.vValid[vidx(i, j)] =
                cell_type(s, i, j - 1) == CELL_FLUID || cell_type(s, i, j) == CELL_FLUID;
    extrapolate(s.u, s.uValid, GRID_WIDTH + 1, GRID_HEIGHT);
    extrapolate(s.v, s.vValid, GRID_WIDTH, GRID_HEIGHT + 1);

    for (int k = 0; k < NU; ++k) s.uTmp[k] = s.u[k] - s.uOld[k];
    for (int k = 0; k < NV; ++k) s.vTmp[k] = s.v[k] - s.vOld[k];

    // PIC/FLIP blend back to particles.
    for (Particle& pt : s.particles) {
        float picU = sample_u(s.u, pt.x, pt.y);
        float picV = sample_v(s.v, pt.x, pt.y);
        float flipU = pt.vx + sample_u(s.uTmp, pt.x, pt.y);
        float flipV = pt.vy + sample_v(s.vTmp, pt.x, pt.y);
        pt.vx = (1.0f - FLIP_RATIO) * picU + FLIP_RATIO * flipU;
        pt.vy = (1.0f - FLIP_RATIO) * picV + FLIP_RATIO * flipV;
        float sp = std::sqrt(pt.vx * pt.vx + pt.vy * pt.vy);
        if (sp > MAX_SPEED) {
            pt.vx *= MAX_SPEED / sp;
            pt.vy *= MAX_SPEED / sp;
        }
    }
}
