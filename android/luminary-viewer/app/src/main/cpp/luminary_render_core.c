/* See luminary_render_core.h. The algorithms here are line-for-line ports of
 * the P4 firmware's validated pipeline (firmware/luminary-background-viewer/
 * main/luminary_animation_viewer.c); comments note where a phone earns a
 * simplification the microcontroller could not afford.
 */
#include "luminary_render_core.h"
#include "ocean_sim.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define OCEAN_MAP_W 1024
#define OCEAN_MAP_ROW0 290
#define OCEAN_MAP_ROWS 310
/* The screen map stores Q8.8 solver coordinates. cell*256+frac overflows
 * uint16 once the grid passes 255 cells (the 1 m grid is 384), so the map
 * element widens with the grid -- build-ocean-screen-map.py writes the
 * matching width off the same threshold. */
#if OCEAN_NX > 255
typedef uint32_t ocean_map_t;
#define OCEAN_MAP_NONE 0xFFFFFFFFu
#else
typedef uint16_t ocean_map_t;
#define OCEAN_MAP_NONE 0xFFFFu
#endif
#define CLOUD_W 1024   /* full GOES-projection resolution: ~2 px/texel over the */
#define CLOUD_H 291    /* 2048x582 sky, so cloud structure no longer shows the  */
                       /* coarse 8 px texels that read as sky seams/banding.    */
#define SHELL_HIGH_M 6000
#define SHELL_MID_M 3000
#define SHELL_LOW_M 1200

static lum_assets_t assets;
static lum_conditions_t conditions;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

/* Solver state; buffers heap-owned. */
static ocean_sim_t sim;
static int16_t *sim_h, *sim_vel;
static uint8_t *sim_depth, *sim_foam;
static int8_t *sim_normal;
static uint16_t *sim_damp_residual;
static pthread_mutex_t sim_lock = PTHREAD_MUTEX_INITIALIZER;
static bool sim_ready;

/* Per-frame tables, same construction as the firmware. */
static int8_t wave_sine[256];
static int16_t wave_component_lut[3][256];
static int8_t wave_crest_lut[256];
static uint8_t wave_color_lut[3][16][256];
static uint32_t color_lut_key = 0xFFFFFFFFu;
static int8_t wave_shade_row[OCEAN_MAP_W];
static uint8_t wave_breaker_phase_row[OCEAN_MAP_W];
static uint8_t wave_foam_row[OCEAN_MAP_W];
static int8_t wave_dx_row[OCEAN_MAP_W];
static int8_t wave_dy_row[OCEAN_MAP_W];
static uint8_t shade_dither[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5},
};
/* Snapshot of solver output taken under the lock once per frame. */
static int8_t *normal_snapshot;
static uint8_t *foam_snapshot;

static uint16_t cloud_x_lut[LUM_WIDTH];   /* Q8 atlas x (texel*256 + frac) */
static uint16_t cloud_y_lut[LUM_HORIZON];  /* Q8 atlas y */
static uint8_t cloud_row_trans[LUM_WIDTH / 4];
static uint8_t *cloud_soft[3];      /* load-time softened copies of the atlases */
static uint8_t cloud_row_add[(LUM_WIDTH / 4) * 3];

static inline bool water_pixel(size_t pixel)
{
    return (assets.water_mask[pixel >> 3] >> (pixel & 7u)) & 1u;
}

/* Island, rocks, lighthouse, foreground: solid, and only a placeholder here
 * for the 3D-printed relief that will cover them. The renderer draws nothing
 * over these -- the authored photograph passes through untouched -- so what
 * shows is exactly what the print will replace. */
static inline bool land_pixel(size_t pixel)
{
    return (assets.land_mask[pixel >> 3] >> (pixel & 7u)) & 1u;
}

static inline uint8_t clamp_channel(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Authored frame's mean sky chroma, from the firmware's constants. */
#define AUTHORED_SKY_R 168
#define AUTHORED_SKY_G 208
#define AUTHORED_SKY_B 228

/* --- per-pixel surface detail -------------------------------------------
 *
 * The 1 m CFD grid carries the real motion -- shoaling, refraction,
 * breaking -- but its finest wave is ~2 cells, a metre or two, and between
 * those the surface is smooth. Real water is not: it carries capillary
 * ripple far below any grid a solver can afford. This layer adds that back
 * per pixel: a tileable multi-octave value-noise field, pre-differentiated
 * in the shoreward direction so a sample reads as ripple shading, scrolled
 * with the wind and scaled by the LOCAL wave energy the solver reports, so
 * ripples live where the water actually moves and calm water stays glassy.
 * It is not physics; it is the sub-grid detail the physics cannot resolve,
 * driven by the physics so it never contradicts it. */
#define DETAIL_N 512           /* power of two: wrap with & (DETAIL_N-1) */

/* Contrast of the CFD swell shading (shade = -gradient * gain). Higher makes
 * the data-driven wave crests/troughs -- and thus the large-scale swell rolling
 * shoreward -- read more strongly against the base photograph. */
#ifndef CFD_SHADE_GAIN
#define CFD_SHADE_GAIN 24
#endif
static int8_t detail_grad[DETAIL_N * DETAIL_N];
/* Scroll offsets for two layers, set per frame in lum_render_frame. */
static int detail_scroll_x0, detail_scroll_y0, detail_scroll_x1, detail_scroll_y1;

static float detail_lattice_value(uint32_t x, uint32_t y, uint32_t period)
{
    /* Deterministic hash -> [-1,1], wrapped at `period` so every octave
     * tiles seamlessly across the DETAIL_N field. */
    uint32_t h = (x % period) * 374761393u + (y % period) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)(h & 0xFFFFu) / 32767.5f - 1.0f;
}

