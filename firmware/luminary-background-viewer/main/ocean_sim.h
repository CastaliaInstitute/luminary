/* Fixed-point shallow-water solver for the Luminary sea surface.
 *
 * Portable C99 with no platform dependencies: this file and ocean_sim.c are
 * intended to be dropped into firmware/luminary-background-viewer/main/
 * verbatim once tuned on the host. Everything is integer maths so host and
 * target produce bit-identical results.
 *
 * Model: explicit finite-difference wave equation with a depth-dependent
 * celerity, c^2 = g*d. Shoaling and refraction emerge from the depth field
 * rather than being authored, which is the reason this beats a Tessendorf
 * spectrum for a shore-dominated scene.
 */
#ifndef OCEAN_SIM_H
#define OCEAN_SIM_H

#include <stdint.h>
#include <stddef.h>

/* The two hot functions run from IRAM on target. Instruction fetch from flash,
 * not arithmetic, is what limits tight loops on the P4: the renderer's wave
 * pass measured 244 cycles per cell from flash against 40 from IRAM. noinline
 * is part of the contract -- IRAM_ATTR only places a function that is actually
 * emitted, and an inlined one silently stays in flash. The host harness sees
 * neither attribute. */
#ifndef OCEAN_SIM_HOT
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define OCEAN_SIM_HOT IRAM_ATTR __attribute__((noinline))
#else
#define OCEAN_SIM_HOT
#endif
#endif

/* 384 x 384 m at 2 m cells. The offshore extent is load-bearing: the TF/SF
 * interface sits 16 rows in from the seaward edge, and the incident wave is
 * only injected cleanly if that row lies in open deep water. A 128-row domain
 * put the interface in 1-3 m of intertidal shelf with the island crossing the
 * injection row, and the swell scattered and died within twenty cells --
 * measured 0 wave energy in the whole visible channel. At 192 rows the
 * interface sits in ~11 m of water east of the island and the domain fills.
 * The cost is the hot working set no longer fitting the P4's 128 KB L2
 * (h + h_prev + depth is now 184 KB). */
/* Grid size is overridable at compile time so each platform picks its own
 * resolution off the same algorithm. The P4 stays at 192x192 / 2 m (its L2
 * and DIRAM cannot afford more); the Android build passes -DOCEAN_NX=384
 * -DOCEAN_NY=384 for 1 m cells over the identical 384x384 m domain -- four
 * times the cells, genuinely finer waves, and still CFL-stable at 33 ms
 * because this nearshore domain tops out near 20 m depth. */
#ifndef OCEAN_NX
#define OCEAN_NX 192u
#endif
#ifndef OCEAN_NY
#define OCEAN_NY 192u
#endif
#define OCEAN_CELLS ((unsigned)OCEAN_NX * (unsigned)OCEAN_NY)

/* Surface elevation is Q4.11: +/-16 m range, 1/2048 m (0.49 mm) resolution. */
#define OCEAN_H_FRAC_BITS 11
#define OCEAN_H_ONE (1 << OCEAN_H_FRAC_BITS)

/* Extra fractional bits carried by the velocity accumulator. */
#define OCEAN_VEL_EXTRA_BITS 4

#define OCEAN_DX_MM 1042u        /* 200 m across OCEAN_NX cells */
#define OCEAN_TICK_MS 33u        /* 30 Hz; CFL-safe to 30 m depth */
#define OCEAN_DEPTH_MAX_MM 30000u /* depth code 255 == 30 m */

/* Cells shallower than this are land: celerity zero, reflecting neighbour. */
#define OCEAN_DEPTH_LAND 0u

/* Sponge layer at the seaward edge. Held at zero: the sponge would sit on the
 * same rows as the Dirichlet forcing, and any ramp strong enough to absorb
 * outgoing energy also extinguishes the incoming swell before it enters the
 * domain (measured: 0.75/step over 8 rows attenuates the incident wave ~1e-4).
 * Absorbing outgoing energy properly needs a Sommerfeld radiation condition on
 * the forcing row; until then breaking is the energy sink. */
#define OCEAN_SPONGE_ROWS 8u
#define OCEAN_SPONGE_STRENGTH 0.0

/* Extra per-step damping where the surface is breaking, scaled by foam. Surf
 * dissipates nearly all its energy in the break; without this the domain
 * resonates because the shore reflects perfectly. */
#define OCEAN_BREAK_DISSIPATION 38u

/* Depth-limited breaking: surf breaks near H/d ~= 0.78. */
#define OCEAN_BREAK_RATIO_PERCENT 78

typedef struct {
    uint32_t period_ms;   /* measured swell period */
    uint32_t height_mm;   /* significant wave height */
    int32_t  from_deg;    /* bearing the swell arrives from */
} ocean_component_t;

