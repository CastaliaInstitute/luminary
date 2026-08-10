/* WebAssembly entry points for the Luminary PWA. Wraps the SAME portable core
 * the Android app and P4 firmware use (luminary_render_core.c + ocean_sim.c) so
 * the browser renders bit-identical water and sky -- one source of truth.
 *
 * Single-threaded on purpose: GitHub Pages does not send the COOP/COEP headers
 * that SharedArrayBuffer/pthreads require, so the JS loop ticks the solver and
 * renders sequentially. The core's pthread mutexes resolve to Emscripten's
 * no-op stubs, which is correct with no real concurrency.
 *
 * The core writes RGBX8888 whose little-endian bytes are r,g,b,255 -- already
 * the layout of a canvas ImageData, so JS blits the framebuffer with no swizzle.
 */
#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "luminary_render_core.h"

static uint32_t *g_fb;

EMSCRIPTEN_KEEPALIVE int lum_wasm_width(void)  { return LUM_WIDTH; }
EMSCRIPTEN_KEEPALIVE int lum_wasm_height(void) { return LUM_HEIGHT; }
EMSCRIPTEN_KEEPALIVE int lum_wasm_cloud_bytes(void) { return (int)lum_cloud_atlas_bytes(); }

/* Assets are copied into wasm memory by JS (via _malloc) and remain owned by JS;
 * the core reads them in place, so they must outlive the session. */
EMSCRIPTEN_KEEPALIVE int lum_wasm_init(
    uint8_t *base, uint8_t *water, uint8_t *land, uint8_t *shore,
    uint8_t *map, uint8_t *depth, uint8_t *cloud_low, uint8_t *cloud_mid,
    uint8_t *cloud_high)
{
    lum_assets_t a = {
        .base_rgb = base, .water_mask = water, .land_mask = land,
        .shore_distance = shore, .ocean_map = map, .ocean_depth = depth,
        .cloud_low = cloud_low, .cloud_mid = cloud_mid, .cloud_high = cloud_high,
    };
    if (!lum_init(&a)) return 0;
    g_fb = malloc((size_t)LUM_WIDTH * LUM_HEIGHT * 4);
    return g_fb != NULL;
}

EMSCRIPTEN_KEEPALIVE uint8_t *lum_wasm_framebuffer(void) { return (uint8_t *)g_fb; }

EMSCRIPTEN_KEEPALIVE void lum_wasm_tick(void) { lum_solver_tick(); }

EMSCRIPTEN_KEEPALIVE void lum_wasm_render(double elapsed_ms)
{
    if (g_fb) lum_render_frame(g_fb, LUM_WIDTH, (uint64_t)elapsed_ms);
}

/* waves: wave_count triples of (height_mm, period_ms, from_deg).
 * shells: 3 quads of (wind_east_mmps, wind_north_mmps, height_m, blue_bias). */
EMSCRIPTEN_KEEPALIVE void lum_wasm_set_conditions(
    int sky_r, int sky_g, int sky_b, int sun_mode, int sun_alt_deci,
    int sun_az_deci, int cloud_permille, int wave_count,
    const int32_t *waves, const int32_t *shells)
{
    lum_conditions_t c;
    memset(&c, 0, sizeof c);
    c.sky_r = (uint8_t)sky_r; c.sky_g = (uint8_t)sky_g; c.sky_b = (uint8_t)sky_b;
    c.sun_mode = (uint8_t)sun_mode;
    c.sun_altitude_deci_deg = sun_alt_deci;
    c.sun_relative_azimuth_deci_deg = sun_az_deci;
    c.cloud_cover_permille = (uint16_t)cloud_permille;
    c.wave_count = (uint32_t)(wave_count > 3 ? 3 : (wave_count < 0 ? 0 : wave_count));
    for (unsigned i = 0; i < c.wave_count; ++i) {
        c.waves[i].height_mm = (uint32_t)waves[i * 3];
        c.waves[i].period_ms = (uint32_t)waves[i * 3 + 1];
        c.waves[i].from_deg = waves[i * 3 + 2];
    }
    for (int s = 0; s < 3; ++s) {
        c.shells[s].wind_east_mmps = shells[s * 4];
        c.shells[s].wind_north_mmps = shells[s * 4 + 1];
        c.shells[s].height_m = (uint32_t)shells[s * 4 + 2];
        c.shells[s].blue_bias = (uint8_t)shells[s * 4 + 3];
    }
    lum_set_conditions(&c);
}

/* Hot-swap freshly fetched cloud atlases (each lum_wasm_cloud_bytes() long). */
EMSCRIPTEN_KEEPALIVE void lum_wasm_set_clouds(uint8_t *low, uint8_t *mid, uint8_t *high)
{
    lum_set_clouds(low, mid, high);
}
