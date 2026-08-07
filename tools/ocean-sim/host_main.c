/* Host driver for the Luminary shallow-water prototype.
 *
 * Builds a synthetic bathymetry, runs the solver, writes PPM frames and
 * reports numerical checks. No hardware and no ESP-IDF involved: the solver
 * source here is the same file destined for firmware/.../main/.
 *
 *   make && ./ocean-sim --steps 900 --frames output/ocean-sim
 */
#include "ocean_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int16_t g_h[OCEAN_CELLS];
static int16_t g_vel[OCEAN_CELLS];
static uint8_t g_depth[OCEAN_CELLS];
static int8_t  g_normal[2 * OCEAN_CELLS];
static uint8_t g_foam[OCEAN_CELLS];

/* Placeholder bathymetry: a curved shoreline at low y, a linear depth ramp out
 * to 30 m, and a rock stack offshore. Real bathymetry must come from the scene
 * geometry -- see the note at the end of build_depth(). */
static double shoreline_y(unsigned x)
{
    const double t = (double)x / (double)OCEAN_NX;
    return 6.0 + 4.0 * sin(t * 2.0 * 2.0 * M_PI);
}

static int g_flat = 0;
static int g_freefield = 0;
static const char *g_depth_file = NULL;

static void build_depth(void)
{
    if (g_depth_file) {
        FILE *f = fopen(g_depth_file, "rb");
        if (!f) { perror(g_depth_file); exit(1); }
        const size_t got = fread(g_depth, 1, OCEAN_CELLS, f);
        fclose(f);
        if (got != OCEAN_CELLS) {
            fprintf(stderr, "depth file %s: expected %u bytes, got %zu\n",
                    g_depth_file, OCEAN_CELLS, got);
            exit(1);
        }
        return;
    }
    if (g_flat) {
        /* Uniform deep basin, no land at all: isolates the integrator from the
         * shore and rock boundary handling. */
        for (size_t i = 0; i < OCEAN_CELLS; ++i) g_depth[i] = 255u;
        return;
    }
    const double rock_x = OCEAN_NX * 0.45;
    const double rock_y = 30.0;
    const double rock_r = 7.0;

    for (unsigned y = 0; y < OCEAN_NY; ++y) {
        for (unsigned x = 0; x < OCEAN_NX; ++x) {
            const size_t i = (size_t)y * OCEAN_NX + x;
            const double shore = shoreline_y(x);

            const double dx = (double)x - rock_x;
            const double dy = (double)y - rock_y;
            if (dx * dx + dy * dy < rock_r * rock_r) {
                g_depth[i] = OCEAN_DEPTH_LAND;
                continue;
            }
            if ((double)y <= shore) {
                g_depth[i] = OCEAN_DEPTH_LAND;
                continue;
            }
            double t = ((double)y - shore) / ((double)(OCEAN_NY - 1u) - shore);
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            unsigned code = (unsigned)(t * 255.0 + 0.5);
            if (code == 0u) code = 1u; /* keep water wet next to the shore */
            g_depth[i] = (uint8_t)code;
        }
    }
    /* TODO: derive from the scene rather than this analytic profile. The
     * existing shore-distance map is image-space only; a world-space depth
     * field needs the sea-plane projection plus the DEM work in scripts/. */
}

