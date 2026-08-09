/* Headless host renderer: bind the shipped Luminary assets, develop the solver
 * a few hundred ticks, render one native 2048x1200 frame and write it as a PPM.
 *
 * This is the deterministic input for scripts/validate-tiling-seams.py -- unlike
 * a device screenshot it has no letterbox bars, no watermark and no panel
 * downscale moire, so the seam/tiling detector sees only what the renderer
 * actually produced, aligned exactly to the water and land masks.
 *
 * Build (same grid dimensions as the Android CMake):
 *   clang -O2 -DOCEAN_NX=384u -DOCEAN_NY=384u \
 *     -I android/luminary-viewer/app/src/main/cpp -I tools/ocean-sim \
 *     tools/host-render/render_frame.c \
 *     android/luminary-viewer/app/src/main/cpp/luminary_render_core.c \
 *     tools/ocean-sim/ocean_sim.c -lm -lpthread -o /tmp/render_frame
 *
 *   render_frame <assets_dir> <base.rgb> <out.ppm>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luminary_render_core.h"

static uint8_t *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(2); }
    fclose(f);
    return b;
}

static uint8_t *asset(const char *dir, const char *name)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return slurp(p);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <assets_dir> <base.rgb> <out.ppm> [extra_ticks] [elapsed_ms]\n", argv[0]);
        return 1;
    }
    const char *dir = argv[1];

    lum_assets_t a;
    memset(&a, 0, sizeof a);
    a.base_rgb       = slurp(argv[2]);
    a.water_mask     = asset(dir, "nubble_runtime_water_mask.bin");
    a.land_mask      = asset(dir, "nubble_runtime_land_mask.bin");
    a.shore_distance = asset(dir, "nubble_runtime_shore_distance.bin");
    a.ocean_map      = asset(dir, "nubble_runtime_ocean_map.bin");
    a.ocean_depth    = asset(dir, "nubble_runtime_ocean_depth.bin");
    a.cloud_low      = asset(dir, "nubble_runtime_cloud_low.bin");
    a.cloud_mid      = asset(dir, "nubble_runtime_cloud_mid.bin");
    a.cloud_high     = asset(dir, "nubble_runtime_cloud_high.bin");

    if (!lum_init(&a)) { fprintf(stderr, "lum_init failed\n"); return 3; }

    lum_conditions_t c;
    memset(&c, 0, sizeof c);
    c.sky_r = 170; c.sky_g = 205; c.sky_b = 232;
    c.sun_mode = 0;                        /* day */
    c.sun_altitude_deci_deg = 420;
    c.sun_relative_azimuth_deci_deg = 200;
    c.cloud_cover_permille = 600;          /* clouds visible for the sky check */
    c.wave_count = 2;
    /* Vigorous surf: the near-field refraction/faceting bugs only manifest when
     * the wave field has strong, spatially-varying gradients, so the validation
     * frame must be driven hard, not calm. */
    c.waves[0].height_mm = 3000; c.waves[0].period_ms = 9000; c.waves[0].from_deg = 135;
    c.waves[1].height_mm = 1800; c.waves[1].period_ms = 7000; c.waves[1].from_deg = 105;
    /* Optional argv[6] overrides the dominant swell direction (deg FROM), for
     * verifying that the shimmer drift tracks it. */
    if (argc > 6) c.waves[0].from_deg = atoi(argv[6]);
    for (int s = 0; s < 3; ++s) {
        c.shells[s].wind_east_mmps = 5000 - s * 1500;
        c.shells[s].wind_north_mmps = 2000 - s * 500;
        c.shells[s].height_m = 6000 - s * 2400;
    }
    lum_set_conditions(&c);

    /* Optional argv[4] = extra solver ticks before render (default 0), argv[5]
     * = elapsed_ms for the render (default 12000). Two runs a few ticks apart
     * let a caller measure which way the surf actually drifts on screen. */
    const int extra = argc > 4 ? atoi(argv[4]) : 0;
    const uint64_t elapsed = argc > 5 ? (uint64_t)strtoull(argv[5], NULL, 10) : 12000;
    /* Optional argv[7] = a directory of raw cloud atlases to HOT-SWAP in via
     * lum_set_clouds (exercises the live-cloud path). */
    if (argc > 7) {
        const char *cd = argv[7];
        uint8_t *lo = asset(cd, "nubble_runtime_cloud_low.bin");
        uint8_t *md = asset(cd, "nubble_runtime_cloud_mid.bin");
        uint8_t *hi = asset(cd, "nubble_runtime_cloud_high.bin");
        lum_set_clouds(lo, md, hi);
        fprintf(stderr, "hot-swapped clouds from %s\n", cd);
    }

    for (int i = 0; i < 400 + extra; ++i) lum_solver_tick();

    uint32_t *fb = malloc((size_t)LUM_WIDTH * LUM_HEIGHT * 4);
    lum_render_frame(fb, LUM_WIDTH, elapsed);

    FILE *o = fopen(argv[3], "wb");
    if (!o) { fprintf(stderr, "cannot write %s\n", argv[3]); return 4; }
    fprintf(o, "P6\n%d %d\n255\n", LUM_WIDTH, LUM_HEIGHT);
    const uint8_t *px = (const uint8_t *)fb;   /* RGBX8888: b0=R b1=G b2=B */
    for (size_t i = 0; i < (size_t)LUM_WIDTH * LUM_HEIGHT; ++i) {
        fputc(px[i * 4 + 0], o);
        fputc(px[i * 4 + 1], o);
        fputc(px[i * 4 + 2], o);
    }
    fclose(o);
    fprintf(stderr, "wrote %s (%dx%d)\n", argv[3], LUM_WIDTH, LUM_HEIGHT);
    return 0;
}
