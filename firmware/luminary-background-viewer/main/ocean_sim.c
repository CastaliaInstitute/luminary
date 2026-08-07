#include "ocean_sim.h"

#include <math.h>
#include <string.h>

#define GRAVITY_MM_S2 9810.0

/* A laplacian larger than this is not a wave any more, it is a discontinuity.
 * Clamping keeps c2*lap inside int32 without a 64-bit multiply and doubles as
 * a steepness limiter at breaking crests. */
#define LAP_CLAMP 32767

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Arithmetic right shift rounds toward negative infinity, which quietly biases
 * the mean downward every step. Truncate toward zero instead. */
static inline int32_t shift_to_zero(int32_t v, unsigned bits)
{
    return v >= 0 ? (v >> bits) : -((-v) >> bits);
}

/* Fixed-point multiply, rounded to nearest with symmetric handling of sign.
 * Truncating toward zero here is not neutral: the error is always opposed to
 * the signal, which acts as amplitude-proportional damping applied per cell
 * per step. Measured on the flat deep test domain, truncation attenuated a
 * propagating swell to 27% over 48 cells; rounding to nearest makes the error
 * zero-mean and the same wave arrives at full height. Rounding toward
 * negative infinity is no better: that bias rectifies into a metres-deep
 * mean-level drift. */
static inline int32_t mul_shift(int32_t a, int32_t b, unsigned bits)
{
    const int64_t p = (int64_t)a * (int64_t)b;
    const int64_t half = (int64_t)1 << (bits - 1u);
    return (int32_t)(p >= 0 ? ((p + half) >> bits) : -(((-p) + half) >> bits));
}

/* Round-to-nearest shift for the height update. See mul_shift: truncation
 * here was the other half of the systematic propagation loss. */
static inline int32_t round_shift(int32_t v, unsigned bits)
{
    const int32_t half = 1 << (bits - 1u);
    return v >= 0 ? ((v + half) >> bits) : -(((-v) + half) >> bits);
}

void ocean_sim_bind(ocean_sim_t *sim, int16_t *h, int16_t *vel,
                    uint8_t *depth, int8_t *normal, uint8_t *foam,
                    uint16_t *damp_residual)
{
    memset(sim, 0, sizeof(*sim));
    sim->h = h;
    sim->vel = vel;
    sim->depth = depth;
    sim->normal = normal;
    sim->foam = foam;
    sim->damp_residual = damp_residual;
    sim->normal_shift = 4;
    sim->shore_normal_deg = 90;
}