static void write_ppm(const ocean_sim_t *sim, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%u %u\n255\n", OCEAN_NX, OCEAN_NY);
    for (unsigned y = 0; y < OCEAN_NY; ++y) {
        /* Draw with the shore at the bottom so it reads like the scene. */
        const unsigned row = OCEAN_NY - 1u - y;
        for (unsigned x = 0; x < OCEAN_NX; ++x) {
            const size_t i = (size_t)row * OCEAN_NX + x;
            unsigned char rgb[3];
            if (sim->depth[i] == OCEAN_DEPTH_LAND) {
                rgb[0] = 70; rgb[1] = 62; rgb[2] = 56;
            } else {
                const int v = sim->h[i];
                const int mag = v > 0 ? v : -v;
                int level = mag * 255 / 3000;
                if (level > 255) level = 255;
                if (v >= 0) {
                    rgb[0] = (unsigned char)(30 + level * 0.85);
                    rgb[1] = (unsigned char)(70 + level * 0.72);
                    rgb[2] = (unsigned char)(110 + level * 0.56);
                } else {
                    rgb[0] = (unsigned char)(30 - level * 0.10 + 0.5);
                    rgb[1] = (unsigned char)(50 - level * 0.15 + 0.5);
                    rgb[2] = (unsigned char)(90 - level * 0.25 + 0.5);
                }
                const unsigned foam = sim->foam[i];
                if (foam) {
                    for (unsigned c = 0; c < 3u; ++c) {
                        rgb[c] = (unsigned char)(rgb[c] + (255 - rgb[c]) * foam / 255);
                    }
                }
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* Mean zero-crossing spacing down a column, within [y0,y1). Two crossings per
 * wavelength, so spacing * 2 is the local wavelength in cells. */
static double column_wavelength(const ocean_sim_t *sim, unsigned x,
                                unsigned y0, unsigned y1, int *crossings_out)
{
    /* Remove the band mean: residual DC from the start transient would
     * otherwise bias every sample to one sign and hide the crossings. */
    double mean = 0.0;
    for (unsigned y = y0; y < y1; ++y) mean += sim->h[(size_t)y * OCEAN_NX + x];
    mean /= (double)(y1 - y0);

    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (unsigned y = y0 + 1u; y < y1; ++y) {
        const double a = sim->h[(size_t)(y - 1u) * OCEAN_NX + x] - mean;
        const double b = sim->h[(size_t)y * OCEAN_NX + x] - mean;
        if ((a <= 0 && b > 0) || (a >= 0 && b < 0)) {
            if (first < 0.0) first = y;
            last = y;
            crossings++;
        }
    }
    if (crossings_out) *crossings_out = crossings;
    if (crossings < 2) return 0.0;
    return (last - first) / (double)(crossings - 1) * 2.0;
}

/* Shallow-water wavelength in cells for a given depth code and period. */
static double predicted_cells(const ocean_sim_t *sim, uint8_t code, uint32_t period_ms)
{
    const double d_m = ocean_depth_mm(code) / 1000.0;
    const double c = sqrt(9.81 * d_m);
    return c * ((double)period_ms / 1000.0) / ((double)sim->dx_mm / 1000.0);
}

int main(int argc, char **argv)
{
    unsigned steps = 900u;
    unsigned frame_every = 0u;
    const char *frame_dir = NULL;
    uint32_t dx_mm = OCEAN_DX_MM;
    uint32_t tick_ms = OCEAN_TICK_MS;
    double gain = 1.0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { frame_dir = argv[++i]; frame_every = 30u; }
        else if (!strcmp(argv[i], "--frame-every") && i + 1 < argc) frame_every = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dx") && i + 1 < argc) dx_mm = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tick") && i + 1 < argc) tick_ms = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--flat")) g_flat = 1;
        else if (!strcmp(argv[i], "--freefield")) g_freefield = 1;
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) g_depth_file = argv[++i];
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) gain = atof(argv[++i]);
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }

    ocean_sim_t sim;
    build_depth();
    ocean_sim_bind(&sim, g_h, g_vel, g_depth, g_normal, g_foam);
    ocean_sim_prepare(&sim, dx_mm, tick_ms, 0.9995);
    sim.source_gain_q16 = (uint32_t)(gain * 65536.0);
    sim.absorb_shore = g_freefield;
    ocean_sim_reset_surface(&sim);

    /* Representative measured swell for York, ME: ~7 s at 1.2 m, plus a
     * longer, smaller secondary from a different bearing. */
    const ocean_component_t comp[2] = {
        { .period_ms = 7000u, .height_mm = 1200u, .from_deg = 110 },
        { .period_ms = 11000u, .height_mm = 500u, .from_deg = 75 },
    };
    ocean_sim_set_components(&sim, comp, 2u, 90);

    printf("grid            %ux%u  dx=%u mm  dt=%u ms  domain %.0f x %.0f m\n",
           OCEAN_NX, OCEAN_NY, sim.dx_mm, sim.tick_ms,
           OCEAN_NX * sim.dx_mm / 1000.0, OCEAN_NY * sim.dx_mm / 1000.0);
    printf("hot working set %zu bytes (h + h_prev + depth)\n",
           (size_t)OCEAN_CELLS * (2 + 2 + 1));
    printf("C^2 at 30 m     %.4f   (stability limit 0.5)\n",
           sim.c2_lut[255] / 65536.0);
    printf("C^2 at  5 m     %.4f\n", sim.c2_lut[42] / 65536.0);

    int32_t peak = 0;
    int32_t peak_late = 0;
    const unsigned probe_i = (OCEAN_NY / 2u) * OCEAN_NX + OCEAN_NX / 4u;
    /* Calibration cell: deep water a few rows inside the TF/SF interface, where
     * the field should still be the clean incident wave. */
    unsigned cal_i = 0; uint8_t cal_best = 0;
    for (unsigned x = 0; x < OCEAN_NX; ++x) {
        const size_t i = (size_t)(sim.tfsf_row - 6u) * OCEAN_NX + x;
        if (g_depth[i] > cal_best) { cal_best = g_depth[i]; cal_i = (unsigned)i; }
    }
    int32_t cal_max = 0;
    /* Leakage: with no scatterer the scattered-field region must stay empty.
     * Anything there is TF/SF leakage. */
    int32_t sf_max = 0;
    const unsigned window = 7000u / tick_ms;
    int32_t pmin = 32767, pmax = -32768;
    size_t peak_i = 0;
    unsigned frame_index = 0;

    for (unsigned s = 0; s < steps; ++s) {
        ocean_sim_step(&sim);

        int32_t local = 0;
        size_t local_i = 0;
        for (size_t i = 0; i < OCEAN_CELLS; ++i) {
            const int32_t v = sim.h[i] < 0 ? -sim.h[i] : sim.h[i];
            if (v > local) { local = v; local_i = i; }
        }
        if (local > peak) { peak_i = local_i; }
        if (local > peak) peak = local;
        if (s >= steps / 2u && local > peak_late) peak_late = local;
        if (steps > 4u * window && s >= steps - 4u * window) {
            const int32_t cv = sim.h[cal_i] < 0 ? -sim.h[cal_i] : sim.h[cal_i];
            if (cv > cal_max) cal_max = cv;
            for (unsigned y = sim.tfsf_row + 2u; y < OCEAN_NY - 2u; ++y)
                for (unsigned x = 0; x < OCEAN_NX; ++x) {
                    const size_t i = (size_t)y * OCEAN_NX + x;
                    if (g_depth[i] == OCEAN_DEPTH_LAND) continue;
                    const int32_t v = sim.h[i] < 0 ? -sim.h[i] : sim.h[i];
                    if (v > sf_max) sf_max = v;
                }
        }
        if (steps > window && s >= steps - window) {
            const int32_t pv = sim.h[probe_i];
            if (pv < pmin) pmin = pv;
            if (pv > pmax) pmax = pv;
        }

        if (frame_dir && frame_every && (s % frame_every) == 0u) {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame-%04u.ppm", frame_dir, frame_index++);
            write_ppm(&sim, path);
        }
    }

    printf("\nran %u steps (%.1f s simulated)\n", steps, steps * OCEAN_TICK_MS / 1000.0);
    {
        const double want = 0.850;
        const double got = cal_max / (double)OCEAN_H_ONE;
        printf("incident calib   %.3f m at TF cell (want %.3f m) -> gain x%.2f\n",
               got, want, got > 0.001 ? want / got : 0.0);
        printf("TF/SF leakage    %.4f m in scattered region (%.1f%% of incident)\n",
               sf_max / (double)OCEAN_H_ONE, sf_max / (double)OCEAN_H_ONE / want * 100.0);
    }
    printf("peak located at   x=%u y=%u  depth %.1f m (domain NY-1=%u, TF/SF row=%u)\n",
           (unsigned)(peak_i % OCEAN_NX), (unsigned)(peak_i / OCEAN_NX),
           ocean_depth_mm(g_depth[peak_i]) / 1000.0, OCEAN_NY - 1u, sim.tfsf_row);
    printf("probe amplitude %.3f m (requested %.3f m) over last period\n",
           (pmax - pmin) / 2.0 / (double)OCEAN_H_ONE, 850.0/1000.0);
    printf("peak |h|        %.3f m overall, %.3f m in second half\n",
           peak / (double)OCEAN_H_ONE, peak_late / (double)OCEAN_H_ONE);

    /* Shoaling: the solver's own model is c = sqrt(g*d), so wavelength should
     * scale as sqrt(depth). Compare a deep band against a shallow one. */
    /* Probe a column that is water for the whole total-field region, and keep
     * every band strictly inside it: the scattered-field rows carry only the
     * outgoing field, so a band spanning the interface measures nothing real. */
    unsigned probe_x = 0, best_run = 0;
    for (unsigned x = 0; x < OCEAN_NX; ++x) {
        unsigned run = 0;
        for (unsigned y = 4u; y < sim.tfsf_row; ++y)
            if (g_depth[(size_t)y * OCEAN_NX + x] != OCEAN_DEPTH_LAND) run++;
        if (run > best_run) { best_run = run; probe_x = x; }
    }
    unsigned first_wet = 4u;
    while (first_wet < sim.tfsf_row &&
           g_depth[(size_t)first_wet * OCEAN_NX + probe_x] == OCEAN_DEPTH_LAND) first_wet++;

    const unsigned deep_hi = sim.tfsf_row - 4u;
    const unsigned deep_lo = first_wet + (deep_hi - first_wet) / 2u;
    const unsigned shal_lo = first_wet + 2u;
    const unsigned shal_hi = deep_lo;
    int deep_cross = 0, shal_cross = 0;
    const double lambda_deep = column_wavelength(&sim, probe_x, deep_lo, deep_hi, &deep_cross);
    const double lambda_shallow = column_wavelength(&sim, probe_x, shal_lo, shal_hi, &shal_cross);
    const uint8_t code_deep = g_depth[(size_t)((deep_lo + deep_hi) / 2u) * OCEAN_NX + probe_x];
    const uint8_t code_shallow = g_depth[(size_t)((shal_lo + shal_hi) / 2u) * OCEAN_NX + probe_x];
    const double d_deep = ocean_depth_mm(code_deep);
    const double d_shallow = ocean_depth_mm(code_shallow);

    printf("\nshoaling check at x=%u (primary period %u ms)\n", probe_x, comp[0].period_ms);
    printf("  deep    band y=%u..%u: depth %.1f m, lambda %.1f cells (predicted %.1f), %d crossings\n",
           deep_lo, deep_hi, d_deep / 1000.0, lambda_deep,
           predicted_cells(&sim, code_deep, comp[0].period_ms), deep_cross);
    printf("  shallow band y=%u..%u: depth %.1f m, lambda %.1f cells (predicted %.1f), %d crossings\n",
           shal_lo, shal_hi, d_shallow / 1000.0, lambda_shallow,
           predicted_cells(&sim, code_shallow, comp[0].period_ms), shal_cross);
    if (lambda_deep > 0.0 && lambda_shallow > 0.0 && d_deep > 0.0) {
        const double measured = lambda_shallow / lambda_deep;
        const double predicted = sqrt(d_shallow / d_deep);
        printf("  ratio measured %.3f vs sqrt(d) predicted %.3f  (error %.1f%%)\n",
               measured, predicted, fabs(measured - predicted) / predicted * 100.0);
    }

    if (getenv("OCEAN_DUMP")) {
        printf("\ncolumn x=%u (shore at low y), h in metres:\n  ", probe_x);
        for (unsigned y = 0; y < OCEAN_NY; y += 4u) {
            printf("%+.2f ", sim.h[(size_t)y * OCEAN_NX + probe_x] / (double)OCEAN_H_ONE);
            if ((y / 4u) % 8u == 7u) printf("\n  ");
        }
        printf("\n");
    }

    size_t breaking = 0;
    for (size_t i = 0; i < OCEAN_CELLS; ++i) if (g_foam[i]) breaking++;
    printf("\nbreaking cells this frame: %zu of %u\n", breaking, OCEAN_CELLS);

    const int stable = peak_late < 32000;
    printf("\n%s\n", stable ? "STABLE: bounded amplitude, no blow-up"
                            : "UNSTABLE: amplitude saturated the fixed-point range");
    return stable ? 0 : 1;
}
