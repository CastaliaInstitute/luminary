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

#define OCEAN_MAP_W 512
#define OCEAN_MAP_ROW0 145
#define OCEAN_MAP_ROWS 155
#define OCEAN_MAP_NONE 0xFFFFu
#define CLOUD_W 256
#define CLOUD_H 96
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

static uint8_t cloud_x_lut[LUM_WIDTH];
static uint8_t cloud_y_lut[LUM_HORIZON];
static uint8_t cloud_row_trans[LUM_WIDTH / 4];
static uint8_t cloud_row_add[(LUM_WIDTH / 4) * 3];

static inline bool water_pixel(size_t pixel)
{
    return (assets.water_mask[pixel >> 3] >> (pixel & 7u)) & 1u;
}

static inline uint8_t clamp_channel(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Authored frame's mean sky chroma, from the firmware's constants. */
#define AUTHORED_SKY_R 168
#define AUTHORED_SKY_G 208
#define AUTHORED_SKY_B 228

bool lum_init(const lum_assets_t *bound)
{
    assets = *bound;
    for (unsigned i = 0; i < 256; ++i) {
        wave_sine[i] = (int8_t)lrintf(127.0f * sinf((float)i * 6.28318530718f / 256.0f));
    }
    for (unsigned x = 0; x < LUM_WIDTH; ++x) {
        cloud_x_lut[x] = (uint8_t)(x * CLOUD_W / LUM_WIDTH);
    }
    for (unsigned y = 0; y < LUM_HORIZON; ++y) {
        cloud_y_lut[y] = (uint8_t)(y * CLOUD_H / LUM_HORIZON);
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

static void compute_wave_row(const uint16_t *map_row, int shade_recip_q16)
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
        const int fx = (int)(gx_q8 & 0xFFu), fy = (int)(gy_q8 & 0xFFu);
        const size_t c00 = (size_t)cy * OCEAN_NX + cx;
        const size_t c10 = c00 + OCEAN_NX;
        const int n00 = normal_snapshot[2u * c00 + 1u];
        const int n01 = normal_snapshot[2u * (c00 + 1u) + 1u];
        const int n10 = normal_snapshot[2u * c10 + 1u];
        const int n11 = normal_snapshot[2u * (c10 + 1u) + 1u];
        const int top = n00 + ((n01 - n00) * fx >> 8);
        const int bottom = n10 + ((n11 - n10) * fx >> 8);
        const int gradient = top + ((bottom - top) * fy >> 8);
        const int gradient_x = normal_snapshot[2u * c00];

        int shade = -gradient * 24;
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

static inline int wrap_cloud_x(int v) { return v & (CLOUD_W - 1); }

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
        const uint8_t *atlases[3] = {assets.cloud_high, assets.cloud_mid, assets.cloud_low};
        for (unsigned shell = 0; shell < 3u; ++shell) {
            const int atlas_y = mirror_cloud_y((int)cloud_y_lut[y] + shift_y[shell]);
            const uint8_t *atlas_row = atlases[shell] + (size_t)atlas_y * CLOUD_W * 2u;
            const unsigned bias = conditions.shells[shell].blue_bias;
            const unsigned half_bias = bias / 2u;
            const int off = shift_x[shell];
            for (unsigned q = 0; q < LUM_WIDTH / 4u; ++q) {
                const unsigned ax = (unsigned)wrap_cloud_x((int)cloud_x_lut[q * 4u] + off);
                const uint8_t *texel = atlas_row + (size_t)ax * 2u;
                unsigned alpha = texel[1] * cover / 1000u;
                alpha = alpha * feather / 22u;
                if (alpha == 0u) continue;
                const unsigned lum = texel[0];
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

    for (unsigned x = 0; x < LUM_WIDTH; ++x) {
        const uint8_t *src = base_row + (size_t)x * 3u;
        unsigned r = clamp_channel((int)src[0] + shift_r);
        unsigned g = clamp_channel((int)src[1] + shift_g);
        unsigned b = clamp_channel((int)src[2] + shift_b);
        if (conditions.cloud_cover_permille > 0u) {
            const unsigned q = x >> 2u;
            const unsigned trans = cloud_row_trans[q];
            if (trans != 255u) {
                const uint8_t *add = cloud_row_add + (size_t)q * 3u;
                r = r * trans / 255u + add[0];
                g = g * trans / 255u + add[1];
                b = b * trans / 255u + add[2];
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
        int quantised = (int)wave_shade_row[px] + 128 +
                        (int)shade_dither[y & 3u][x & 3u] - 8;
        if (quantised < 0) quantised = 0;
        if (quantised > 255) quantised = 255;
        const unsigned si = (unsigned)quantised >> 4u;

        int sx = (int)x + (wave_dx_row[px] * taper_q8 >> 8);
        if (sx < 0) sx = 0;
        if (sx >= LUM_WIDTH) sx = LUM_WIDTH - 1;
        int sy = (int)y + (wave_dy_row[px] * taper_q8 >> 8);
        if (sy < LUM_HORIZON) sy = LUM_HORIZON;
        if (sy >= LUM_HEIGHT) sy = LUM_HEIGHT - 1;
        src = base + ((size_t)sy * LUM_WIDTH + (size_t)sx) * 3u;
        unsigned r = wave_color_lut[0][si][src[0]];
        unsigned g = wave_color_lut[1][si][src[1]];
        unsigned b = wave_color_lut[2][si][src[2]];

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

    const uint16_t *map = (const uint16_t *)assets.ocean_map;
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