void ocean_sim_prepare(ocean_sim_t *sim, uint32_t dx_mm, uint32_t tick_ms,
                       double damping_per_step)
{
    sim->dx_mm = dx_mm ? dx_mm : OCEAN_DX_MM;
    sim->tick_ms = tick_ms ? tick_ms : OCEAN_TICK_MS;

    const double dt = (double)sim->tick_ms / 1000.0;
    const double dx = (double)sim->dx_mm;
    const double scale = dt * dt / (dx * dx);

    for (unsigned code = 0; code < 256u; ++code) {
        const double depth_mm = (double)ocean_depth_mm((uint8_t)code);
        /* c^2 = g*d, expressed as the dimensionless Courant term. */
        double c2 = GRAVITY_MM_S2 * depth_mm * scale;
        /* Explicit 2D stencil is stable to 0.5; clamp so a bad depth field
         * degrades into a slow wave rather than an exploding one. */
        if (c2 > 0.5) c2 = 0.5;
        sim->c2_lut[code] = (uint16_t)(c2 * 65536.0 + 0.5);

        /* Depth-limited breaking uses full wave height H ~= 2 * crest. */
        const double break_crest_mm =
            depth_mm * (double)OCEAN_BREAK_RATIO_PERCENT / 100.0 / 2.0;
        double crest_q411 = break_crest_mm * (double)OCEAN_H_ONE / 1000.0;
        if (crest_q411 > 32767.0) crest_q411 = 32767.0;
        sim->break_h[code] = (int16_t)crest_q411;

        double limit_q411 = depth_mm * (double)OCEAN_H_ONE / 1000.0;
        if (limit_q411 > 32767.0) limit_q411 = 32767.0;
        sim->h_limit[code] = (int16_t)limit_q411;

        /* Mur coefficient from the local Courant number S = c*dt/dx. */
        const double s = sqrt((double)sim->c2_lut[code] / 65536.0);
        sim->mur_k[code] = (int32_t)(((s - 1.0) / (s + 1.0)) * 65536.0);
    }

    /* Leave a scattered-field buffer between the interface and the Mur row. */
    sim->tfsf_row = OCEAN_NY > 32u ? OCEAN_NY - 16u : OCEAN_NY / 2u;
    if (!sim->source_gain_q16) sim->source_gain_q16 = 65536u;

    for (unsigned i = 0; i < 256u; ++i) {
        sim->sin_q14[i] = (int16_t)(sin((double)i * 2.0 * M_PI / 256.0) * 16383.0);
    }

    const uint16_t base = (uint16_t)(damping_per_step * 65536.0 + 0.5);
    for (unsigned y = 0; y < OCEAN_NY; ++y) {
        sim->row_damp_q16[y] = base;
    }
    /* See OCEAN_SPONGE_STRENGTH: disabled by default because the sponge and
     * the forcing share these rows. */
    if (OCEAN_SPONGE_STRENGTH > 0.0) {
        for (unsigned i = 0; i < OCEAN_SPONGE_ROWS && i < OCEAN_NY; ++i) {
            const unsigned y = OCEAN_NY - 1u - i;
            const double t = (double)(OCEAN_SPONGE_ROWS - i) / (double)OCEAN_SPONGE_ROWS;
            const double damp =
                (double)base / 65536.0 * (1.0 - OCEAN_SPONGE_STRENGTH * t * t);
            sim->row_damp_q16[y] = (uint16_t)(damp * 65536.0 + 0.5);
        }
    }
}

void ocean_sim_set_components(ocean_sim_t *sim,
                              const ocean_component_t *comp, unsigned count,
                              int32_t shore_normal_deg)
{
    if (count > 3u) count = 3u;
    sim->comp_count = count;
    sim->shore_normal_deg = shore_normal_deg;

    /* The incident plane wave must satisfy the solver's own dispersion at the
     * interface, which is shallow-water c = sqrt(g*d) -- not deep-water
     * dispersion. Use the mean depth along the TF/SF row. */
    double interface_depth_mm = 0.0;
    unsigned wet = 0u;
    for (unsigned x = 0; x < OCEAN_NX; ++x) {
        const uint8_t code = sim->depth[(size_t)sim->tfsf_row * OCEAN_NX + x];
        if (code != OCEAN_DEPTH_LAND) { interface_depth_mm += ocean_depth_mm(code); wet++; }
    }
    interface_depth_mm = wet ? interface_depth_mm / wet : (double)OCEAN_DEPTH_MAX_MM;

    /* Courant number of the discrete scheme at the interface depth. */
    unsigned iface_code = (unsigned)(interface_depth_mm * 255.0 / OCEAN_DEPTH_MAX_MM + 0.5);
    if (iface_code > 255u) iface_code = 255u;
    const double courant = sqrt((double)sim->c2_lut[iface_code] / 65536.0);

    for (unsigned i = 0; i < count; ++i) {
        sim->comp[i] = comp[i];
        const double period_s = (double)comp[i].period_ms / 1000.0;

        sim->phase_per_tick[i] = period_s > 0.0
            ? (uint32_t)(65536.0 * (double)sim->tick_ms / (double)comp[i].period_ms)
            : 0u;

        /* Only the component of travel along the boundary produces an
         * along-boundary phase gradient; head-on swell has none. */
        const double relative_rad =
            ((double)(comp[i].from_deg - shore_normal_deg)) * M_PI / 180.0;
        const double along = sin(relative_rad);
        const double onshore = cos(relative_rad);

        /* Solve the scheme's OWN dispersion relation for the wavenumber:
         *   sin^2(w*dt/2) = S^2 * [sin^2(kx*dx/2) + sin^2(ky*dx/2)]
         * Injecting a wave built from the continuous relation k = w/c instead
         * leaves a residual the interface cannot cancel -- measured at 52.7%
         * of incident amplitude leaking into the scattered-field region. */
        const double omega_dt = 2.0 * M_PI * (double)sim->tick_ms / (double)comp[i].period_ms;
        const double target = sin(omega_dt / 2.0) * sin(omega_dt / 2.0)
                              / (courant * courant);
        double lo = 0.0, hi = M_PI; /* k*dx, capped at the Nyquist limit */
        for (unsigned it = 0; it < 60u; ++it) {
            const double mid = 0.5 * (lo + hi);
            const double sx = sin(mid * along / 2.0);
            const double sy = sin(mid * onshore / 2.0);
            if (sx * sx + sy * sy < target) lo = mid; else hi = mid;
        }
        const double k_dx = 0.5 * (lo + hi); /* radians of phase per cell */

        sim->phase_per_cell_x[i] = (int32_t)(65536.0 * k_dx * along / (2.0 * M_PI));
        /* Positive k_y makes crests of sin(wt + k*y) travel toward -y, i.e.
         * shoreward. */
        sim->phase_per_cell_y[i] = (int32_t)(65536.0 * k_dx * onshore / (2.0 * M_PI));

        /* Significant height is crest-to-trough; the boundary drives amplitude. */
        sim->amp_q411[i] =
            (int32_t)((double)comp[i].height_mm * (double)OCEAN_H_ONE / 1000.0 / 2.0);
    }

    /* Ramp over ~4 periods of the longest component. */
    uint32_t longest_ms = 0u;
    for (unsigned i = 0; i < count; ++i) {
        if (comp[i].period_ms > longest_ms) longest_ms = comp[i].period_ms;
    }
    sim->ramp_ticks = sim->tick_ms ? (4u * longest_ms / sim->tick_ms) : 0u;
}

