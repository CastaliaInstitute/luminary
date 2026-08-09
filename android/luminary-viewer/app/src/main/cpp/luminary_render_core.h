/* Portable core of the Luminary sea renderer.
 *
 * This is the pipeline validated on the ESP32-P4, freed of that platform's
 * constraints: the shallow-water solver drives shading, refraction and foam
 * over the authored Nubble photograph, exactly as on the panel. The pixel
 * format here is RGBX8888 (Android ANativeWindow) rather than the panel's
 * BGR888, and there is no IRAM, no DIRAM ceiling, and no PSRAM thermal
 * regime to design around -- a mid-range phone runs the whole frame in a
 * few milliseconds.
 *
 * The solver itself is tools/ocean-sim/ocean_sim.c, compiled verbatim.
 */
#ifndef LUMINARY_RENDER_CORE_H
#define LUMINARY_RENDER_CORE_H

#include <stdint.h>
#include <stdbool.h>

/* Double the P4 panel's 1024x600: everything upstream of the base
 * photograph is resolution-independent (continuous camera fit, world-space
 * solver, per-pixel shading), so the phone renders the same scene at 2x and
 * the compositor's slight downscale to the glass supersamples it. */
#define LUM_WIDTH  2048
#define LUM_HEIGHT 1200
#define LUM_HORIZON 582

/* All buffers are caller-owned copies of the repo's runtime assets. */
typedef struct {
    const uint8_t *base_rgb;        /* LUM_WIDTH*LUM_HEIGHT*3, RGB, decoded from the JPEG */
    const uint8_t *water_mask;      /* LUM_WIDTH*LUM_HEIGHT/8, LSB-first bits */
    const uint8_t *land_mask;       /* LUM_WIDTH*LUM_HEIGHT/8: island, rocks,
                                     * lighthouse, foreground -- the solid
                                     * placeholder for the 3D print, passed
                                     * through the photo untouched. */
    const uint8_t *shore_distance;  /* LUM_WIDTH*LUM_HEIGHT */
    const uint8_t *ocean_map;       /* 512*155 pairs of Q8.8 uint16 LE */
    const uint8_t *ocean_depth;     /* OCEAN_CELLS depth codes */
    const uint8_t *cloud_low;       /* 256*96*2 luminance+alpha */
    const uint8_t *cloud_mid;
    const uint8_t *cloud_high;
} lum_assets_t;

typedef struct {
    /* From the runtime scene state; same meaning as the firmware fields. */
    uint8_t sky_r, sky_g, sky_b;
    uint8_t sun_mode;               /* 0 day, 1 civil, 2 nautical, 3 night */
    int32_t sun_altitude_deci_deg;
    int32_t sun_relative_azimuth_deci_deg;
    uint16_t cloud_cover_permille;
    struct { uint32_t height_mm, period_ms; int32_t from_deg; } waves[3];
    uint32_t wave_count;
    struct { int32_t wind_east_mmps, wind_north_mmps; uint32_t height_m; uint8_t blue_bias; } shells[3];
} lum_conditions_t;

/* One-time setup: binds assets, allocates working memory, prepares the
 * solver on the bundled bathymetry. Returns false on allocation failure. */
bool lum_init(const lum_assets_t *assets);

/* Apply (possibly updated) scene conditions. Safe between frames. */
void lum_set_conditions(const lum_conditions_t *conditions);

/* Advance the solver by one tick (call at ~20-30 Hz from its own thread;
 * internally locked against lum_render_frame). */
void lum_solver_tick(void);
uint32_t lum_solver_tick_ms(void);

/* Render the scene at elapsed_ms into an RGBX8888 buffer with the given
 * stride in PIXELS. Thread-safe against lum_solver_tick. */
void lum_render_frame(uint32_t *pixels, int stride_px, uint64_t elapsed_ms);

#endif