typedef struct {
    int16_t *h;        /* OCEAN_CELLS, Q4.11, current surface */
    /* Damping error-feedback: the low 16 bits of last step's v*damp product.
     * Rounding the damp multiply to nearest lets small velocities survive it
     * unchanged forever (0.9995*v rounds back to v below |v|~1000), which
     * integrates into metre-scale static tilts; truncating instead eats one
     * unit per step and silences the weak diffracted waves the sheltered
     * channel lives on. Carrying the exact remainder forward gives the true
     * decay rate at every amplitude with no bias in either direction. */
    uint16_t *damp_residual;
    /* Velocity carries OCEAN_VEL_EXTRA_BITS more fractional bits than h. A
     * well-resolved wave has a tiny integer Laplacian -- at ~68 cells per
     * wavelength and 0.85 m amplitude it is about 15 units, so c^2*lap lands
     * near 1.5 and truncates to 1 in Q4.11. Accumulating into a finer-grained
     * velocity keeps the sub-unit acceleration that propagation depends on. */
    int16_t *vel;      /* OCEAN_CELLS, Q4.(11+extra) per-step surface velocity */
    uint8_t *depth;    /* OCEAN_CELLS, 0 == land, 255 == OCEAN_DEPTH_MAX_MM */
    int8_t  *normal;   /* 2 * OCEAN_CELLS, interleaved dh/dx, dh/dy */
    uint8_t *foam;     /* OCEAN_CELLS, 0..255 breaking intensity */

    uint16_t c2_lut[256];      /* depth code -> c^2 * dt^2 / dx^2, Q0.16 */
    uint16_t row_damp_q16[OCEAN_NY]; /* per-row damping incl. sponge ramp */
    int16_t  sin_q14[256];     /* full-turn sine, Q1.14 */
    int16_t  break_h[256];     /* depth code -> crest height that breaks, Q4.11 */
    /* Maximum excursion per depth code. As d -> 0 the celerity c^2 = g*d also
     * goes to zero, leaving those cells with no restoring force: they integrate
     * velocity freely and drift without bound, tilting the whole surface. The
     * surface physically cannot fall below the bed, so bound it by depth. */
    int16_t  h_limit[256];

    ocean_component_t comp[3];
    unsigned comp_count;
    /* Per-component phase stepping, precomputed in set_components. Phase is
     * uint16 where a full turn is 65536, so it wraps for free. */
    uint32_t phase_per_tick[3];
    int32_t  phase_per_cell_x[3];
    int32_t  phase_per_cell_y[3];
    int32_t  amp_q411[3];
    /* Bearing pointing from the shore out to sea; Nubble faces ~east. */
    int32_t  shore_normal_deg;
    /* Forcing is ramped in over this many ticks. Starting at full amplitude
     * shock-excites a near-zero-frequency mode that only damping removes, and
     * at 0.9995/step its half-life is ~1400 steps -- it swamps the swell. */
    uint32_t ramp_ticks;

    uint32_t tick;        /* steps elapsed */
    uint8_t  normal_shift; /* gradient -> int8 scaling */

    /* Cell size and timestep are tunable so the domain can be sized against
     * the swell wavelength; defaults are OCEAN_DX_MM / OCEAN_TICK_MS. */
    uint32_t dx_mm;
    uint32_t tick_ms;

    /* Rolling copies of the two rows the stencil still needs unmodified, so
     * the surface can be updated in place without a third full-size buffer. */
    int16_t row_a[OCEAN_NX];
    int16_t row_b[OCEAN_NX];

    /* First-order Mur absorbing boundary on the seaward row, so shore
     * reflections leave the domain instead of resonating. Swell is injected by
     * a soft source a few rows inboard: a Dirichlet forcing row would reflect
     * everything that came back at it. */
    int32_t mur_k[256];          /* depth code -> (S-1)/(S+1), signed Q0.16 */
    int16_t bnd_row_n1[OCEAN_NX]; /* row NY-1 at step n */
    int16_t bnd_row_n2[OCEAN_NX]; /* row NY-2 at step n */

    /* Total-field / scattered-field interface. Rows below tfsf_row carry the
     * total field; rows at or above it carry only the scattered (outgoing)
     * field, which the Mur row then absorbs cleanly. The incident wave enters
     * purely through stencil corrections at the interface, so there is no
     * source row to pile up. */
    unsigned tfsf_row;
    uint32_t source_gain_q16;
    /* Diagnostic only, and currently UNSTABLE -- the mirrored Mur below
     * saturates the surface within a few hundred steps, so this does not yet
     * give a usable free-field leakage number. Off by default; do not enable
     * without fixing the shore-side radiation condition first. */
    int absorb_shore;
    int16_t shr_row_0[OCEAN_NX];
    int16_t shr_row_1[OCEAN_NX];
} ocean_sim_t;

/* Bind caller-owned buffers. On target these come from heap_caps_malloc with
 * MALLOC_CAP_SPIRAM after boot -- never static arrays, the internal heap has
 * no room. */
void ocean_sim_bind(ocean_sim_t *sim, int16_t *h, int16_t *vel,
                    uint8_t *depth, int8_t *normal, uint8_t *foam,
                    uint16_t *damp_residual);

/* Build the celerity LUT, damping profile and sine table. Call after the
 * depth field is populated and before set_components. Uses floating point but
 * runs once at init. */
void ocean_sim_prepare(ocean_sim_t *sim, uint32_t dx_mm, uint32_t tick_ms,
                       double damping_per_step);

/* shore_normal_deg is the bearing from shore out to open water; component
 * bearings are resolved against it to get the along-boundary phase gradient. */
void ocean_sim_set_components(ocean_sim_t *sim,
                              const ocean_component_t *comp, unsigned count,
                              int32_t shore_normal_deg);

/* Zero the surface without touching depth or LUTs. */
void ocean_sim_reset_surface(ocean_sim_t *sim);

/* Advance one OCEAN_TICK_MS step: force the seaward boundary, integrate,
 * damp, then refresh the normal and foam fields. */
void ocean_sim_step(ocean_sim_t *sim);

/* Convert a depth code to millimetres. */
static inline uint32_t ocean_depth_mm(uint8_t code)
{
    return (uint32_t)code * OCEAN_DEPTH_MAX_MM / 255u;
}

#endif /* OCEAN_SIM_H */