void ocean_sim_reset_surface(ocean_sim_t *sim)
{
    memset(sim->h, 0, OCEAN_CELLS * sizeof(int16_t));
    memset(sim->vel, 0, OCEAN_CELLS * sizeof(int16_t));
    memset(sim->normal, 0, 2u * OCEAN_CELLS * sizeof(int8_t));
    memset(sim->foam, 0, OCEAN_CELLS);
    memset(sim->damp_residual, 0, OCEAN_CELLS * sizeof(uint16_t));
    sim->tick = 0u;
}

/* Land neighbours mirror the centre cell, giving dh/dn = 0: a reflecting wall
 * at the rock rather than an absorbing hole. */
static inline int32_t neighbour(const int16_t *h, const uint8_t *depth,
                                size_t self, size_t other)
{
    return depth[other] == OCEAN_DEPTH_LAND ? (int32_t)h[self] : (int32_t)h[other];
}

/* First-order Mur radiation condition on the seaward row: outgoing energy
 * leaves instead of reflecting back in. */
static void apply_mur(ocean_sim_t *sim, int16_t *h)
{
    const size_t r1 = (size_t)(OCEAN_NY - 1u) * OCEAN_NX;
    const size_t r2 = (size_t)(OCEAN_NY - 2u) * OCEAN_NX;
    for (unsigned x = 0; x < OCEAN_NX; ++x) {
        if (sim->depth[r1 + x] == OCEAN_DEPTH_LAND) { h[r1 + x] = 0; continue; }
        const int32_t k = sim->mur_k[sim->depth[r1 + x]];
        const int32_t inner_new = h[r2 + x];
        const int32_t inner_old = sim->bnd_row_n2[x];
        const int32_t outer_old = sim->bnd_row_n1[x];
        int32_t v = inner_old + mul_shift(k, inner_new - outer_old, 16);
        /* The Mur update has no restoring term, so any offset it inherits sits
         * on this row forever. Bleed it and bound it like the interior. */
        v -= shift_to_zero(v, 12);
        const int32_t limit = sim->h_limit[sim->depth[r1 + x]];
        v = clamp_i32(v, -limit, limit);
        h[r1 + x] = (int16_t)clamp_i32(v, -32768, 32767);
    }
}