static float detail_octave(float u, float v, uint32_t period)
{
    const float fx = u * period, fy = v * period;
    const uint32_t x0 = (uint32_t)fx, y0 = (uint32_t)fy;
    const float tx = fx - x0, ty = fy - y0;
    /* Smoothstep for C1 continuity across lattice cells. */
    const float sx = tx * tx * (3.0f - 2.0f * tx);
    const float sy = ty * ty * (3.0f - 2.0f * ty);
    const float a = detail_lattice_value(x0, y0, period);
    const float b = detail_lattice_value(x0 + 1, y0, period);
    const float c = detail_lattice_value(x0, y0 + 1, period);
    const float d = detail_lattice_value(x0 + 1, y0 + 1, period);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

static void build_detail_field(void)
{
    /* Four octaves at tiling periods; height first, then the shoreward
     * derivative baked to int8. */
    static float height[DETAIL_N * DETAIL_N];
    for (unsigned y = 0; y < DETAIL_N; ++y) {
        for (unsigned x = 0; x < DETAIL_N; ++x) {
            const float u = (float)x / DETAIL_N, v = (float)y / DETAIL_N;
            float h = detail_octave(u, v, 8) * 0.55f +
                      detail_octave(u, v, 16) * 0.28f +
                      detail_octave(u, v, 32) * 0.13f +
                      detail_octave(u, v, 64) * 0.06f;
            height[y * DETAIL_N + x] = h;
        }
    }
    for (unsigned y = 0; y < DETAIL_N; ++y) {
        const unsigned ym = (y - 1) & (DETAIL_N - 1);
        const unsigned yp = (y + 1) & (DETAIL_N - 1);
        for (unsigned x = 0; x < DETAIL_N; ++x) {
            const float g = (height[yp * DETAIL_N + x] - height[ym * DETAIL_N + x]) * 96.0f;
            const int gi = (int)lrintf(g);
            detail_grad[y * DETAIL_N + x] = (int8_t)(gi < -127 ? -127 : gi > 127 ? 127 : gi);
        }
    }
}

/* Soften a cloud atlas at load with two [1,2,1] separable passes (~radius 2).
 * The 256x96 atlas is 8 px/texel on screen, so a crisp cloud edge quantises
 * into a straight, seam-like segment and gives the panel's upscaler sharp fine
 * structure to alias into banding. Blurring the atlas once, up front, keeps
 * cloud edges gradual without any per-frame cost. Both channels (luminance,
 * alpha) are blurred; edges are clamped. */
#ifndef CLOUD_SOFTEN_PASSES
#define CLOUD_SOFTEN_PASSES 5
#endif
static void soften_atlas(const uint8_t *src, uint8_t *dst)
{
    static uint8_t a[CLOUD_W * CLOUD_H * 2], b[CLOUD_W * CLOUD_H * 2];
    memcpy(a, src, sizeof a);
    for (int pass = 0; pass < CLOUD_SOFTEN_PASSES; ++pass) {
        for (int y = 0; y < CLOUD_H; ++y)
            for (int x = 0; x < CLOUD_W; ++x)
                for (int c = 0; c < 2; ++c) {
                    const int xm = x > 0 ? x - 1 : 0;
                    const int xp = x < CLOUD_W - 1 ? x + 1 : CLOUD_W - 1;
                    b[(y * CLOUD_W + x) * 2 + c] = (uint8_t)((
                        a[(y * CLOUD_W + xm) * 2 + c] +
                        2 * a[(y * CLOUD_W + x) * 2 + c] +
                        a[(y * CLOUD_W + xp) * 2 + c]) >> 2);
                }
        for (int y = 0; y < CLOUD_H; ++y)
            for (int x = 0; x < CLOUD_W; ++x)
                for (int c = 0; c < 2; ++c) {
                    const int ym = y > 0 ? y - 1 : 0;
                    const int yp = y < CLOUD_H - 1 ? y + 1 : CLOUD_H - 1;
                    a[(y * CLOUD_W + x) * 2 + c] = (uint8_t)((
                        b[(ym * CLOUD_W + x) * 2 + c] +
                        2 * b[(y * CLOUD_W + x) * 2 + c] +
                        b[(yp * CLOUD_W + x) * 2 + c]) >> 2);
                }
    }
    memcpy(dst, a, sizeof a);
}

bool lum_init(const lum_assets_t *bound)
{
    assets = *bound;
    build_detail_field();
    for (unsigned i = 0; i < 256; ++i) {
        wave_sine[i] = (int8_t)lrintf(127.0f * sinf((float)i * 6.28318530718f / 256.0f));
    }
    for (unsigned x = 0; x < LUM_WIDTH; ++x) {
        cloud_x_lut[x] = (uint16_t)((unsigned)x * CLOUD_W * 256u / LUM_WIDTH);
    }
    for (unsigned y = 0; y < LUM_HORIZON; ++y) {
        cloud_y_lut[y] = (uint16_t)((unsigned)y * CLOUD_H * 256u / LUM_HORIZON);
    }

    sim_h = calloc(OCEAN_CELLS, sizeof(int16_t));
    sim_vel = calloc(OCEAN_CELLS, sizeof(int16_t));
    sim_depth = malloc(OCEAN_CELLS);
    sim_normal = calloc(2 * OCEAN_CELLS, 1);
    sim_foam = calloc(OCEAN_CELLS, 1);
    sim_damp_residual = calloc(OCEAN_CELLS, sizeof(uint16_t));
    normal_snapshot = calloc(2 * OCEAN_CELLS, 1);
    foam_snapshot = calloc(OCEAN_CELLS, 1);
    if (!sim_h || !sim_vel || !sim_depth || !sim_normal || !sim_foam ||
        !sim_damp_residual || !normal_snapshot || !foam_snapshot) {
        return false;
    }
    const uint8_t *raw_atlas[3] = {assets.cloud_high, assets.cloud_mid, assets.cloud_low};
    for (int i = 0; i < 3; ++i) {
        cloud_soft[i] = malloc((size_t)CLOUD_W * CLOUD_H * 2);
        if (!cloud_soft[i]) return false;
        soften_atlas(raw_atlas[i], cloud_soft[i]);
    }

    memcpy(sim_depth, assets.ocean_depth, OCEAN_CELLS);
    ocean_sim_bind(&sim, sim_h, sim_vel, sim_depth, sim_normal, sim_foam,
                   sim_damp_residual);
    /* 2 m bathymetry cells; 30 Hz -- the phone has the headroom the P4 did
     * not, and finer time steps only help the surf. CFL-safe (c^2 scales
     * with dt^2 and the prepare clamp holds regardless). */
    ocean_sim_prepare(&sim, 2000, 33, 0.9995);
    ocean_sim_reset_surface(&sim);
    return true;
}

uint32_t lum_solver_tick_ms(void) { return sim.tick_ms; }

void lum_set_conditions(const lum_conditions_t *next)
{
    pthread_mutex_lock(&state_lock);
    conditions = *next;
    ocean_component_t comp[3];
    unsigned count = conditions.wave_count > 3u ? 3u : conditions.wave_count;
    for (unsigned i = 0; i < count; ++i) {
        comp[i].period_ms = conditions.waves[i].period_ms;
        comp[i].height_mm = conditions.waves[i].height_mm;
        comp[i].from_deg = conditions.waves[i].from_deg;
    }
    if (count == 0u) {
        comp[0] = (ocean_component_t){7000u, 500u, 137};
        count = 1u;
    }
    pthread_mutex_lock(&sim_lock);
    ocean_sim_set_components(&sim, comp, count, 90);
    sim_ready = true;
    pthread_mutex_unlock(&sim_lock);
    pthread_mutex_unlock(&state_lock);
}

size_t lum_cloud_atlas_bytes(void) { return (size_t)CLOUD_W * CLOUD_H * 2u; }

void lum_set_clouds(const uint8_t *low, const uint8_t *mid, const uint8_t *high)
{
    /* cloud_soft is ordered {high, mid, low} to match the render's shell loop. */
    const uint8_t *raw[3] = {high, mid, low};
    pthread_mutex_lock(&state_lock);
    for (int i = 0; i < 3; ++i) {
        if (raw[i] && cloud_soft[i]) soften_atlas(raw[i], cloud_soft[i]);
    }
    pthread_mutex_unlock(&state_lock);
}

void lum_solver_tick(void)
{
    pthread_mutex_lock(&sim_lock);
    if (sim_ready) ocean_sim_step(&sim);
    pthread_mutex_unlock(&sim_lock);
}

/* ---- per-frame table construction (firmware: build_wave_component_luts) */

static int wave_sine_q8(uint32_t phase_q8)
{
    const unsigned index = (phase_q8 >> 8) & 0xFFu;
    const unsigned fraction = phase_q8 & 0xFFu;
    const int low = wave_sine[index];
    const int high = wave_sine[(index + 1u) & 0xFFu];
    return low + (((high - low) * (int)fraction) >> 8);
}

static void build_wave_tables(uint64_t elapsed_ms, int *total_weight_out)
{
    uint16_t phase_q8[3] = {0, 0, 0};
    int weight[3] = {1, 1, 1};
    int total = 0;
    unsigned count = conditions.wave_count > 3u ? 3u : conditions.wave_count;
    if (count == 0u) count = 1u;
    for (unsigned c = 0; c < count; ++c) {
        const uint32_t period = conditions.waves[c].period_ms > 500u
                                    ? conditions.waves[c].period_ms : 7000u;
        phase_q8[c] = (uint16_t)(elapsed_ms * 65536ull / period);
        unsigned h = conditions.waves[c].height_mm / 80u;
        weight[c] = 1 + (int)(h > 30u ? 30u : h);
        total += weight[c];
    }
    for (unsigned c = 0; c < 3u; ++c) {
        const int w = c < count ? weight[c] : 0;
        const uint32_t p = c < count ? phase_q8[c] : 0u;
        for (unsigned stored = 0; stored < 256u; ++stored) {
            const uint32_t argument = (((uint32_t)stored + 64u) << 8) - p;
            wave_component_lut[c][stored] =
                (int16_t)(wave_sine_q8(argument & 0xFFFFu) * w);
        }
    }
    for (unsigned stored = 0; stored < 256u; ++stored) {
        const uint32_t argument = ((uint32_t)stored << 8) - phase_q8[0];
        wave_crest_lut[stored] = (int8_t)wave_sine_q8(argument & 0xFFFFu);
    }
    *total_weight_out = total;
}

static unsigned sunset_warmth_255(void)
{
    const int altitude = conditions.sun_altitude_deci_deg;
    if (altitude >= 100) return 0u;
    if (altitude >= 0) return (unsigned)((100 - altitude) * 220 / 100);
    if (altitude >= -10) return (unsigned)(220 + (-altitude) * 2);
    if (altitude >= -60) return (unsigned)(240 - ((-altitude - 10) * 160 / 50));
    if (altitude >= -120) return (unsigned)((120 + altitude) * 80 / 60);
    return 0u;
}

static void build_color_lut(void)
{
    const uint32_t key = ((uint32_t)conditions.sky_b << 24) |
                         ((uint32_t)conditions.sky_g << 16) |
                         ((uint32_t)conditions.sky_r << 8) | conditions.sun_mode;
    if (key == color_lut_key) return;
    color_lut_key = key;
    /* Channel order here is R,G,B (the output format's order); reflected sky
     * per channel mirrors the firmware's construction. */
    const unsigned reflected[3] = {conditions.sky_r, conditions.sky_g, conditions.sky_b};
    static const unsigned nautical[3] = {34u, 42u, 52u};
    static const unsigned night[3] = {18u, 24u, 34u};
    for (unsigned ch = 0; ch < 3u; ++ch) {
        for (unsigned si = 0; si < 16u; ++si) {
            const int shade = -30 + (int)si * 4;
            const unsigned glint = shade >= 0 ? (unsigned)shade * 3u : 0u;
            for (unsigned src = 0; src < 256u; ++src) {
                int v = (int)src;
                if (shade >= 0) v += ((int)reflected[ch] - v) * (int)glint >> 7;
                v = (int)clamp_channel(v + shade);
                if (conditions.sun_mode == 2u) v = v * (int)nautical[ch] / 100;
                else if (conditions.sun_mode == 3u) v = v * (int)night[ch] / 100;
                wave_color_lut[ch][si][src] = (uint8_t)v;
            }
        }
    }
}

/* ---- wave row from the solver (firmware: compute_wave_row_sim) */

static void compute_wave_row(const ocean_map_t *map_row, int shade_recip_q16)
{
    for (unsigned px = 0; px < OCEAN_MAP_W; ++px) {
        const unsigned gx_q8 = map_row[px * 2u];
        const unsigned gy_q8 = map_row[px * 2u + 1u];
        if (gy_q8 == OCEAN_MAP_NONE) {
            /* Outside the solver domain: analytic sine fallback, as on the
             * panel. The stored phase field is not shipped to the phone, so
             * far-field cells use a plain horizontal progression -- they sit
             * within a few rows of the horizon where any coherent phase
             * reads correctly. */
            const uint8_t stored = (uint8_t)(px * 3u);
            const int light = wave_component_lut[0][stored] +
                              wave_component_lut[1][(uint8_t)(stored + 85u)] +
                              wave_component_lut[2][(uint8_t)(stored + 170u)];
            int shade = light * shade_recip_q16 >> 16;
            if (shade < -32) shade = -32;
            if (shade > 31) shade = 31;
            wave_shade_row[px] = (int8_t)(shade << 2);
            wave_breaker_phase_row[px] = stored;
            wave_foam_row[px] = 255u;
            wave_dx_row[px] = 0;
            wave_dy_row[px] = 0;
            continue;
        }
        const unsigned cx = gx_q8 >> 8, cy = gy_q8 >> 8;
        /* Smoothstep the interpolation fractions (Hermite 3t^2-2t^3 in Q8).
         * Plain bilinear is value-continuous but slope-DIScontinuous at cell
         * boundaries; since shade = -gradient*24 amplifies slope, those slope
         * jumps read as the projected solver grid -- faint diagonal
         * parallelogram tiling in smooth near-field water. Smoothstep zeroes
         * the derivative at the cell nodes (C1), so the facets disappear
         * without a finer, costlier grid. */
        int fx = (int)(gx_q8 & 0xFFu), fy = (int)(gy_q8 & 0xFFu);
        fx = fx * fx * (768 - 2 * fx) >> 16;
        fy = fy * fy * (768 - 2 * fy) >> 16;
        const size_t c00 = (size_t)cy * OCEAN_NX + cx;
        const size_t c10 = c00 + OCEAN_NX;
        const int n00 = normal_snapshot[2u * c00 + 1u];
        const int n01 = normal_snapshot[2u * (c00 + 1u) + 1u];
        const int n10 = normal_snapshot[2u * c10 + 1u];
        const int n11 = normal_snapshot[2u * (c10 + 1u) + 1u];
        const int top = n00 + ((n01 - n00) * fx >> 8);
        const int bottom = n10 + ((n11 - n10) * fx >> 8);
        const int gradient = top + ((bottom - top) * fy >> 8);
        /* Interpolate the X gradient across the cell too. Sampled from the
         * nearest cell it was piecewise-constant, so the refraction offset dx
         * below jumped at every solver-cell boundary; in the near field where
         * one 1 m cell spans a wide screen patch those jumps displaced the base
         * photo in constant blocks -- the projected grid read as diamond
         * tiling. Bilerping it makes the displacement vary smoothly. */
        const int gx00 = normal_snapshot[2u * c00];
        const int gx01 = normal_snapshot[2u * (c00 + 1u)];
        const int gx10 = normal_snapshot[2u * c10];
        const int gx11 = normal_snapshot[2u * (c10 + 1u)];
        const int gx_top = gx00 + ((gx01 - gx00) * fx >> 8);
        const int gx_bottom = gx10 + ((gx11 - gx10) * fx >> 8);
        const int gradient_x = gx_top + ((gx_bottom - gx_top) * fy >> 8);

        int shade = -gradient * CFD_SHADE_GAIN;
        if (shade < -128) shade = -128;
        if (shade > 127) shade = 127;
        wave_shade_row[px] = (int8_t)shade;
        wave_breaker_phase_row[px] = 0u;

        int dx = gradient_x * 2;
        if (dx < -7) dx = -7;
        if (dx > 7) dx = 7;
        int dy = -gradient / 4;
        if (dy < -1) dy = -1;
        if (dy > 1) dy = 1;
        wave_dx_row[px] = (int8_t)dx;
        wave_dy_row[px] = (int8_t)dy;

        const int f00 = foam_snapshot[c00], f01 = foam_snapshot[c00 + 1u];
        const int f10 = foam_snapshot[c10], f11 = foam_snapshot[c10 + 1u];
        const int foam_top = f00 + ((f01 - f00) * fx >> 8);
        const int foam_bottom = f10 + ((f11 - f10) * fx >> 8);
        int foam = foam_top + ((foam_bottom - foam_top) * fy >> 8);
        if (foam > 254) foam = 254;
        wave_foam_row[px] = (uint8_t)foam;
    }
}

/* ---- sky (firmware: render_sky_row, composed clouds + grade) */

/* Mirror-fold both atlas axes. Plain wrap in X made the atlas repeat every
 * CLOUD_W texels; because the GOES crop is not horizontally tileable, texel
 * 255 abutted texel 0 at a hard discontinuity, and with cloud drift that
 * boundary swept across the sky as a vertical seam (one per shell). Reflecting
 * at the edge -- exactly what Y already does -- keeps the field C0 across the
 * fold, so there is no seam. */
static inline int mirror_cloud_x(int v)
{
    const int period = 2 * (CLOUD_W - 1);
    v %= period;
    if (v < 0) v += period;
    return v < CLOUD_W ? v : period - v;
}

static inline int mirror_cloud_y(int v)
{
    const int period = 2 * (CLOUD_H - 1);
    v %= period;
    if (v < 0) v += period;
    return v < CLOUD_H ? v : period - v;
}

static void render_sky_row(uint32_t *out, const uint8_t *base_row, unsigned y,
                           const int shift_x[3], const int shift_y[3])
{
    const unsigned cover = conditions.cloud_cover_permille;
    if (cover > 0u) {
        const unsigned clearance = LUM_HORIZON - y;
        const unsigned feather = clearance < 22u ? clearance : 22u;
        memset(cloud_row_trans, 255, sizeof(cloud_row_trans));
        memset(cloud_row_add, 0, sizeof(cloud_row_add));
        const uint8_t *atlases[3] = {cloud_soft[0], cloud_soft[1], cloud_soft[2]};
        for (unsigned shell = 0; shell < 3u; ++shell) {
            /* Bilinear atlas fetch. The atlas is only 256x96 stretched across
             * the 2048x582 sky, so nearest sampling turned every texel into a
             * flat ~8x6 px block -- the sky "tiling". Interpolate both axes so
             * the low-res cloud field reads as a smooth gradient. */
            const int ay_q8 = (int)cloud_y_lut[y] + (shift_y[shell] << 8);
            const int iy = ay_q8 >> 8;
            const unsigned fyc = (unsigned)(ay_q8 & 0xFF);
            const uint8_t *row0 = atlases[shell] +
                (size_t)mirror_cloud_y(iy) * CLOUD_W * 2u;
            const uint8_t *row1 = atlases[shell] +
                (size_t)mirror_cloud_y(iy + 1) * CLOUD_W * 2u;
            const unsigned bias = conditions.shells[shell].blue_bias;
            const unsigned half_bias = bias / 2u;
            const int off_q8 = shift_x[shell] << 8;
            for (unsigned q = 0; q < LUM_WIDTH / 4u; ++q) {
                const int ax_q8 = (int)cloud_x_lut[q * 4u] + off_q8;
                const unsigned fxc = (unsigned)(ax_q8 & 0xFF);
                const unsigned ax0 = (unsigned)mirror_cloud_x(ax_q8 >> 8);
                const unsigned ax1 = (unsigned)mirror_cloud_x((ax_q8 >> 8) + 1);
                const uint8_t *t00 = row0 + (size_t)ax0 * 2u;
                const uint8_t *t01 = row0 + (size_t)ax1 * 2u;
                const uint8_t *t10 = row1 + (size_t)ax0 * 2u;
                const uint8_t *t11 = row1 + (size_t)ax1 * 2u;
                const unsigned l_top = t00[0] + (((int)t01[0] - t00[0]) * (int)fxc >> 8);
                const unsigned l_bot = t10[0] + (((int)t11[0] - t10[0]) * (int)fxc >> 8);
                const unsigned lum = l_top + (((int)l_bot - (int)l_top) * (int)fyc >> 8);
                const unsigned a_top = t00[1] + (((int)t01[1] - t00[1]) * (int)fxc >> 8);
                const unsigned a_bot = t10[1] + (((int)t11[1] - t10[1]) * (int)fxc >> 8);
                const unsigned a_tex = a_top + (((int)a_bot - (int)a_top) * (int)fyc >> 8);
                unsigned alpha = a_tex * cover / 1000u;
                alpha = alpha * feather / 22u;
                if (alpha == 0u) continue;
                const unsigned cg = lum > half_bias ? lum - half_bias : 0u;
                const unsigned cr = lum > bias ? lum - bias : 0u;
                const unsigned keep = 255u - alpha;
                uint8_t *add = cloud_row_add + (size_t)q * 3u;
                /* RGBX order: r, g, b. Cloud b = luminance (firmware bgr[0]). */
                add[0] = (uint8_t)((add[0] * keep + cr * alpha) / 255u);
                add[1] = (uint8_t)((add[1] * keep + cg * alpha) / 255u);
                add[2] = (uint8_t)((add[2] * keep + lum * alpha) / 255u);
                cloud_row_trans[q] = (uint8_t)(cloud_row_trans[q] * keep / 255u);
            }
        }
    }

    const int shift_r = (int)conditions.sky_r - AUTHORED_SKY_R;
    const int shift_g = (int)conditions.sky_g - AUTHORED_SKY_G;
    const int shift_b = (int)conditions.sky_b - AUTHORED_SKY_B;
    const unsigned warmth = sunset_warmth_255();
    unsigned glow_row = 0u;
    if (warmth > 0u) {
        const unsigned distance = LUM_HORIZON - y;
        const unsigned vertical = distance < 190u ? (190u - distance) * 255u / 190u : 0u;
        glow_row = warmth * vertical / 255u;
    }
    const bool sunset_left = conditions.sun_relative_azimuth_deci_deg < 0;
    unsigned mode = 0u, light = 255u;
    if (conditions.sun_mode == 1u && conditions.sun_altitude_deci_deg < 0) {
        const int na = -conditions.sun_altitude_deci_deg;
        const unsigned depth = (unsigned)(na > 60 ? 60 : na);
        light = 255u - depth * 85u / 60u;
        mode = 1u;
    } else if (conditions.sun_mode >= 2u) {
        mode = conditions.sun_mode;
    }

    const size_t row_pixel = (size_t)y * LUM_WIDTH;
    for (unsigned x = 0; x < LUM_WIDTH; ++x) {
        const uint8_t *src = base_row + (size_t)x * 3u;
        // The island and lighthouse rise above the horizon into the sky band.
        // They are solid placeholder for the print: pass the photo through
        // untouched, no grading, no clouds.
        if (land_pixel(row_pixel + x)) {
            out[x] = 0xFF000000u | ((unsigned)src[2] << 16) |
                     ((unsigned)src[1] << 8) | src[0];
            continue;
        }
        unsigned r = clamp_channel((int)src[0] + shift_r);
        unsigned g = clamp_channel((int)src[1] + shift_g);
        unsigned b = clamp_channel((int)src[2] + shift_b);
        if (conditions.cloud_cover_permille > 0u) {
            /* The cloud contribution is accumulated once per 4 px quad. Applied
             * flat it left hard 4 px vertical steps that are invisible at 1x but
             * that the panel's non-integer upscale beats into visible vertical
             * banding. Interpolate each quad's transmission and premultiplied
             * colour across the 4 px it spans so the cloud field is smooth. */
            const unsigned q = x >> 2u;
            const unsigned q1 = (q + 1u < LUM_WIDTH / 4u) ? q + 1u : q;
            const int fx = (int)(x & 3u);
            const int t0 = cloud_row_trans[q], t1 = cloud_row_trans[q1];
            const unsigned trans = (unsigned)(t0 + ((t1 - t0) * fx) / 4);
            if (trans != 255u) {
                const uint8_t *a0 = cloud_row_add + (size_t)q * 3u;
                const uint8_t *a1 = cloud_row_add + (size_t)q1 * 3u;
                const unsigned add0 = (unsigned)(a0[0] + ((a1[0] - a0[0]) * fx) / 4);
                const unsigned add1 = (unsigned)(a0[1] + ((a1[1] - a0[1]) * fx) / 4);
                const unsigned add2 = (unsigned)(a0[2] + ((a1[2] - a0[2]) * fx) / 4);
                r = r * trans / 255u + add0;
                g = g * trans / 255u + add1;
                b = b * trans / 255u + add2;
            }
        }
        if (warmth > 0u) {
            const unsigned horizontal = sunset_left
                ? 255u - x * 96u / (LUM_WIDTH - 1u)
                : 159u + x * 96u / (LUM_WIDTH - 1u);
            const unsigned glow = glow_row * horizontal / 255u;
            r = (r * (255u - glow) + 248u * glow) / 255u;
            g = (g * (255u - glow) + 126u * glow) / 255u;
            b = (b * (255u - glow) + 78u * glow) / 255u;
        }
        if (mode == 1u) {
            r = r * light / 255u; g = g * light / 255u; b = b * light / 255u;
        } else if (mode == 2u) {
            r = r * 15u / 100u; g = g * 24u / 100u; b = b * 34u / 100u;
        } else if (mode == 3u) {
            r = r * 7u / 100u; g = g * 12u / 100u; b = b * 22u / 100u;
        }
        out[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
}

/* ---- water (firmware: render_water_row) */

static void render_water_row(uint32_t *out, const uint8_t *base, unsigned y,
                             unsigned warmth)
{
    const size_t row_pixel = (size_t)y * LUM_WIDTH;
    const int reach = (int)(y - LUM_HORIZON);
    const int taper_q8 = reach >= 160 ? 256 : reach * 256 / 160;
    unsigned glow = 0u;
    if (warmth > 0u) {
        const unsigned distance = y - LUM_HORIZON;
        const unsigned vertical = distance < 170u ? (170u - distance) * 255u / 170u : 0u;
        glow = warmth * vertical / 255u * 128u / 255u;
    }
    const unsigned sun_mode = conditions.sun_mode;

    for (unsigned x = 0; x < LUM_WIDTH; ++x) {
        const size_t pixel = row_pixel + x;
        const uint8_t *src;
        if (!water_pixel(pixel)) {
            src = assets.base_rgb + pixel * 3u;
            out[x] = 0xFF000000u | ((unsigned)src[2] << 16) |
                     ((unsigned)src[1] << 8) | src[0];
            continue;
        }
        const unsigned px = x >> 1u;
        int shade = (int)wave_shade_row[px];

        /* Per-pixel detail -- fine surface shimmer, deliberately subtle. Two
         * earlier bugs made it read as "blocks of tint": near the camera the
         * field was sampled 1:1 so a 256-tile texture repeated every 256 px in
         * big diamonds, and its amplitude swamped the shade. Now it is sampled
         * at a high fixed frequency (features a handful of pixels wide, so any
         * repeat of the 512 field is fine texture, never blocks) from a larger
         * field, and its amplitude is a small fraction of a shade level. Two
         * layers at coprime-ish steps drift apart so nothing pulses. */
        const unsigned energy = 12u + ((unsigned)(shade < 0 ? -shade : shade) >> 2);
        const unsigned n0 = ((((unsigned)x * 5u + detail_scroll_x0) & (DETAIL_N - 1)) +
                             ((((unsigned)y * 5u + detail_scroll_y0) & (DETAIL_N - 1)) << 9));
        const unsigned n1 = ((((unsigned)x * 7u + detail_scroll_x1) & (DETAIL_N - 1)) +
                             ((((unsigned)y * 7u + detail_scroll_y1) & (DETAIL_N - 1)) << 9));
        const int detail = detail_grad[n0] + (detail_grad[n1] >> 1);
        shade += detail * (int)energy >> 9;

        int quantised = shade + 128;
        if (quantised < 0) quantised = 0;
        if (quantised > 255) quantised = 255;
        const unsigned si = (unsigned)quantised >> 4u;
        const unsigned si2 = si < 15u ? si + 1u : si;   /* next shade grade */
        const unsigned frac = (unsigned)quantised & 15u; /* blend fraction */

        int sx = (int)x + (wave_dx_row[px] * taper_q8 >> 8);
        if (sx < 0) sx = 0;
        if (sx >= LUM_WIDTH) sx = LUM_WIDTH - 1;
        int sy = (int)y + (wave_dy_row[px] * taper_q8 >> 8);
        if (sy < LUM_HORIZON) sy = LUM_HORIZON;
        if (sy >= LUM_HEIGHT) sy = LUM_HEIGHT - 1;
        src = base + ((size_t)sy * LUM_WIDTH + (size_t)sx) * 3u;
        /* Interpolate between the two neighbouring shade grades by `frac`
         * instead of snapping to one of 16 -- otherwise a smooth swell shows
         * the level boundaries as broad flat tint bands. */
        unsigned r = (wave_color_lut[0][si][src[0]] * (16u - frac) +
                      wave_color_lut[0][si2][src[0]] * frac) >> 4;
        unsigned g = (wave_color_lut[1][si][src[1]] * (16u - frac) +
                      wave_color_lut[1][si2][src[1]] * frac) >> 4;
        unsigned b = (wave_color_lut[2][si][src[2]] * (16u - frac) +
                      wave_color_lut[2][si2][src[2]] * frac) >> 4;

        const unsigned sim_foam = wave_foam_row[px];
        if (sim_foam != 255u) {
            if (sim_foam > 0u) {
                const unsigned f = sim_foam > 240u ? 240u : sim_foam;
                r += (255u - r) * f >> 8;
                g += (255u - g) * f >> 8;
                b += (255u - b) * f >> 8;
            }
        } else {
            const uint8_t shore = assets.shore_distance[pixel];
            if (shore < 36u) {
                const int crest =
                    wave_crest_lut[(uint8_t)(wave_breaker_phase_row[px] + shore * 5u)];
                if (crest > 58) {
                    unsigned f = (unsigned)((crest - 58) * (36u - shore)) / 48u;
                    if (f > 240u) f = 240u;
                    r += (255u - r) * f >> 8;
                    g += (255u - g) * f >> 8;
                    b += (255u - b) * f >> 8;
                }
            }
        }
        if (warmth > 0u) {
            r = (r * (255u - glow) + 220u * glow) / 255u;
            g = (g * (255u - glow) + 102u * glow) / 255u;
            b = (b * (255u - glow) + 68u * glow) / 255u;
            if (sun_mode == 2u) {
                r = r * 34u / 100u; g = g * 42u / 100u; b = b * 52u / 100u;
            } else if (sun_mode == 3u) {
                r = r * 18u / 100u; g = g * 24u / 100u; b = b * 34u / 100u;
            }
        }
        out[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
}

void lum_render_frame(uint32_t *pixels, int stride_px, uint64_t elapsed_ms)
{
    pthread_mutex_lock(&state_lock);
    build_color_lut();
    int total_weight = 0;
    build_wave_tables(elapsed_ms, &total_weight);
    const int shade_recip_q16 = total_weight > 0 ? 65536 / (total_weight * 4) : 0;
    const unsigned warmth = sunset_warmth_255();

    /* Advance the two detail layers ALONG THE LIVE SWELL so the surface visibly
     * drifts with the real conditions rather than on a fixed vector. The
     * dominant component's from_deg, relative to the shore normal (90 deg, due
     * east), sets the on-screen drift: shoreward is DOWN the frame (toward the
     * viewer) and the alongshore part maps to screen-right for a swell from the
     * south of east -- right for SE/S, left for NE/N, straight onshore for a
     * due-east swell (see config/nubble-sea-projection.json: camera yaw 83 deg,
     * so screen-right ~= south). Drift speed scales mildly with wave height.
     * The second layer is nudged perpendicular so the interference never
     * repeats (no scrolling-texture tiling). */
    const int from_deg = conditions.wave_count > 0u ? conditions.waves[0].from_deg : 137;
    const float rel = (float)(from_deg - 90) * (float)M_PI / 180.0f;
    const float ux = sinf(rel);                 /* screen-right (alongshore) */
    float uy = cosf(rel);                        /* screen-down (shoreward)  */
    if (uy < 0.15f) uy = 0.15f;                  /* always some onshore march */
    const float px = -uy, py = ux;               /* unit perpendicular       */
    const int h_mm = conditions.wave_count > 0u ? (int)conditions.waves[0].height_mm : 800;
    float spd = (float)h_mm / 1200.0f;           /* mild height scaling       */
    if (spd < 0.5f) spd = 0.5f; else if (spd > 2.2f) spd = 2.2f;
    const float e = (float)elapsed_ms / 1000.0f;
    detail_scroll_x0 = (int)(e * spd * (34.0f * ux));
    detail_scroll_y0 = (int)(e * spd * (34.0f * uy));
    detail_scroll_x1 = (int)(e * spd * (22.0f * ux + 8.0f * px));
    detail_scroll_y1 = (int)(e * spd * (22.0f * uy + 8.0f * py));

    int shift_x[3] = {0, 0, 0}, shift_y[3] = {0, 0, 0};
    if (conditions.cloud_cover_permille > 0u) {
        static const uint32_t heights_m[3] = {SHELL_HIGH_M, SHELL_MID_M, SHELL_LOW_M};
        const int64_t t = (int64_t)(elapsed_ms % 21600000ull);
        for (unsigned s = 0; s < 3u; ++s) {
            const int64_t h_mm = (int64_t)(conditions.shells[s].height_m
                                               ? conditions.shells[s].height_m
                                               : heights_m[s]) * 1000ll;
            shift_x[s] = (int)(-(int64_t)conditions.shells[s].wind_north_mmps *
                t * CLOUD_W * 256ll / (h_mm * 1919ll) >> 8);
            shift_y[s] = (int)((int64_t)conditions.shells[s].wind_east_mmps *
                t * CLOUD_H * 256ll / (h_mm * 1187ll) >> 8);
        }
    }

    pthread_mutex_lock(&sim_lock);
    memcpy(normal_snapshot, sim_normal, 2u * OCEAN_CELLS);
    memcpy(foam_snapshot, sim_foam, OCEAN_CELLS);
    pthread_mutex_unlock(&sim_lock);

    const ocean_map_t *map = (const ocean_map_t *)assets.ocean_map;
    for (unsigned y = 0; y < LUM_HEIGHT; ++y) {
        uint32_t *out = pixels + (size_t)y * stride_px;
        const uint8_t *base_row = assets.base_rgb + (size_t)y * LUM_WIDTH * 3u;
        if (y < LUM_HORIZON) {
            render_sky_row(out, base_row, y, shift_x, shift_y);
            continue;
        }
        if (((y & 1u) == 0u) || y == LUM_HORIZON) {
            const unsigned map_row = (y >> 1u) - OCEAN_MAP_ROW0;
            if (map_row < OCEAN_MAP_ROWS) {
                compute_wave_row(map + (size_t)map_row * OCEAN_MAP_W * 2u,
                                 shade_recip_q16);
            }
        }
        render_water_row(out, assets.base_rgb, y, warmth);
    }
    pthread_mutex_unlock(&state_lock);
}