/* Additive soft source a few rows inboard of the absorbing edge. It radiates
 * both ways -- the seaward half is absorbed by the Mur row -- and crucially it
 * is transparent to shore reflections passing back out. */
/* Incident plane-wave elevation at grid position (x, y) and absolute tick,
 * Q4.11. Defined everywhere, but only ever evaluated on the two rows either
 * side of the TF/SF interface. */
static int32_t incident(const ocean_sim_t *sim, unsigned x, unsigned y, uint32_t tick)
{
    int32_t sum = 0;
    for (unsigned c = 0; c < sim->comp_count; ++c) {
        const uint32_t phase = tick * sim->phase_per_tick[c] +
                               (uint32_t)((int32_t)x * sim->phase_per_cell_x[c]) +
                               (uint32_t)((int32_t)y * sim->phase_per_cell_y[c]);
        const uint8_t index = (uint8_t)((phase >> 8) & 0xFFu);
        sum += mul_shift(sim->amp_q411[c], (int32_t)sim->sin_q14[index], 14);
    }
    uint32_t ramp = 65536u;
    if (sim->ramp_ticks && tick < sim->ramp_ticks) {
        ramp = (uint32_t)(((uint64_t)tick << 16) / sim->ramp_ticks);
    }
    sum = mul_shift(sum, (int32_t)ramp, 16);
    return mul_shift(sum, (int32_t)sim->source_gain_q16, 16);
}

OCEAN_SIM_HOT static void refresh_normals_and_foam(ocean_sim_t *sim)
{
    const int16_t *h = sim->h;
    const uint8_t *depth = sim->depth;
    const uint8_t shift = sim->normal_shift;

    for (unsigned y = 0; y < OCEAN_NY; ++y) {
        const unsigned ym = (y == 0u) ? 0u : y - 1u;
        const unsigned yp = (y == OCEAN_NY - 1u) ? y : y + 1u;
        for (unsigned x = 0; x < OCEAN_NX; ++x) {
            const size_t i = (size_t)y * OCEAN_NX + x;
            if (depth[i] == OCEAN_DEPTH_LAND) {
                sim->normal[2u * i] = 0;
                sim->normal[2u * i + 1u] = 0;
                sim->foam[i] = 0u;
                continue;
            }
            const unsigned xm = (x == 0u) ? OCEAN_NX - 1u : x - 1u;
            const unsigned xp = (x == OCEAN_NX - 1u) ? 0u : x + 1u;
            const size_t im = (size_t)y * OCEAN_NX + xm;
            const size_t ip = (size_t)y * OCEAN_NX + xp;
            const size_t jm = (size_t)ym * OCEAN_NX + x;
            const size_t jp = (size_t)yp * OCEAN_NX + x;

            const int32_t gx = neighbour(h, depth, i, ip) - neighbour(h, depth, i, im);
            const int32_t gy = neighbour(h, depth, i, jp) - neighbour(h, depth, i, jm);
            sim->normal[2u * i] = (int8_t)clamp_i32(gx >> shift, -127, 127);
            sim->normal[2u * i + 1u] = (int8_t)clamp_i32(gy >> shift, -127, 127);

            /* Depth-limited breaking: crest above the local threshold, scaled
             * by how steep the face is. Replaces the shore-distance heuristic
             * with the criterion actual surf obeys. */
            const int32_t crest = h[i];
            const int32_t threshold = sim->break_h[depth[i]];
            /* Foam decays instead of vanishing: cells near the breaking
             * threshold flip in and out on successive steps, and on the
             * panel each solver cell is tens of pixels wide, so binary foam
             * reads as flashing blocks. An eighth per step gives white water
             * a roughly one-second tail, which is also what surf does. */
            const int32_t faded = (int32_t)sim->foam[i] - ((sim->foam[i] + 7u) >> 3);
            if (crest > threshold && threshold > 0) {
                const int32_t excess = crest - threshold;
                const int32_t steep = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
                int32_t value = (excess * (64 + steep)) >> 9;
                if (value < faded) value = faded;
                sim->foam[i] = (uint8_t)clamp_i32(value, 0, 255);
            } else {
                sim->foam[i] = (uint8_t)faded;
            }
        }
    }
}

OCEAN_SIM_HOT void ocean_sim_step(ocean_sim_t *sim)
{
    int16_t *h = sim->h;
    int16_t *vel = sim->vel;
    const uint8_t *depth = sim->depth;

    /* prev_row is the unmodified row y-1 and cur_row the unmodified row y;
     * row y+1 has not been written yet so it reads straight from h. This keeps
     * the stencil exact while updating in place, avoiding a third full-size
     * buffer that would push the working set out of L2. */
    int16_t *prev_row = sim->row_a;
    int16_t *cur_row = sim->row_b;
    memcpy(prev_row, h, OCEAN_NX * sizeof(int16_t));

    /* The Mur update needs both seaward rows as they stand at step n. */
    memcpy(sim->bnd_row_n1, h + (size_t)(OCEAN_NY - 1u) * OCEAN_NX,
           OCEAN_NX * sizeof(int16_t));
    memcpy(sim->bnd_row_n2, h + (size_t)(OCEAN_NY - 2u) * OCEAN_NX,
           OCEAN_NX * sizeof(int16_t));
    if (sim->absorb_shore) {
        memcpy(sim->shr_row_0, h, OCEAN_NX * sizeof(int16_t));
        memcpy(sim->shr_row_1, h + OCEAN_NX, OCEAN_NX * sizeof(int16_t));
    }

    for (unsigned y = 1u; y < OCEAN_NY - 1u; ++y) {
        const size_t base = (size_t)y * OCEAN_NX;
        const uint32_t damp = sim->row_damp_q16[y];
        memcpy(cur_row, h + base, OCEAN_NX * sizeof(int16_t));

        /* TF/SF stencil corrections. The row just inside the interface reads a
         * scattered-field neighbour, so the incident part must be added back;
         * the row just outside reads a total-field neighbour, so it must be
         * removed. This is the only place the incident wave enters. */
        const int tf_edge = (y + 1u == sim->tfsf_row);
        const int sf_edge = (y == sim->tfsf_row);

        for (unsigned x = 0; x < OCEAN_NX; ++x) {
            const size_t i = base + x;
            if (depth[i] == OCEAN_DEPTH_LAND) {
                h[i] = 0;
                vel[i] = 0;
                continue;
            }
            const unsigned xm = (x == 0u) ? OCEAN_NX - 1u : x - 1u;
            const unsigned xp = (x == OCEAN_NX - 1u) ? 0u : x + 1u;
            const int32_t centre = cur_row[x];

            const int32_t left = depth[base + xm] == OCEAN_DEPTH_LAND
                                 ? centre : (int32_t)cur_row[xm];
            const int32_t right = depth[base + xp] == OCEAN_DEPTH_LAND
                                  ? centre : (int32_t)cur_row[xp];
            int32_t below = depth[i - OCEAN_NX] == OCEAN_DEPTH_LAND
                            ? centre : (int32_t)prev_row[x];
            int32_t above = depth[i + OCEAN_NX] == OCEAN_DEPTH_LAND
                            ? centre : (int32_t)h[i + OCEAN_NX];

            if (tf_edge && depth[i + OCEAN_NX] != OCEAN_DEPTH_LAND) {
                above += incident(sim, x, y + 1u, sim->tick);
            } else if (sf_edge && depth[i - OCEAN_NX] != OCEAN_DEPTH_LAND) {
                below -= incident(sim, x, y - 1u, sim->tick);
            }

            int32_t lap = left + right + below + above - 4 * centre;
            lap = clamp_i32(lap, -LAP_CLAMP, LAP_CLAMP);

            /* Land the acceleration in velocity units, preserving the
             * sub-Q4.11 magnitude that propagation depends on. */
            const int32_t accel = mul_shift((int32_t)sim->c2_lut[depth[i]], lap,
                                            16 - OCEAN_VEL_EXTRA_BITS);

            /* Dissipate through the velocity, never by scaling the height:
             * breaking only triggers on positive crests, so damping h would
             * rectify the signal into a metre-deep set-down. */
            uint32_t cell_damp = damp;
            const uint32_t foam = sim->foam[i];
            if (foam) {
                const uint32_t loss = foam * OCEAN_BREAK_DISSIPATION;
                cell_damp = (cell_damp * (65536u - (loss > 32768u ? 32768u : loss))) >> 16;
            }

            int32_t v = (int32_t)vel[i] + accel;
            /* Exact damping via error feedback: floor the product and carry
             * the 16-bit remainder into the next step. See damp_residual. */
            {
                const int64_t product = (int64_t)v * (int64_t)cell_damp +
                                        (int64_t)sim->damp_residual[i];
                v = (int32_t)(product >> 16);
                sim->damp_residual[i] = (uint16_t)(product - ((int64_t)v << 16));
            }
            v = clamp_i32(v, -32768, 32767);
            vel[i] = (int16_t)v;

            int32_t next = centre + round_shift(v, OCEAN_VEL_EXTRA_BITS);
            /* Grid-scale filter. The discrete stencil's two-cell checkerboard
             * mode is physically meaningless and nothing above damps it: with
             * rounded arithmetic it accumulates without bound (measured: the
             * mean grid-scale residual grew monotonically 1 -> 12 over 6000
             * steps and was still climbing). One part in 512 of the already
             * computed Laplacian removes it with a ~64-step half-life while
             * costing a 7 s swell about 1e-4 of its amplitude per step. */
            next += round_shift(lap, 9);
            /* Velocity damping leaves a static offset as a fixed point, so
             * bleed the height slowly to stop DC accumulating. */
            next -= shift_to_zero(next, 14);

            /* Bound the excursion by the local depth and kill the velocity on
             * contact, so shallow cells cannot free-run. */
            const int32_t limit = sim->h_limit[depth[i]];
            if (next > limit) { next = limit; vel[i] = 0; }
            else if (next < -limit) { next = -limit; vel[i] = 0; }
            h[i] = (int16_t)clamp_i32(next, -32768, 32767);
        }

        int16_t *swap = prev_row;
        prev_row = cur_row;
        cur_row = swap;
    }

    if (sim->absorb_shore) {
        /* Mirrored Mur on the shore row: outgoing toward -y. */
        for (unsigned x = 0; x < OCEAN_NX; ++x) {
            if (sim->depth[x] == OCEAN_DEPTH_LAND) { h[x] = 0; continue; }
            const int32_t k = sim->mur_k[sim->depth[x]];
            int32_t v = sim->shr_row_1[x] +
                mul_shift(k, (int32_t)h[OCEAN_NX] - (int32_t)sim->shr_row_0[x], 16);
            v -= shift_to_zero(v, 12);
            const int32_t limit = sim->h_limit[sim->depth[x]];
            h[x] = (int16_t)clamp_i32(v, -limit, limit);
        }
    } else {
        /* Row 0 is the shore wall. */
        memset(h, 0, OCEAN_NX * sizeof(int16_t));
        memset(vel, 0, OCEAN_NX * sizeof(int16_t));
    }
    sim->tick++;

    apply_mur(sim, h);

    /* The normal and foam fields exist for the renderer, which samples them
     * at a few frames per second; they do not need the physics rate. This
     * pass is also two thirds of the step's external-memory traffic
     * (normals and foam alone stream 147 KB), and on the P4 that traffic is
     * what starves the render core: the water pass measured 78 ms against a
     * flat sea and up to 940 ms in a storm purely because heavier seas
     * lengthen the solver's PSRAM residency. Half-rate refresh halves the
     * solver's footprint on the shared memory system. */
    if ((sim->tick & 1u) == 0u) {
        refresh_normals_and_foam(sim);
    }
}
