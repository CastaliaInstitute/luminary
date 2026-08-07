/*
 * Luminary animation viewer: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B only.
 *
 * 7-inch landscape EK79007 MIPI-DSI panel: 1024 x 600, two DSI lanes,
 * reset GPIO 33. There is deliberately no 5-inch or portrait fallback.
 */

#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/jpeg_decode.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "cJSON.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_crc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "ocean_sim.h"
#include "luminary_runtime_state.h"
#include "hipparcos_stars.h"

#define LUMINARY_WIDTH                  1024U
#define LUMINARY_HEIGHT                  600U
#define LUMINARY_BPP                        3U
#define LUMINARY_FRAME_BYTES (LUMINARY_WIDTH * LUMINARY_HEIGHT * LUMINARY_BPP)
#define LUMINARY_DECODE_HEIGHT             608U /* JPEG DMA output is 16-line aligned. */
#define LUMINARY_DECODE_BYTES (LUMINARY_WIDTH * LUMINARY_DECODE_HEIGHT * LUMINARY_BPP)
#define LUMINARY_WAVE_CYCLE_MAX_BYTES       (10U * 1024U * 1024U)
#define LUMINARY_WAVE_CYCLE_MAX_FRAME_BYTES  (2U * 1024U * 1024U)

#define LUMINARY_LCD_RESET_GPIO           33
#define LUMINARY_MIPI_LDO_CHANNEL          3
#define LUMINARY_MIPI_LDO_VOLTAGE_MV    2500
#define LUMINARY_MIPI_DSI_LANES            2

#define LUMV_MAGIC 0x564d554cU /* ASCII LUMV, little-endian */
#define LUMV_VERSION 1U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint16_t width;
    uint16_t height;
    uint32_t fps_milli;
    uint32_t frame_count;
    uint32_t index_offset;
    uint32_t data_offset;
} lumv_header_t;

typedef struct __attribute__((packed)) {
    uint32_t offset; /* Absolute offset from the beginning of the LUMV blob. */
    uint32_t length;
} lumv_frame_t;

typedef struct {
    uint8_t *payload;
    size_t payload_bytes;
    const lumv_frame_t *frames;
    uint32_t frame_count;
    uint32_t fps_milli;
} wave_cycle_t;

extern const uint8_t nubble_runtime_base_jpg_start[] asm("_binary_nubble_runtime_base_jpg_start");
extern const uint8_t nubble_runtime_base_jpg_end[] asm("_binary_nubble_runtime_base_jpg_end");
extern const uint8_t nubble_runtime_water_mask_bin_start[] asm("_binary_nubble_runtime_water_mask_bin_start");
extern const uint8_t nubble_runtime_shore_distance_bin_start[] asm("_binary_nubble_runtime_shore_distance_bin_start");
extern const uint8_t nubble_runtime_ocean_phase_bin_start[] asm("_binary_nubble_runtime_ocean_phase_bin_start");
extern const uint8_t nubble_runtime_cloud_low_bin_start[] asm("_binary_nubble_runtime_cloud_low_bin_start");
extern const uint8_t nubble_runtime_cloud_mid_bin_start[] asm("_binary_nubble_runtime_cloud_mid_bin_start");
extern const uint8_t nubble_runtime_cloud_high_bin_start[] asm("_binary_nubble_runtime_cloud_high_bin_start");

static const char *TAG = "luminary-anim";
/* PSRAM copies of the two per-pixel lookup assets the water pass reads. The
 * embedded originals live in flash, and per-pixel flash data reads from the
 * IRAM row loops go through the flash cache path -- the same one that made
 * flash-resident code catastrophically slow here. PSRAM reads share the
 * ordinary data cache with everything else the pass touches. */
static const uint8_t *water_mask_ram;
static const uint8_t *shore_distance_ram;
/* Read only while building the per-frame wave tables (about a thousand reads
 * a frame), so it lives in PSRAM: internal RAM here is what decides whether
 * the firmware boots. This image boot-loops in ESP-Hosted startup once static
 * DIRAM passes roughly 133 KB. */
static int8_t *wave_sine;
/* Hot per-pixel row tables: internal RAM for speed, but from the heap at
 * renderer start rather than .bss -- the static image sits against the
 * ~133 KB DIRAM ceiling where ESP-Hosted startup boot-loops. */
static int8_t *wave_shade_row;
static uint8_t *wave_breaker_phase_row;
/* Three cloud shells composed into one screen-space layer, per half-res
 * column: transmission of the stack and the premultiplied colour it adds.
 * Turns three per-pixel alpha blends across 291 rows into one -- the shells
 * only vary at atlas resolution (256 texels across 1024 pixels), so nothing
 * visible is lost at half-column granularity. Internal heap, allocated after
 * the boot-critical window like the other row scratch. */
static uint8_t *cloud_row_trans;
static uint8_t *cloud_row_add; /* interleaved b, g, r per half-res column */
/* 4x4 ordered dither for the shade quantisation, 0..15. Read once per water
 * pixel from an IRAM loop, so it must be RAM: written at renderer start
 * precisely so the compiler cannot const-promote it into flash rodata --
 * which it did to the initialised version of this table. */
static uint8_t shade_dither[4][4];
#if CONFIG_LUMINARY_OCEAN_SIM
/* Per-cell foam from the shallow-water solver, 0..254; 255 means "no solver
 * data here, use the analytic crest heuristic". Read per water pixel, so it
 * must be internal RAM -- but from the heap at renderer start, not .bss:
 * the static image sits against the boot DIRAM ceiling. */
static uint8_t *wave_foam_row;
/* Per-cell texture displacement in panel pixels, from the solver's surface
 * gradient. Shading alone reads as a light wash over a still photograph --
 * the authored texture's own chop stays frozen. Refracting the source
 * lookup through the moving surface makes that texture itself move. */
static int8_t *wave_dx_row;
static int8_t *wave_dy_row;
#endif

/* Per-phase render profiling, off by default. Each phase of the frame is
 * timed and reported alongside the cadence log, which is how the render budget
 * was attributed; enable CONFIG_LUMINARY_RENDER_PROFILING to get it back. The
 * timing calls are compiled out entirely when it is off, so the shipping path
 * pays nothing for them. */
#if CONFIG_LUMINARY_RENDER_PROFILING
static int64_t prof_basecopy_us, prof_lut_us, prof_wave_us,
               prof_sky_us, prof_water_us, prof_stars_us, prof_fbcopy_us;
#define PROF_STAMP(name)        const int64_t name = esp_timer_get_time()
#define PROF_STAMP_MUT(name)    int64_t name = esp_timer_get_time()
#define PROF_RESTAMP(name)      name = esp_timer_get_time()
#define PROF_ADD(acc, from, to) ((acc) += (to) - (from))
#define PROF_SET(acc, from)     ((acc) = esp_timer_get_time() - (from))
#define PROF_ZERO(...)          do { __VA_ARGS__; } while (0)
#else
#define PROF_STAMP(name)        ((void)0)
#define PROF_STAMP_MUT(name)    ((void)0)
#define PROF_RESTAMP(name)      ((void)0)
#define PROF_ADD(acc, from, to) ((void)0)
#define PROF_SET(acc, from)     ((void)0)
#define PROF_ZERO(...)          ((void)0)
#endif

/* Measured: esp_async_memcpy (DMA) for this 1.84 MB PSRAM->PSRAM copy came in
 * at 47.0 ms against 45.9 ms for plain memcpy. The transfer is bandwidth
 * bound, not CPU bound, so DMA buys nothing and costs an ISR round trip.
 * Folding the copy into the per-row work is the promising direction instead. */
/* 12 KB, the largest single static object this firmware had. Internal heap
 * at renderer start instead of .bss: same DRAM, same speed, but it no longer
 * counts against the boot-time DIRAM ceiling that ESP-Hosted startup
 * enforces. Indexed as [channel][shade][source]. */
typedef uint8_t wave_color_lut_t[3][16][256];
static wave_color_lut_t *wave_color_lut_mem;
#define wave_color_lut (*wave_color_lut_mem)
/* PSRAM: read three times per quarter-res column by the cloud compose, not
 * per pixel. Freed 1 KB of the internal budget for the IRAM row loop. */
static uint8_t *cloud_x_lut;
/* Three reads per sky row against one per pixel for cloud_x_lut: the row
 * table can afford PSRAM, the column table cannot. */
static uint8_t *cloud_y_lut;

typedef struct {
    uint8_t *atlas; /* Interleaved luminance, alpha. */
    int32_t wind_east_mmps;
    int32_t wind_north_mmps;
    uint32_t height_m;
    uint8_t blue_bias;
} cloud_shell_t;

typedef struct {
    uint32_t height_mm;
    uint32_t period_ms;
} wave_component_t;

typedef struct {
    uint16_t cloud_cover_permille;
    uint8_t sky_r, sky_g, sky_b;
    uint8_t sun_mode; /* 0 day, 1 civil, 2 nautical, 3 night */
    int16_t sun_altitude_deci_deg;
    int16_t sun_relative_azimuth_deci_deg;
    bool moon_visible;
    int16_t moon_x, moon_y;
    uint16_t moon_illumination_permille;
    uint32_t wave_height_mm;
    uint32_t wave_period_ms;
    int32_t wave_kx_q10, wave_ky_q10;
    uint8_t wave_component_count;
    wave_component_t waves[3];
    uint8_t *ocean_phase;
    cloud_shell_t shells[3]; /* high, mid, low */
} runtime_state_t;

static runtime_state_t runtime_state;
static SemaphoreHandle_t runtime_lock;
static wave_cycle_t runtime_wave_cycle;
static const uint8_t *current_framebuffer;

typedef struct {
    int16_t x, y;
    uint8_t intensity;
    uint8_t radius;
} visible_star_t;

#define MAX_VISIBLE_STARS 512U
// Written once per solar update and read once per frame, so it has no
// business occupying internal RAM. Moving it to PSRAM frees 3 KB of the
// internal budget for the IRAM render loops, which do need the speed.
static visible_star_t *visible_stars;
static unsigned visible_star_count;

#define CLOUD_ATLAS_BYTES (LUMINARY_CLOUD_TEXTURE_WIDTH * LUMINARY_CLOUD_TEXTURE_HEIGHT * 2U)
#define OCEAN_PHASE_WIDTH  (LUMINARY_WIDTH / 2U)
#define OCEAN_PHASE_HEIGHT (LUMINARY_HEIGHT / 2U)
#define OCEAN_PHASE_COMPONENTS 3U
#define OCEAN_PHASE_BYTES (OCEAN_PHASE_WIDTH * OCEAN_PHASE_HEIGHT * OCEAN_PHASE_COMPONENTS)
#define RUNTIME_MANIFEST_MAX_BYTES 8192U
#define RUNTIME_STATE_MAX_BYTES    8192U
#define RUNTIME_URL_MAX_BYTES       512U
#define RUNTIME_PATH_MAX_BYTES      160U
#define SD_CACHE_MAGIC       0x434d554cU /* ASCII LUMC, little-endian */
#define SD_CACHE_VERSION             1U
#define SD_CACHE_MOUNT       "/sdcard"
#define SD_CACHE_DIRECTORY   SD_CACHE_MOUNT "/luminary"
/* Keep cache leaf names FAT 8.3-compatible; long-filename support is deliberately
 * disabled in this small embedded build. */
#define SD_CACHE_SLOT_A      SD_CACHE_DIRECTORY "/RUN_A.BIN"
#define SD_CACHE_SLOT_B      SD_CACHE_DIRECTORY "/RUN_B.BIN"
#define YORK_LATITUDE_DEG        43.1637
#define YORK_LONGITUDE_DEG      -70.6480
#define NUBBLE_CAMERA_BEARING_DEG    90.0

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1

static EventGroupHandle_t wifi_events;
static unsigned wifi_retry_count;
static char active_bundle_id[48];
static bool sd_cache_available;
static uint64_t sd_cache_sequence;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint64_t sequence;
    uint32_t payload_crc32;
    uint32_t state_bytes;
    uint32_t cloud_bytes;
    uint32_t ocean_bytes;
    char bundle_id[48];
} sd_cache_header_t;

static bool parse_lumv_cycle(const uint8_t *payload, size_t payload_bytes,
                             uint32_t *frame_count, uint32_t *fps_milli,
                             const lumv_frame_t **frames_out);
static void release_wave_cycle_locked(void);
static bool valid_state_root(cJSON *root);
static void apply_state_root_locked(cJSON *root);

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        if (wifi_retry_count++ < CONFIG_LUMINARY_WIFI_MAXIMUM_RETRY) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        } else {
            xEventGroupSetBits(wifi_events, WIFI_FAILED_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        wifi_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi connected; IPv4=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void start_wifi(void)
{
    if (CONFIG_LUMINARY_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi disabled: no local SSID configured");
        return;
    }

    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_ERR_NO_MEM);

    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(wifi_events ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t station = {0};
    strlcpy((char *)station.sta.ssid, CONFIG_LUMINARY_WIFI_SSID,
            sizeof(station.sta.ssid));
    strlcpy((char *)station.sta.password, CONFIG_LUMINARY_WIFI_PASSWORD,
            sizeof(station.sta.password));
    station.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
    ESP_ERROR_CHECK(esp_wifi_start());
    const esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.cloudflare.com");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_config));
    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID '%s'", CONFIG_LUMINARY_WIFI_SSID);
}

static double wrap_degrees(double value)
{
    value = fmod(value, 360.0);
    return value < 0.0 ? value + 360.0 : value;
}

static void update_visible_stars(double jd, double gmst, double sun_altitude_deg)
{
    visible_star_count = 0;
    double limiting_magnitude;
    if (sun_altitude_deg >= -6.0) return;
    if (sun_altitude_deg < -18.0) {
        limiting_magnitude = 6.5;
    } else if (sun_altitude_deg < -12.0) {
        limiting_magnitude = 4.5 + (-sun_altitude_deg - 12.0) / 6.0 * 2.0;
    } else {
        limiting_magnitude = 1.5 + (-sun_altitude_deg - 6.0) / 6.0 * 3.0;
    }
    const double years = 2000.0 + (jd - 2451545.0) / 365.25 - HIPPARCOS_EPOCH_YEAR;
    const double centuries = (jd - 2451545.0) / 36525.0;
    const double zeta = (2306.2181 * centuries + 0.30188 * centuries * centuries +
                         0.017998 * centuries * centuries * centuries) / 3600.0 * M_PI / 180.0;
    const double zed = (2306.2181 * centuries + 1.09468 * centuries * centuries +
                        0.018203 * centuries * centuries * centuries) / 3600.0 * M_PI / 180.0;
    const double theta = (2004.3109 * centuries - 0.42665 * centuries * centuries -
                          0.041833 * centuries * centuries * centuries) / 3600.0 * M_PI / 180.0;
    const double latitude = YORK_LATITUDE_DEG * M_PI / 180.0;
    for (unsigned index = 0; index < HIPPARCOS_STAR_COUNT; ++index) {
        const hipparcos_star_t *star = &HIPPARCOS_STARS[index];
        const double magnitude = star->vmag_centimag / 100.0;
        if (magnitude > limiting_magnitude) break; /* Catalogue is magnitude sorted. */
        double dec_deg = star->dec_microdeg / 1000000.0 +
                         star->pm_dec_mas_year * years / 3600000.0;
        double dec = dec_deg * M_PI / 180.0;
        const double cos_dec = cos(dec);
        double ra_deg = star->ra_microdeg / 1000000.0;
        if (fabs(cos_dec) > 1e-6) {
            ra_deg += star->pm_ra_mas_year * years / (3600000.0 * cos_dec);
        }
        // Precess the proper-motion-corrected ICRS/J2000 direction to the
        // equator/equinox of date before applying Greenwich sidereal time.
        const double ra0 = ra_deg * M_PI / 180.0;
        const double a = cos(dec) * sin(ra0 + zeta);
        const double b = cos(theta) * cos(dec) * cos(ra0 + zeta) - sin(theta) * sin(dec);
        const double c = sin(theta) * cos(dec) * cos(ra0 + zeta) + cos(theta) * sin(dec);
        ra_deg = wrap_degrees((atan2(a, b) + zed) * 180.0 / M_PI);
        dec = asin(c);
        double hour_angle = wrap_degrees(gmst + YORK_LONGITUDE_DEG - ra_deg) + 180.0;
        hour_angle = hour_angle * M_PI / 180.0 - M_PI;
        const double altitude = asin(sin(latitude) * sin(dec) +
                                     cos(latitude) * cos(dec) * cos(hour_angle));
        if (altitude <= 0.0) continue;
        const double azimuth = wrap_degrees(atan2(
            sin(hour_angle), cos(hour_angle) * sin(latitude) - tan(dec) * cos(latitude)
        ) * 180.0 / M_PI + 180.0);
        double relative = azimuth - NUBBLE_CAMERA_BEARING_DEG;
        while (relative > 180.0) relative -= 360.0;
        while (relative < -180.0) relative += 360.0;
        if (fabs(relative) >= 55.0) continue;
        const int x = (int)lrint(512.0 + 358.53 * tan(relative * M_PI / 180.0));
        const int y = (int)lrint(291.0 - 782.79 * tan(altitude));
        if (x < 0 || x >= (int)LUMINARY_WIDTH || y < 0 || y >= (int)LUMINARY_RUNTIME_HORIZON) continue;
        if (visible_star_count >= MAX_VISIBLE_STARS) break;
        visible_star_t *visible = &visible_stars[visible_star_count++];
        visible->x = (int16_t)x;
        visible->y = (int16_t)y;
        visible->intensity = (uint8_t)fmin(255.0, fmax(18.0,
            255.0 * pow(0.72, magnitude + 1.5)));
        visible->radius = magnitude < 0.5 ? 2U : magnitude < 2.0 ? 1U : 0U;
    }
}

static bool update_solar_position_from_clock(time_t now)
{
    // Reject the unset epoch. The fetched runtime state remains the fallback
    // until SNTP supplies UTC; thereafter the P4 owns solar position locally.
    if (now < 1700000000) return false;
    const double jd = (double)now / 86400.0 + 2440587.5;
    const double d = jd - 2451545.0;
    const double mean_longitude = wrap_degrees(280.460 + 0.9856474 * d);
    const double anomaly = wrap_degrees(357.528 + 0.9856003 * d) * M_PI / 180.0;
    const double ecliptic_longitude = wrap_degrees(
        mean_longitude + 1.915 * sin(anomaly) + 0.020 * sin(2.0 * anomaly)) * M_PI / 180.0;
    const double obliquity = (23.439 - 0.0000004 * d) * M_PI / 180.0;
    const double right_ascension = atan2(cos(obliquity) * sin(ecliptic_longitude),
                                         cos(ecliptic_longitude)) * 180.0 / M_PI;
    const double declination = asin(sin(obliquity) * sin(ecliptic_longitude));
    const double gmst = wrap_degrees(280.46061837 + 360.98564736629 * d);
    double hour_angle = wrap_degrees(gmst + YORK_LONGITUDE_DEG - right_ascension) + 180.0;
    hour_angle = (hour_angle * M_PI / 180.0) - M_PI;
    const double latitude = YORK_LATITUDE_DEG * M_PI / 180.0;
    const double altitude = asin(sin(latitude) * sin(declination) +
                                 cos(latitude) * cos(declination) * cos(hour_angle));
    const double azimuth = wrap_degrees(atan2(
        sin(hour_angle), cos(hour_angle) * sin(latitude) - tan(declination) * cos(latitude)
    ) * 180.0 / M_PI + 180.0);
    double relative = azimuth - NUBBLE_CAMERA_BEARING_DEG;
    while (relative > 180.0) relative -= 360.0;
    while (relative < -180.0) relative += 360.0;
    const double altitude_deg = altitude * 180.0 / M_PI;
    runtime_state.sun_altitude_deci_deg = (int16_t)lrint(altitude_deg * 10.0);
    runtime_state.sun_relative_azimuth_deci_deg = (int16_t)lrint(relative * 10.0);
    runtime_state.sun_mode = altitude_deg >= 0.0 ? 0U :
                             altitude_deg >= -6.0 ? 1U :
                             altitude_deg >= -12.0 ? 2U : 3U;
    update_visible_stars(jd, gmst, altitude_deg);
    return true;
}

static void initialize_wave_lut(void)
{
    wave_color_lut_mem = heap_caps_malloc(sizeof(wave_color_lut_t),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(wave_color_lut_mem ? ESP_OK : ESP_ERR_NO_MEM);
    cloud_x_lut = heap_caps_malloc(LUMINARY_WIDTH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(cloud_x_lut ? ESP_OK : ESP_ERR_NO_MEM);
    wave_sine = heap_caps_malloc(256, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cloud_y_lut = heap_caps_malloc(LUMINARY_RUNTIME_HORIZON,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(wave_sine && cloud_y_lut ? ESP_OK : ESP_ERR_NO_MEM);
    wave_shade_row = heap_caps_malloc(LUMINARY_WIDTH / 2U,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    wave_breaker_phase_row = heap_caps_malloc(LUMINARY_WIDTH / 2U,
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(wave_shade_row && wave_breaker_phase_row ? ESP_OK : ESP_ERR_NO_MEM);
    cloud_row_trans = heap_caps_malloc(LUMINARY_WIDTH / 2U,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    cloud_row_add = heap_caps_malloc((LUMINARY_WIDTH / 2U) * 3U,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(cloud_row_trans && cloud_row_add ? ESP_OK : ESP_ERR_NO_MEM);
#if CONFIG_LUMINARY_OCEAN_SIM
    wave_foam_row = heap_caps_malloc(LUMINARY_WIDTH / 2U,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    wave_dx_row = heap_caps_malloc(LUMINARY_WIDTH / 2U,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    wave_dy_row = heap_caps_malloc(LUMINARY_WIDTH / 2U,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(wave_foam_row && wave_dx_row && wave_dy_row ?
                    ESP_OK : ESP_ERR_NO_MEM);
    memset(wave_foam_row, 255, LUMINARY_WIDTH / 2U);
    memset(wave_dx_row, 0, LUMINARY_WIDTH / 2U);
    memset(wave_dy_row, 0, LUMINARY_WIDTH / 2U);
#endif
    {
        static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6,
                                          3, 11, 1, 9, 15, 7, 13, 5};
        memcpy(shade_dither, bayer, sizeof(bayer));
    }
    {
        uint8_t *mask = heap_caps_malloc(LUMINARY_WIDTH * LUMINARY_HEIGHT / 8U,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t *shore = heap_caps_malloc(LUMINARY_WIDTH * LUMINARY_HEIGHT,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_ERROR_CHECK(mask && shore ? ESP_OK : ESP_ERR_NO_MEM);
        memcpy(mask, nubble_runtime_water_mask_bin_start,
               LUMINARY_WIDTH * LUMINARY_HEIGHT / 8U);
        memcpy(shore, nubble_runtime_shore_distance_bin_start,
               LUMINARY_WIDTH * LUMINARY_HEIGHT);
        water_mask_ram = mask;
        shore_distance_ram = shore;
    }
    for (unsigned index = 0; index < 256; ++index) {
        wave_sine[index] = (int8_t)lrintf(127.0f * sinf((float)index * 6.28318530718f / 256.0f));
    }
    for (unsigned x = 0; x < LUMINARY_WIDTH; ++x) {
        cloud_x_lut[x] = (uint8_t)(x * LUMINARY_CLOUD_TEXTURE_WIDTH / LUMINARY_WIDTH);
    }
    for (unsigned y = 0; y < LUMINARY_RUNTIME_HORIZON; ++y) {
        cloud_y_lut[y] = (uint8_t)(y * LUMINARY_CLOUD_TEXTURE_HEIGHT / LUMINARY_RUNTIME_HORIZON);
    }
}

static void initialize_runtime_state(void)
{
    runtime_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(runtime_lock ? ESP_OK : ESP_ERR_NO_MEM);
    runtime_state = (runtime_state_t) {
        .cloud_cover_permille = LUMINARY_CLOUD_COVER_PERMILLE,
        .sky_r = LUMINARY_SKY_R, .sky_g = LUMINARY_SKY_G, .sky_b = LUMINARY_SKY_B,
        .sun_mode = LUMINARY_SUN_MODE,
        .sun_altitude_deci_deg = LUMINARY_SUN_ALTITUDE_DECI_DEG,
        .sun_relative_azimuth_deci_deg = LUMINARY_SUN_RELATIVE_AZIMUTH_DECI_DEG,
        .moon_visible = LUMINARY_MOON_VISIBLE,
        .moon_x = LUMINARY_MOON_X, .moon_y = LUMINARY_MOON_Y,
        .moon_illumination_permille = LUMINARY_MOON_ILLUMINATION_PERMILLE,
        .wave_height_mm = LUMINARY_WAVE_HEIGHT_MM,
        .wave_period_ms = LUMINARY_WAVE_PERIOD_MS,
        .wave_kx_q10 = LUMINARY_WAVE_KX_Q10,
        .wave_ky_q10 = LUMINARY_WAVE_KY_Q10,
        .wave_component_count = LUMINARY_WAVE_COMPONENT_COUNT,
        .waves = {
            {LUMINARY_WAVE_0_HEIGHT_MM, LUMINARY_WAVE_0_PERIOD_MS},
            {LUMINARY_WAVE_1_HEIGHT_MM, LUMINARY_WAVE_1_PERIOD_MS},
            {LUMINARY_WAVE_2_HEIGHT_MM, LUMINARY_WAVE_2_PERIOD_MS},
        },
        .shells = {
            {NULL, LUMINARY_HIGH_WIND_EAST_MMPS, LUMINARY_HIGH_WIND_NORTH_MMPS,
             LUMINARY_HIGH_CLOUD_HEIGHT_M, 14U},
            {NULL, LUMINARY_MID_WIND_EAST_MMPS, LUMINARY_MID_WIND_NORTH_MMPS,
             LUMINARY_MID_CLOUD_HEIGHT_M, 9U},
            {NULL, LUMINARY_LOW_WIND_EAST_MMPS, LUMINARY_LOW_WIND_NORTH_MMPS,
             LUMINARY_LOW_CLOUD_HEIGHT_M, 4U},
        },
    };
    visible_stars = heap_caps_malloc(sizeof(visible_star_t) * MAX_VISIBLE_STARS,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(visible_stars ? ESP_OK : ESP_ERR_NO_MEM);
    runtime_state.ocean_phase = heap_caps_malloc(OCEAN_PHASE_BYTES,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(runtime_state.ocean_phase ? ESP_OK : ESP_ERR_NO_MEM);
    memcpy(runtime_state.ocean_phase, nubble_runtime_ocean_phase_bin_start, OCEAN_PHASE_BYTES);
    const uint8_t *embedded[] = {nubble_runtime_cloud_high_bin_start,
                                 nubble_runtime_cloud_mid_bin_start,
                                 nubble_runtime_cloud_low_bin_start};
    for (unsigned index = 0; index < 3; ++index) {
        runtime_state.shells[index].atlas = heap_caps_malloc(CLOUD_ATLAS_BYTES,
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_ERROR_CHECK(runtime_state.shells[index].atlas ? ESP_OK : ESP_ERR_NO_MEM);
        memcpy(runtime_state.shells[index].atlas, embedded[index], CLOUD_ATLAS_BYTES);
    }
}

static bool read_sd_cache_header(const char *path, sd_cache_header_t *header)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    const bool valid = fread(header, sizeof(*header), 1, file) == 1 &&
        header->magic == SD_CACHE_MAGIC && header->version == SD_CACHE_VERSION &&
        header->header_bytes == sizeof(*header) &&
        header->state_bytes > 0U && header->state_bytes <= RUNTIME_STATE_MAX_BYTES &&
        header->cloud_bytes == CLOUD_ATLAS_BYTES && header->ocean_bytes == OCEAN_PHASE_BYTES &&
        memchr(header->bundle_id, '\0', sizeof(header->bundle_id)) != NULL;
    fclose(file);
    return valid;
}

static esp_err_t activate_sd_cache_file(const char *path, const sd_cache_header_t *expected)
{
    esp_err_t result = ESP_FAIL;
    FILE *file = fopen(path, "rb");
    uint8_t *state = NULL;
    uint8_t *cloud[3] = {NULL, NULL, NULL};
    uint8_t *ocean = NULL;
    cJSON *scene = NULL;
    sd_cache_header_t header;
    if (!file) return ESP_ERR_NOT_FOUND;
    if (fread(&header, sizeof(header), 1, file) != 1 ||
        memcmp(&header, expected, sizeof(header)) != 0) {
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    state = malloc(header.state_bytes + 1U);
    ocean = heap_caps_malloc(OCEAN_PHASE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (unsigned index = 0; index < 3; ++index) {
        cloud[index] = heap_caps_malloc(CLOUD_ATLAS_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!state || !ocean || !cloud[0] || !cloud[1] || !cloud[2]) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    uint32_t crc = 0;
    if (fread(state, header.state_bytes, 1, file) != 1) goto cleanup;
    crc = esp_crc32_le(crc, state, header.state_bytes);
    for (unsigned index = 0; index < 3; ++index) {
        if (fread(cloud[index], CLOUD_ATLAS_BYTES, 1, file) != 1) goto cleanup;
        crc = esp_crc32_le(crc, cloud[index], CLOUD_ATLAS_BYTES);
    }
    if (fread(ocean, OCEAN_PHASE_BYTES, 1, file) != 1 ||
        fgetc(file) != EOF) goto cleanup;
    crc = esp_crc32_le(crc, ocean, OCEAN_PHASE_BYTES);
    if (crc != header.payload_crc32) {
        result = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }
    state[header.state_bytes] = '\0';
    scene = cJSON_Parse((char *)state);
    if (!scene || !valid_state_root(scene)) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    xSemaphoreTake(runtime_lock, portMAX_DELAY);
    for (unsigned index = 0; index < 3; ++index) {
        memcpy(runtime_state.shells[index].atlas, cloud[index], CLOUD_ATLAS_BYTES);
    }
    memcpy(runtime_state.ocean_phase, ocean, OCEAN_PHASE_BYTES);
    apply_state_root_locked(scene);
    strlcpy(active_bundle_id, header.bundle_id, sizeof(active_bundle_id));
    xSemaphoreGive(runtime_lock);
    sd_cache_sequence = header.sequence;
    ESP_LOGI(TAG, "SD cache bundle %s activated (sequence %llu)", active_bundle_id,
             (unsigned long long)sd_cache_sequence);
    result = ESP_OK;

cleanup:
    cJSON_Delete(scene);
    free(state);
    free(ocean);
    for (unsigned index = 0; index < 3; ++index) free(cloud[index]);
    fclose(file);
    return result;
}

static void mount_and_load_sd_cache(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16U * 1024U,
    };
    /* The ESP-C6 already occupies SDMMC slot 1. Use the TF socket's supported
     * SPI wiring so a missing/bad card cannot tear down the Wi-Fi transport. */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    const sd_pwr_ctrl_ldo_config_t power_config = {.ldo_chan_id = 4};
    sd_pwr_ctrl_handle_t power = NULL;
    esp_err_t mounted = sd_pwr_ctrl_new_on_chip_ldo(&power_config, &power);
    if (mounted != ESP_OK) {
        ESP_LOGW(TAG, "Cannot power microSD socket (%s); using embedded/network assets",
                 esp_err_to_name(mounted));
        return;
    }
    host.pwr_ctrl_handle = power;
    const spi_bus_config_t bus = {
        .mosi_io_num = GPIO_NUM_44,
        .miso_io_num = GPIO_NUM_39,
        .sclk_io_num = GPIO_NUM_43,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 16U * 1024U,
    };
    mounted = spi_bus_initialize(host.slot, &bus, SPI_DMA_CH_AUTO);
    if (mounted != ESP_OK) {
        sd_pwr_ctrl_del_on_chip_ldo(power);
        ESP_LOGW(TAG, "Cannot initialize microSD SPI bus (%s); using embedded/network assets",
                 esp_err_to_name(mounted));
        return;
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = host.slot;
    slot.gpio_cs = GPIO_NUM_42;
    sdmmc_card_t *card = NULL;
    mounted = esp_vfs_fat_sdspi_mount(SD_CACHE_MOUNT, &host, &slot, &mount_config, &card);
    if (mounted != ESP_OK) {
        spi_bus_free(host.slot);
        sd_pwr_ctrl_del_on_chip_ldo(power);
        ESP_LOGW(TAG, "No usable microSD cache (%s); using embedded/network assets",
                 esp_err_to_name(mounted));
        return;
    }
    sd_cache_available = true;
    if (mkdir(SD_CACHE_DIRECTORY, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "Cannot create %s; SD cache disabled", SD_CACHE_DIRECTORY);
        sd_cache_available = false;
        return;
    }
    sd_cache_header_t headers[2];
    const char *paths[2] = {SD_CACHE_SLOT_A, SD_CACHE_SLOT_B};
    bool valid[2] = {read_sd_cache_header(paths[0], &headers[0]),
                     read_sd_cache_header(paths[1], &headers[1])};
    int first = valid[0] && valid[1] ? (headers[1].sequence > headers[0].sequence ? 1 : 0) :
                valid[0] ? 0 : valid[1] ? 1 : -1;
    if (first >= 0) {
        sd_cache_sequence = headers[first].sequence;
        if (activate_sd_cache_file(paths[first], &headers[first]) != ESP_OK) {
            const int second = first ^ 1;
            if (valid[second]) {
                sd_cache_sequence = headers[second].sequence;
                (void)activate_sd_cache_file(paths[second], &headers[second]);
            }
        }
    }
    ESP_LOGI(TAG, "microSD runtime cache ready%s", first >= 0 ? "" : "; no saved bundle yet");
}

static esp_err_t persist_sd_cache(const char *bundle_id, const uint8_t *state, size_t state_bytes,
                                  uint8_t *const cloud[3], const uint8_t *ocean)
{
    if (!sd_cache_available) return ESP_ERR_NOT_SUPPORTED;
    const uint64_t sequence = sd_cache_sequence + 1U;
    const bool slot_a = (sequence & 1U) == 0U;
    const char *path = slot_a ? SD_CACHE_SLOT_A : SD_CACHE_SLOT_B;
    FILE *file = fopen(path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "Cannot open microSD cache slot %s for writing: errno=%d", path, errno);
        return ESP_FAIL;
    }
    sd_cache_header_t header = {
        .magic = 0U, /* Written last: an interrupted slot is never considered valid. */
        .version = SD_CACHE_VERSION,
        .header_bytes = sizeof(header),
        .sequence = sequence,
        .state_bytes = (uint32_t)state_bytes,
        .cloud_bytes = CLOUD_ATLAS_BYTES,
        .ocean_bytes = OCEAN_PHASE_BYTES,
    };
    strlcpy(header.bundle_id, bundle_id, sizeof(header.bundle_id));
    const char *failed_step = "header placeholder";
    bool ok = fwrite(&header, sizeof(header), 1, file) == 1;
    uint32_t crc = 0;
    if (ok) {
        failed_step = "runtime state";
        ok = fwrite(state, state_bytes, 1, file) == 1;
        crc = esp_crc32_le(crc, state, state_bytes);
    }
    for (unsigned index = 0; ok && index < 3; ++index) {
        failed_step = index == 0 ? "high cloud atlas" :
                      index == 1 ? "middle cloud atlas" : "low cloud atlas";
        ok = fwrite(cloud[index], CLOUD_ATLAS_BYTES, 1, file) == 1;
        crc = esp_crc32_le(crc, cloud[index], CLOUD_ATLAS_BYTES);
    }
    if (ok) {
        failed_step = "ocean phase";
        ok = fwrite(ocean, OCEAN_PHASE_BYTES, 1, file) == 1;
        crc = esp_crc32_le(crc, ocean, OCEAN_PHASE_BYTES);
    }
    if (ok) {
        failed_step = "payload sync";
        ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
    }
    if (ok) {
        header.magic = SD_CACHE_MAGIC;
        header.payload_crc32 = crc;
        failed_step = "committed header";
        ok = fseek(file, 0, SEEK_SET) == 0 && fwrite(&header, sizeof(header), 1, file) == 1 &&
             fflush(file) == 0 && fsync(fileno(file)) == 0;
    }
    const int saved_errno = errno;
    const int stream_error = ferror(file);
    fclose(file);
    if (!ok) {
        ESP_LOGE(TAG, "microSD cache write failed during %s: errno=%d ferror=%d",
                 failed_step, saved_errno, stream_error);
        return ESP_FAIL;
    }
    sd_cache_sequence = sequence;
    ESP_LOGI(TAG, "Runtime bundle %s saved to microSD slot %c", bundle_id,
             slot_a ? 'A' : 'B');
    return ESP_OK;
}

static esp_err_t receive_exact(httpd_req_t *request, uint8_t *destination, size_t expected)
{
    if ((size_t)request->content_len != expected) return ESP_ERR_INVALID_SIZE;
    size_t received = 0;
    while (received < expected) {
        const int count = httpd_req_recv(request, (char *)destination + received, expected - received);
        if (count <= 0) return ESP_FAIL;
        received += (size_t)count;
    }
    return ESP_OK;
}

static esp_err_t cloud_upload_handler(httpd_req_t *request)
{
    const unsigned shell_index = (unsigned)(uintptr_t)request->user_ctx;
    uint8_t *incoming = heap_caps_malloc(CLOUD_ATLAS_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!incoming) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    const esp_err_t result = receive_exact(request, incoming, CLOUD_ATLAS_BYTES);
    if (result != ESP_OK) {
        free(incoming);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "cloud atlas must be 49152 bytes");
    }
    xSemaphoreTake(runtime_lock, portMAX_DELAY);
    memcpy(runtime_state.shells[shell_index].atlas, incoming, CLOUD_ATLAS_BYTES);
    xSemaphoreGive(runtime_lock);
    free(incoming);
    ESP_LOGI(TAG, "Live cloud shell %u updated", shell_index);
    return httpd_resp_sendstr(request, "ok\n");
}

static esp_err_t ocean_phase_upload_handler(httpd_req_t *request)
{
    uint8_t *incoming = heap_caps_malloc(OCEAN_PHASE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!incoming) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    const esp_err_t result = receive_exact(request, incoming, OCEAN_PHASE_BYTES);
    if (result != ESP_OK) {
        free(incoming);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "ocean phase atlas must be 460800 bytes");
    }
    xSemaphoreTake(runtime_lock, portMAX_DELAY);
    memcpy(runtime_state.ocean_phase, incoming, OCEAN_PHASE_BYTES);
    xSemaphoreGive(runtime_lock);
    free(incoming);
    ESP_LOGI(TAG, "Perspective ocean phase map updated");
    return httpd_resp_sendstr(request, "ok\n");
}

static esp_err_t wave_cycle_upload_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > (int)LUMINARY_WAVE_CYCLE_MAX_BYTES) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "wave cycle is too large");
    }
    uint8_t *incoming = heap_caps_malloc((size_t)request->content_len,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!incoming) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    const esp_err_t result = receive_exact(request, incoming, (size_t)request->content_len);
    if (result != ESP_OK) {
        free(incoming);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid wave-cycle payload");
    }
    uint32_t frame_count = 0U;
    uint32_t fps_milli = 0U;
    const lumv_frame_t *frames = NULL;
    if (!parse_lumv_cycle(incoming, (size_t)request->content_len, &frame_count, &fps_milli, &frames)) {
        free(incoming);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                  "invalid LUMV wave-cycle payload");
    }
    xSemaphoreTake(runtime_lock, portMAX_DELAY);
    release_wave_cycle_locked();
    runtime_wave_cycle.payload = incoming;
    runtime_wave_cycle.payload_bytes = (size_t)request->content_len;
    runtime_wave_cycle.frames = frames;
    runtime_wave_cycle.frame_count = frame_count;
    runtime_wave_cycle.fps_milli = fps_milli;
    xSemaphoreGive(runtime_lock);
    ESP_LOGI(TAG, "Wave cycle updated: %u frames at %u.%03u fps",
             runtime_wave_cycle.frame_count, runtime_wave_cycle.fps_milli / 1000U,
             runtime_wave_cycle.fps_milli % 1000U);
    return httpd_resp_sendstr(request, "ok\n");
}

static bool valid_state_root(cJSON *root)
{
    cJSON *camera = cJSON_GetObjectItem(root, "camera");
    cJSON *horizon = camera ? cJSON_GetObjectItem(camera, "horizon_y_px") : NULL;
    return cJSON_IsNumber(horizon) && horizon->valueint == LUMINARY_RUNTIME_HORIZON;
}

/* Apply a prevalidated state while runtime_lock is held by the caller. */
static void apply_state_root_locked(cJSON *root)
{
    cJSON *sky = cJSON_GetObjectItem(root, "sky");
    cJSON *fraction = sky ? cJSON_GetObjectItem(sky, "observed_cloud_fraction") : NULL;
    if (cJSON_IsNumber(fraction)) {
        runtime_state.cloud_cover_permille = (uint16_t)fmin(1000.0, fmax(0.0, fraction->valuedouble * 1000.0));
    }
    cJSON *palette = sky ? cJSON_GetObjectItem(sky, "palette_rgb") : NULL;
    if (cJSON_IsArray(palette) && cJSON_GetArraySize(palette) == 3) {
        runtime_state.sky_r = (uint8_t)cJSON_GetArrayItem(palette, 0)->valueint;
        runtime_state.sky_g = (uint8_t)cJSON_GetArrayItem(palette, 1)->valueint;
        runtime_state.sky_b = (uint8_t)cJSON_GetArrayItem(palette, 2)->valueint;
    }
    cJSON *shells = sky ? cJSON_GetObjectItem(sky, "shells") : NULL;
    cJSON *shell = NULL;
    cJSON_ArrayForEach(shell, shells) {
        cJSON *name = cJSON_GetObjectItem(shell, "name");
        const unsigned index = cJSON_IsString(name) && strcmp(name->valuestring, "high") == 0 ? 0U :
                               cJSON_IsString(name) && strcmp(name->valuestring, "mid") == 0 ? 1U : 2U;
        cJSON *height = cJSON_GetObjectItem(shell, "projection_height_m");
        cJSON *advection = cJSON_GetObjectItem(shell, "advection");
        cJSON *east = advection ? cJSON_GetObjectItem(advection, "east_mps") : NULL;
        cJSON *north = advection ? cJSON_GetObjectItem(advection, "north_mps") : NULL;
        if (cJSON_IsNumber(height)) runtime_state.shells[index].height_m = (uint32_t)height->valueint;
        if (cJSON_IsNumber(east)) runtime_state.shells[index].wind_east_mmps = (int32_t)lrint(east->valuedouble * 1000.0);
        if (cJSON_IsNumber(north)) runtime_state.shells[index].wind_north_mmps = (int32_t)lrint(north->valuedouble * 1000.0);
    }
    cJSON *ocean = cJSON_GetObjectItem(root, "ocean");
    cJSON *height = ocean ? cJSON_GetObjectItem(ocean, "significant_wave_height_m") : NULL;
    cJSON *period = ocean ? cJSON_GetObjectItem(ocean, "dominant_period_s") : NULL;
    cJSON *direction = ocean ? cJSON_GetObjectItem(ocean, "wave_from_deg") : NULL;
    if (cJSON_IsNumber(height)) runtime_state.wave_height_mm = (uint32_t)lrint(height->valuedouble * 1000.0);
    if (cJSON_IsNumber(period)) runtime_state.wave_period_ms = (uint32_t)lrint(period->valuedouble * 1000.0);
    if (cJSON_IsNumber(direction)) {
        const double relative = (direction->valuedouble - 90.0) * 3.141592653589793 / 180.0;
        runtime_state.wave_kx_q10 = (int32_t)lrint(sin(relative) * 1024.0);
        runtime_state.wave_ky_q10 = (int32_t)lrint(cos(relative) * 1024.0);
    }
    cJSON *components = ocean ? cJSON_GetObjectItem(ocean, "components") : NULL;
    if (cJSON_IsArray(components)) {
        const unsigned count = (unsigned)fmin(3.0, cJSON_GetArraySize(components));
        runtime_state.wave_component_count = (uint8_t)count;
        for (unsigned index = 0; index < 3U; ++index) {
            runtime_state.waves[index] = (wave_component_t){0U, 1000U};
            if (index >= count) continue;
            cJSON *component = cJSON_GetArrayItem(components, index);
            cJSON *component_height = cJSON_GetObjectItem(component, "height_m");
            cJSON *component_period = cJSON_GetObjectItem(component, "period_s");
            if (cJSON_IsNumber(component_height)) {
                runtime_state.waves[index].height_mm =
                    (uint32_t)fmax(0.0, lrint(component_height->valuedouble * 1000.0));
            }
            if (cJSON_IsNumber(component_period)) {
                runtime_state.waves[index].period_ms =
                    (uint32_t)fmax(500.0, lrint(component_period->valuedouble * 1000.0));
            }
        }
    }
    cJSON *sun = cJSON_GetObjectItem(root, "sun");
    cJSON *sun_name = sun ? cJSON_GetObjectItem(sun, "state") : NULL;
    cJSON *sun_altitude = sun ? cJSON_GetObjectItem(sun, "altitude_deg") : NULL;
    cJSON *sun_azimuth = sun ? cJSON_GetObjectItem(sun, "azimuth_deg") : NULL;
    if (cJSON_IsString(sun_name)) {
        runtime_state.sun_mode = strcmp(sun_name->valuestring, "civil_twilight") == 0 ? 1U :
                                 strcmp(sun_name->valuestring, "nautical_twilight") == 0 ? 2U :
                                 strcmp(sun_name->valuestring, "night") == 0 ? 3U : 0U;
    }
    if (cJSON_IsNumber(sun_altitude)) {
        runtime_state.sun_altitude_deci_deg = (int16_t)lrint(sun_altitude->valuedouble * 10.0);
    }
    if (cJSON_IsNumber(sun_azimuth)) {
        double relative = sun_azimuth->valuedouble - 90.0;
        while (relative > 180.0) relative -= 360.0;
        while (relative < -180.0) relative += 360.0;
        runtime_state.sun_relative_azimuth_deci_deg = (int16_t)lrint(relative * 10.0);
    }
    cJSON *moon = cJSON_GetObjectItem(root, "moon");
    cJSON *visible = moon ? cJSON_GetObjectItem(moon, "visible") : NULL;
    cJSON *azimuth = moon ? cJSON_GetObjectItem(moon, "azimuth_deg") : NULL;
    cJSON *altitude = moon ? cJSON_GetObjectItem(moon, "altitude_deg") : NULL;
    cJSON *illumination = moon ? cJSON_GetObjectItem(moon, "illumination") : NULL;
    runtime_state.moon_visible = cJSON_IsTrue(visible);
    if (runtime_state.moon_visible && cJSON_IsNumber(azimuth) && cJSON_IsNumber(altitude)) {
        double delta = azimuth->valuedouble - 90.0;
        while (delta > 180.0) delta -= 360.0;
        while (delta < -180.0) delta += 360.0;
        runtime_state.moon_x = (int16_t)lrint(512.0 + 358.53 * tan(delta * 3.141592653589793 / 180.0));
        runtime_state.moon_y = (int16_t)lrint(291.0 - 782.79 * tan(altitude->valuedouble * 3.141592653589793 / 180.0));
        runtime_state.moon_visible = runtime_state.moon_x >= 0 && runtime_state.moon_x < LUMINARY_WIDTH &&
                                     runtime_state.moon_y >= 0 && runtime_state.moon_y < LUMINARY_RUNTIME_HORIZON;
    }
    if (cJSON_IsNumber(illumination)) {
        runtime_state.moon_illumination_permille = (uint16_t)fmin(1000.0,
            fmax(0.0, illumination->valuedouble * 1000.0));
    }
}

static esp_err_t state_upload_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > RUNTIME_STATE_MAX_BYTES) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid state size");
    }
    char *body = malloc((size_t)request->content_len + 1U);
    if (!body) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    const esp_err_t received = receive_exact(request, (uint8_t *)body, (size_t)request->content_len);
    body[request->content_len] = '\0';
    cJSON *root = received == ESP_OK ? cJSON_Parse(body) : NULL;
    free(body);
    if (!root) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid JSON");
    if (!valid_state_root(root)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "horizon must be 291");
    }
    xSemaphoreTake(runtime_lock, portMAX_DELAY);
    apply_state_root_locked(root);
    xSemaphoreGive(runtime_lock);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Live state updated: cloud=%u/1000", runtime_state.cloud_cover_permille);
    return httpd_resp_sendstr(request, "ok\n");
}

static esp_err_t screenshot_handler(httpd_req_t *request)
{
    uint8_t *snapshot = heap_caps_malloc(LUMINARY_FRAME_BYTES,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    xSemaphoreTake(runtime_lock, portMAX_DELAY);
    if (!current_framebuffer) {
        xSemaphoreGive(runtime_lock);
        free(snapshot);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "frame not ready");
    }
    memcpy(snapshot, current_framebuffer, LUMINARY_FRAME_BYTES);
    xSemaphoreGive(runtime_lock);

    // The scanout buffer is BGR888. PPM is deliberately used here because it
    // preserves the exact displayed pixels without adding a JPEG encoder to
    // production firmware; the host can losslessly convert it to PNG.
    for (size_t pixel = 0; pixel < (size_t)LUMINARY_WIDTH * LUMINARY_HEIGHT; ++pixel) {
        const uint8_t blue = snapshot[pixel * 3U];
        snapshot[pixel * 3U] = snapshot[pixel * 3U + 2U];
        snapshot[pixel * 3U + 2U] = blue;
    }
    httpd_resp_set_type(request, "image/x-portable-pixmap");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char header[32];
    const int header_bytes = snprintf(header, sizeof(header), "P6\n%u %u\n255\n",
                                      LUMINARY_WIDTH, LUMINARY_HEIGHT);
    esp_err_t result = httpd_resp_send_chunk(request, header, header_bytes);
    for (size_t offset = 0; result == ESP_OK && offset < LUMINARY_FRAME_BYTES; offset += 65536U) {
        const size_t remaining = LUMINARY_FRAME_BYTES - offset;
        const size_t chunk = remaining < 65536U ? remaining : 65536U;
        result = httpd_resp_send_chunk(request, (const char *)snapshot + offset, chunk);
    }
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, NULL, 0);
    free(snapshot);
    return result;
}

static void start_runtime_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t state_uri = {.uri = "/runtime/state", .method = HTTP_POST,
                                   .handler = state_upload_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &state_uri));
    const httpd_uri_t screenshot_uri = {.uri = "/runtime/screenshot.ppm", .method = HTTP_GET,
                                        .handler = screenshot_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &screenshot_uri));
    const char *uris[] = {"/runtime/cloud/high", "/runtime/cloud/mid", "/runtime/cloud/low"};
    for (unsigned index = 0; index < 3; ++index) {
        const httpd_uri_t uri = {.uri = uris[index], .method = HTTP_POST,
                                 .handler = cloud_upload_handler, .user_ctx = (void *)(uintptr_t)index};
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
    }
    const httpd_uri_t ocean_uri = {.uri = "/runtime/ocean-phase", .method = HTTP_POST,
                                    .handler = ocean_phase_upload_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ocean_uri));
    const httpd_uri_t wave_cycle_uri = {.uri = "/runtime/wave-cycle", .method = HTTP_POST,
                                       .handler = wave_cycle_upload_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wave_cycle_uri));
    ESP_LOGI(TAG, "Live runtime update API listening on port 80");
}

typedef struct {
    char path[RUNTIME_PATH_MAX_BYTES];
    size_t bytes;
    uint32_t crc32;
} remote_asset_t;

static esp_err_t http_get_buffer(const char *url, uint8_t *buffer, size_t capacity,
                                 size_t *received)
{
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 4,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_err_t result = esp_http_client_open(client, 0);
    if (result != ESP_OK) goto done;
    const int64_t content_length = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        result = ESP_ERR_HTTP_FETCH_HEADER;
        goto done;
    }
    if (content_length > 0 && (uint64_t)content_length > capacity) {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    const int count = esp_http_client_read_response(client, (char *)buffer, (int)capacity);
    if (count < 0) {
        result = ESP_FAIL;
        goto done;
    }
    *received = (size_t)count;
    if (content_length >= 0 && *received != (size_t)content_length) {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    result = ESP_OK;
done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

static bool parse_remote_asset(cJSON *assets, const char *name, size_t required_bytes,
                               remote_asset_t *asset)
{
    cJSON *entry = cJSON_GetObjectItem(assets, name);
    cJSON *path = entry ? cJSON_GetObjectItem(entry, "path") : NULL;
    cJSON *bytes = entry ? cJSON_GetObjectItem(entry, "bytes") : NULL;
    cJSON *crc = entry ? cJSON_GetObjectItem(entry, "crc32") : NULL;
    if (!cJSON_IsString(path) || !cJSON_IsNumber(bytes) || !cJSON_IsString(crc) ||
        strlen(path->valuestring) >= sizeof(asset->path) || strlen(crc->valuestring) != 8U) {
        return false;
    }
    asset->bytes = (size_t)bytes->valuedouble;
    if ((required_bytes && asset->bytes != required_bytes) ||
        (!required_bytes && (asset->bytes == 0U || asset->bytes > RUNTIME_STATE_MAX_BYTES))) {
        return false;
    }
    char *end = NULL;
    asset->crc32 = (uint32_t)strtoul(crc->valuestring, &end, 16);
    if (!end || *end != '\0') return false;
    strlcpy(asset->path, path->valuestring, sizeof(asset->path));
    return true;
}

static bool parse_remote_cycle_asset(cJSON *assets, const char *name, size_t max_bytes,
                                    remote_asset_t *asset)
{
    cJSON *entry = cJSON_GetObjectItem(assets, name);
    cJSON *path = entry ? cJSON_GetObjectItem(entry, "path") : NULL;
    cJSON *bytes = entry ? cJSON_GetObjectItem(entry, "bytes") : NULL;
    cJSON *crc = entry ? cJSON_GetObjectItem(entry, "crc32") : NULL;
    if (!cJSON_IsString(path) || !cJSON_IsNumber(bytes) || !cJSON_IsString(crc) ||
        strlen(path->valuestring) >= sizeof(asset->path) || strlen(crc->valuestring) != 8U) {
        return false;
    }
    asset->bytes = (size_t)bytes->valuedouble;
    if (asset->bytes == 0U || asset->bytes > max_bytes) return false;
    char *end = NULL;
    asset->crc32 = (uint32_t)strtoul(crc->valuestring, &end, 16);
    if (!end || *end != '\0') return false;
    strlcpy(asset->path, path->valuestring, sizeof(asset->path));
    return true;
}

static bool parse_lumv_cycle(const uint8_t *payload, size_t payload_bytes,
                             uint32_t *frame_count, uint32_t *fps_milli,
                             const lumv_frame_t **frames_out)
{
    if (!payload || payload_bytes < sizeof(lumv_header_t)) return false;
    const lumv_header_t *header = (const lumv_header_t *)payload;
    if (header->magic != LUMV_MAGIC || header->version != LUMV_VERSION ||
        header->header_bytes != sizeof(lumv_header_t) ||
        header->width != LUMINARY_WIDTH || header->height != LUMINARY_HEIGHT ||
        header->frame_count == 0U || header->fps_milli == 0U ||
        header->frame_count > (UINT32_MAX / sizeof(lumv_frame_t))) {
        return false;
    }
    const size_t index_offset = header->index_offset;
    const size_t data_offset = header->data_offset;
    const size_t required_index = sizeof(lumv_header_t) +
        (size_t)header->frame_count * sizeof(lumv_frame_t);
    if (index_offset != sizeof(lumv_header_t) || data_offset != required_index ||
        data_offset > payload_bytes) {
        return false;
    }
    const lumv_frame_t *frames = (const lumv_frame_t *)(payload + index_offset);
    const lumv_frame_t *end = frames + header->frame_count;
    for (const lumv_frame_t *frame = frames; frame < end; ++frame) {
        const size_t offset = frame->offset;
        const size_t length = frame->length;
        if (offset < data_offset || length == 0U ||
            length > payload_bytes || offset > payload_bytes - length) {
            return false;
        }
    }
    *frame_count = header->frame_count;
    *fps_milli = header->fps_milli;
    *frames_out = frames;
    return true;
}

static void release_wave_cycle_locked(void)
{
    free(runtime_wave_cycle.payload);
    runtime_wave_cycle = (wave_cycle_t){0};
}

static esp_err_t download_remote_asset(const remote_asset_t *asset, uint8_t *destination)
{
    char url[RUNTIME_URL_MAX_BYTES];
    if (snprintf(url, sizeof(url), "%s/%s", CONFIG_LUMINARY_RUNTIME_BASE_URL,
                 asset->path) >= sizeof(url)) return ESP_ERR_INVALID_SIZE;
    size_t received = 0;
    esp_err_t result = http_get_buffer(url, destination, asset->bytes, &received);
    if (result != ESP_OK || received != asset->bytes) return ESP_ERR_INVALID_SIZE;
    return esp_crc32_le(0, destination, (uint32_t)received) == asset->crc32 ?
           ESP_OK : ESP_ERR_INVALID_CRC;
}

static esp_err_t pull_runtime_bundle(void)
{
    esp_err_t result = ESP_FAIL;
    uint8_t *manifest_bytes = malloc(RUNTIME_MANIFEST_MAX_BYTES + 1U);
    cJSON *manifest = NULL;
    cJSON *scene = NULL;
    uint8_t *cloud[3] = {NULL, NULL, NULL};
    uint8_t *ocean = NULL;
    uint8_t *state = NULL;
    uint8_t *wave_cycle = NULL;
    const lumv_frame_t *cycle_frames = NULL;
    uint32_t cycle_frame_count = 0U;
    uint32_t cycle_fps_milli = 0U;
    remote_asset_t cycle_descriptor = {0};
    if (!manifest_bytes) return ESP_ERR_NO_MEM;

    char manifest_url[RUNTIME_URL_MAX_BYTES];
    snprintf(manifest_url, sizeof(manifest_url), "%s/manifest.json?poll=%llu",
             CONFIG_LUMINARY_RUNTIME_BASE_URL,
             (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    size_t manifest_size = 0;
    result = http_get_buffer(manifest_url, manifest_bytes, RUNTIME_MANIFEST_MAX_BYTES,
                             &manifest_size);
    if (result != ESP_OK) goto cleanup;
    manifest_bytes[manifest_size] = '\0';
    manifest = cJSON_Parse((char *)manifest_bytes);
    if (!manifest) { result = ESP_ERR_INVALID_RESPONSE; goto cleanup; }
    cJSON *schema = cJSON_GetObjectItem(manifest, "schema");
    cJSON *bundle = cJSON_GetObjectItem(manifest, "bundle_id");
    cJSON *camera = cJSON_GetObjectItem(manifest, "camera");
    cJSON *horizon = camera ? cJSON_GetObjectItem(camera, "horizon_y_px") : NULL;
    cJSON *assets = cJSON_GetObjectItem(manifest, "assets");
    if (!cJSON_IsString(schema) || strcmp(schema->valuestring, "luminary-runtime-bundle/v1") != 0 ||
        !cJSON_IsString(bundle) || strlen(bundle->valuestring) >= sizeof(active_bundle_id) ||
        !cJSON_IsNumber(horizon) || horizon->valueint != LUMINARY_RUNTIME_HORIZON ||
        !cJSON_IsObject(assets)) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    if (strcmp(bundle->valuestring, active_bundle_id) == 0) {
        ESP_LOGI(TAG, "Runtime bundle %s unchanged", active_bundle_id);
        result = ESP_OK;
        goto cleanup;
    }

    remote_asset_t descriptor[5];
    const bool has_cycle = cJSON_GetObjectItem(assets, "wave_cycle") != NULL;
    if (!parse_remote_asset(assets, "cloud_high", CLOUD_ATLAS_BYTES, &descriptor[0]) ||
        !parse_remote_asset(assets, "cloud_mid", CLOUD_ATLAS_BYTES, &descriptor[1]) ||
        !parse_remote_asset(assets, "cloud_low", CLOUD_ATLAS_BYTES, &descriptor[2]) ||
        !parse_remote_asset(assets, "ocean_phase", OCEAN_PHASE_BYTES, &descriptor[3]) ||
        !parse_remote_asset(assets, "state", 0, &descriptor[4]) ||
        (has_cycle && !parse_remote_cycle_asset(assets, "wave_cycle",
                                               LUMINARY_WAVE_CYCLE_MAX_BYTES, &cycle_descriptor))) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    for (unsigned index = 0; index < 3; ++index) {
        cloud[index] = heap_caps_malloc(CLOUD_ATLAS_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!cloud[index]) { result = ESP_ERR_NO_MEM; goto cleanup; }
        result = download_remote_asset(&descriptor[index], cloud[index]);
        if (result != ESP_OK) goto cleanup;
    }
    ocean = heap_caps_malloc(OCEAN_PHASE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    state = malloc(descriptor[4].bytes + 1U);
    if (!ocean || !state) { result = ESP_ERR_NO_MEM; goto cleanup; }
    result = download_remote_asset(&descriptor[3], ocean);
    if (result != ESP_OK) goto cleanup;
    result = download_remote_asset(&descriptor[4], state);
    if (result != ESP_OK) goto cleanup;
    state[descriptor[4].bytes] = '\0';
    scene = cJSON_Parse((char *)state);
    if (!scene || !valid_state_root(scene)) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    if (has_cycle) {
        wave_cycle = heap_caps_malloc(cycle_descriptor.bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!wave_cycle) {
            result = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        result = download_remote_asset(&cycle_descriptor, wave_cycle);
        if (result != ESP_OK) goto cleanup;
        if (!parse_lumv_cycle(wave_cycle, cycle_descriptor.bytes,
                              &cycle_frame_count, &cycle_fps_milli, &cycle_frames)) {
            result = ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }
    }

    const esp_err_t cached = persist_sd_cache(bundle->valuestring, state,
                                               descriptor[4].bytes, cloud, ocean);
    if (cached != ESP_OK && cached != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Validated bundle could not be cached on microSD: %s",
                 esp_err_to_name(cached));
    }

    /* One lock protects the complete bundle swap; rendering sees old or new. */
    xSemaphoreTake(runtime_lock, portMAX_DELAY);
    for (unsigned index = 0; index < 3; ++index) {
        memcpy(runtime_state.shells[index].atlas, cloud[index], CLOUD_ATLAS_BYTES);
    }
    memcpy(runtime_state.ocean_phase, ocean, OCEAN_PHASE_BYTES);
    if (has_cycle) {
        release_wave_cycle_locked();
        runtime_wave_cycle.payload = wave_cycle;
        runtime_wave_cycle.payload_bytes = cycle_descriptor.bytes;
        runtime_wave_cycle.frames = cycle_frames;
        runtime_wave_cycle.frame_count = cycle_frame_count;
        runtime_wave_cycle.fps_milli = cycle_fps_milli;
        wave_cycle = NULL;
    } else {
        release_wave_cycle_locked();
    }
    apply_state_root_locked(scene);
    strlcpy(active_bundle_id, bundle->valuestring, sizeof(active_bundle_id));
    xSemaphoreGive(runtime_lock);
    ESP_LOGI(TAG, "Autonomous runtime bundle %s activated; cloud=%u/1000; sun alt=%.1f deg rel-az=%.1f deg",
             active_bundle_id, runtime_state.cloud_cover_permille,
             runtime_state.sun_altitude_deci_deg / 10.0,
             runtime_state.sun_relative_azimuth_deci_deg / 10.0);
    result = ESP_OK;

cleanup:
    cJSON_Delete(scene);
    cJSON_Delete(manifest);
    free(manifest_bytes);
    free(state);
    free(ocean);
    for (unsigned index = 0; index < 3; ++index) free(cloud[index]);
    free(wave_cycle);
    return result;
}

static void runtime_pull_task(void *argument)
{
    (void)argument;
    while (true) {
        xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        const esp_err_t result = pull_runtime_bundle();
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Autonomous runtime refresh failed: %s; retaining current scene",
                     esp_err_to_name(result));
        }
        const unsigned delay_seconds = result == ESP_OK ?
            CONFIG_LUMINARY_RUNTIME_POLL_SECONDS : 60U;
        vTaskDelay(pdMS_TO_TICKS(delay_seconds * 1000U));
    }
}

static inline bool runtime_water_pixel(size_t pixel)
{
    return (water_mask_ram[pixel >> 3] >> (pixel & 7U)) & 1U;
}

static inline int wrap_cloud_x(int value)
{
    return value & (LUMINARY_CLOUD_TEXTURE_WIDTH - 1U);
}

static inline int mirror_cloud_y(int value)
{
    const int period = 2 * ((int)LUMINARY_CLOUD_TEXTURE_HEIGHT - 1);
    value %= period;
    if (value < 0) value += period;
    return value < (int)LUMINARY_CLOUD_TEXTURE_HEIGHT ? value : period - value;
}

static void composite_cloud_shell(uint8_t *bgr, const cloud_shell_t *shell,
                                  unsigned x, unsigned y, int shift_x, int shift_y,
                                  unsigned cloud_cover_permille)
{
    if (cloud_cover_permille == 0U || y >= LUMINARY_RUNTIME_HORIZON) return;

    // The locked 110 x 68 degree view is parameterized as an equirectangular
    // patch on each cloud sphere. Wind rotates the shell continuously.  At
    // the east-facing Nubble camera, north is screen-left and east is into
    // the view, so the two measured wind components drive orthogonal axes.
    const int atlas_x = wrap_cloud_x((int)cloud_x_lut[x] + shift_x);
    const int atlas_y = mirror_cloud_y((int)cloud_y_lut[y] + shift_y);
    const size_t index = ((size_t)atlas_y * LUMINARY_CLOUD_TEXTURE_WIDTH + atlas_x) * 2U;
    const unsigned luminance = shell->atlas[index];
    unsigned alpha = shell->atlas[index + 1U];
    alpha = alpha * cloud_cover_permille / 1000U;
    // Feather the bottom 22 rows so no shell can create a false horizon line.
    const unsigned clearance = LUMINARY_RUNTIME_HORIZON - y;
    if (clearance < 22U) alpha = alpha * clearance / 22U;
    if (alpha == 0U) return;

    const unsigned cloud_b = luminance;
    const unsigned cloud_g = luminance > shell->blue_bias / 2U ? luminance - shell->blue_bias / 2U : 0U;
    const unsigned cloud_r = luminance > shell->blue_bias ? luminance - shell->blue_bias : 0U;
    bgr[0] = (uint8_t)((bgr[0] * (255U - alpha) + cloud_b * alpha) / 255U);
    bgr[1] = (uint8_t)((bgr[1] * (255U - alpha) + cloud_g * alpha) / 255U);
    bgr[2] = (uint8_t)((bgr[2] * (255U - alpha) + cloud_r * alpha) / 255U);
}

static inline uint8_t clamp_channel(int value)
{
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

static unsigned sunset_warmth_255(void)
{
    // Begin golden hour at +10 degrees, peak just after the apparent disc
    // reaches the horizon, then fade naturally through civil twilight.
    const int altitude = runtime_state.sun_altitude_deci_deg;
    if (altitude >= 100) return 0U;
    if (altitude >= 0) return (unsigned)((100 - altitude) * 220 / 100);
    if (altitude >= -10) return (unsigned)(220 + (-altitude) * 2);
    if (altitude >= -60) return (unsigned)(240 - ((-altitude - 10) * 160 / 50));
    if (altitude >= -120) return (unsigned)((120 + altitude) * 80 / 60);
    return 0U;
}

static unsigned sunset_horizontal_255(unsigned x)
{
    // Sunset is behind the east-facing Nubble view. Preserve that geometry:
    // no false disc, only a broad side-weighted atmospheric illumination.
    const int relative = runtime_state.sun_relative_azimuth_deci_deg;
    if (relative < 0) return 255U - x * 96U / (LUMINARY_WIDTH - 1U);
    return 159U + x * 96U / (LUMINARY_WIDTH - 1U);
}

static void grade_sky_pixel(uint8_t *bgr, unsigned x, unsigned y)
{
    // Preserve the authored luminance gradient while matching the live-camera
    // chroma. Solar state then grades the same fixed geometry continuously.
    bgr[0] = clamp_channel((int)bgr[0] + (int)runtime_state.sky_b - (int)LUMINARY_SKY_B);
    bgr[1] = clamp_channel((int)bgr[1] + (int)runtime_state.sky_g - (int)LUMINARY_SKY_G);
    bgr[2] = clamp_channel((int)bgr[2] + (int)runtime_state.sky_r - (int)LUMINARY_SKY_R);
    const unsigned warmth = sunset_warmth_255();
    if (warmth > 0U) {
        const unsigned distance = LUMINARY_RUNTIME_HORIZON - y;
        const unsigned vertical = distance < 190U ? (190U - distance) * 255U / 190U : 0U;
        const unsigned glow = warmth * vertical / 255U * sunset_horizontal_255(x) / 255U;
        bgr[0] = (uint8_t)((bgr[0] * (255U - glow) + 78U * glow) / 255U);
        bgr[1] = (uint8_t)((bgr[1] * (255U - glow) + 126U * glow) / 255U);
        bgr[2] = (uint8_t)((bgr[2] * (255U - glow) + 248U * glow) / 255U);
    }
    if (runtime_state.sun_mode == 1U && runtime_state.sun_altitude_deci_deg < 0) {
        const unsigned depth = (unsigned)fmin(60.0, -(double)runtime_state.sun_altitude_deci_deg);
        const unsigned light = 255U - depth * 85U / 60U;
        bgr[0] = (uint8_t)(bgr[0] * light / 255U);
        bgr[1] = (uint8_t)(bgr[1] * light / 255U);
        bgr[2] = (uint8_t)(bgr[2] * light / 255U);
    } else if (runtime_state.sun_mode == 2U) {
        bgr[0] = (uint8_t)(bgr[0] * 34U / 100U);
        bgr[1] = (uint8_t)(bgr[1] * 24U / 100U);
        bgr[2] = (uint8_t)(bgr[2] * 15U / 100U);
    } else if (runtime_state.sun_mode == 3U) {
        bgr[0] = (uint8_t)(bgr[0] * 22U / 100U);
        bgr[1] = (uint8_t)(bgr[1] * 12U / 100U);
        bgr[2] = (uint8_t)(bgr[2] * 7U / 100U);
    }

    // The moon is projected from measured York altitude/azimuth. It is drawn
    // before cloud shells, allowing real clouds to occlude it.
    if (runtime_state.moon_visible) {
        const int dx = (int)x - runtime_state.moon_x;
        const int dy = (int)y - runtime_state.moon_y;
        if (dx * dx + dy * dy <= 144) {
            const int terminator = 12 - (int)(runtime_state.moon_illumination_permille * 24U / 1000U);
            if (dx >= terminator) {
                const unsigned alpha = 185U;
                bgr[0] = (uint8_t)((bgr[0] * (255U - alpha) + 226U * alpha) / 255U);
                bgr[1] = (uint8_t)((bgr[1] * (255U - alpha) + 226U * alpha) / 255U);
                bgr[2] = (uint8_t)((bgr[2] * (255U - alpha) + 214U * alpha) / 255U);
            }
        }
    }
}

static void grade_water_pixel(uint8_t *bgr, unsigned y)
{
    const unsigned warmth = sunset_warmth_255();
    if (warmth > 0U) {
        const unsigned distance = y - LUMINARY_RUNTIME_HORIZON;
        const unsigned vertical = distance < 170U ? (170U - distance) * 255U / 170U : 0U;
        const unsigned glow = warmth * vertical / 255U * 128U / 255U;
        bgr[0] = (uint8_t)((bgr[0] * (255U - glow) + 68U * glow) / 255U);
        bgr[1] = (uint8_t)((bgr[1] * (255U - glow) + 102U * glow) / 255U);
        bgr[2] = (uint8_t)((bgr[2] * (255U - glow) + 220U * glow) / 255U);
    }
    if (runtime_state.sun_mode == 2U) {
        bgr[0] = (uint8_t)(bgr[0] * 52U / 100U);
        bgr[1] = (uint8_t)(bgr[1] * 42U / 100U);
        bgr[2] = (uint8_t)(bgr[2] * 34U / 100U);
    } else if (runtime_state.sun_mode == 3U) {
        bgr[0] = (uint8_t)(bgr[0] * 34U / 100U);
        bgr[1] = (uint8_t)(bgr[1] * 24U / 100U);
        bgr[2] = (uint8_t)(bgr[2] * 18U / 100U);
    }
}

static unsigned cloud_transmission_at(unsigned x, unsigned y, const int shift_x[3],
                                      const int shift_y[3], unsigned cloud_cover_permille)
{
    unsigned transmission = 255U;
    for (unsigned shell_index = 0; shell_index < 3U; ++shell_index) {
        const cloud_shell_t *shell = &runtime_state.shells[shell_index];
        const int atlas_x = wrap_cloud_x((int)cloud_x_lut[x] + shift_x[shell_index]);
        const int atlas_y = mirror_cloud_y((int)cloud_y_lut[y] + shift_y[shell_index]);
        const size_t index = ((size_t)atlas_y * LUMINARY_CLOUD_TEXTURE_WIDTH + atlas_x) * 2U;
        unsigned alpha = shell->atlas[index + 1U] * cloud_cover_permille / 1000U;
        const unsigned clearance = LUMINARY_RUNTIME_HORIZON - y;
        if (clearance < 22U) alpha = alpha * clearance / 22U;
        transmission = transmission * (255U - alpha) / 255U;
    }
    return transmission;
}

static void blend_star_pixel(uint8_t *destination, int x, int y, unsigned intensity)
{
    if (x < 0 || x >= (int)LUMINARY_WIDTH || y < 0 || y >= (int)LUMINARY_RUNTIME_HORIZON) return;
    uint8_t *bgr = destination + ((size_t)y * LUMINARY_WIDTH + (unsigned)x) * LUMINARY_BPP;
    const unsigned target[3] = {255U, 244U, 235U};
    for (unsigned channel = 0; channel < 3U; ++channel) {
        bgr[channel] = (uint8_t)((bgr[channel] * (255U - intensity) +
                                  target[channel] * intensity) / 255U);
    }
}

static void overlay_real_stars(uint8_t *destination, const int shift_x[3], const int shift_y[3],
                               unsigned cloud_cover_permille)
{
    for (unsigned index = 0; index < visible_star_count; ++index) {
        const visible_star_t *star = &visible_stars[index];
        const unsigned transmission = cloud_transmission_at(
            (unsigned)star->x, (unsigned)star->y, shift_x, shift_y, cloud_cover_permille);
        const unsigned center = star->intensity * transmission / 255U;
        if (center < 5U) continue;
        blend_star_pixel(destination, star->x, star->y, center);
        if (star->radius >= 1U) {
            const unsigned halo = center / 3U;
            blend_star_pixel(destination, star->x - 1, star->y, halo);
            blend_star_pixel(destination, star->x + 1, star->y, halo);
            blend_star_pixel(destination, star->x, star->y - 1, halo);
            blend_star_pixel(destination, star->x, star->y + 1, halo);
        }
        if (star->radius >= 2U) {
            const unsigned halo = center / 6U;
            blend_star_pixel(destination, star->x - 2, star->y, halo);
            blend_star_pixel(destination, star->x + 2, star->y, halo);
            blend_star_pixel(destination, star->x, star->y - 2, halo);
            blend_star_pixel(destination, star->x, star->y + 2, halo);
        }
    }
}

#if CONFIG_LUMINARY_OCEAN_SIM
extern const uint8_t nubble_runtime_ocean_depth_bin_start[] asm("_binary_nubble_runtime_ocean_depth_bin_start");
extern const uint8_t nubble_runtime_ocean_map_bin_start[] asm("_binary_nubble_runtime_ocean_map_bin_start");

/* Shallow-water solver state. Every grid buffer lives in PSRAM: h, vel, depth,
 * normal and foam total 192 KB, and the internal heap has nowhere near that
 * spare -- adding 1.7 KB of IRAM was already enough to fail an allocation
 * during ESP-Hosted startup. The solver's own working set (h + vel + depth) is
 * sized to stay inside the 128 KB L2 cache as it streams through. */
/* The solver struct is ~5.7 KB of LUTs and row scratch. That is small next to
 * the grids but not next to the internal heap: this firmware boot-loops in
 * ESP-Hosted startup once DIRAM passes roughly 133 KB, so it goes to PSRAM
 * too. Its lookup tables are 256 entries each and stay resident in L2. */
static ocean_sim_t *ocean_sim;
static SemaphoreHandle_t ocean_lock;
static bool ocean_ready;

/* Screen-to-solver map: one uint16 solver cell index per half-resolution
 * panel cell, rows 290..599, built offline by scripts/build-ocean-screen-map.py
 * from the fitted sea-plane projection. 0xFFFF marks cells the solver domain
 * does not cover (the far field near the horizon); those keep the analytic
 * sine-phase path. Copied to PSRAM at start: the render loop reads it once
 * per cell and flash-mapped rodata would contend with instruction fetch. */
#define OCEAN_MAP_ROW0 145u
#define OCEAN_MAP_ROWS 155u
#define OCEAN_MAP_NONE 0xFFFFu
/* Q8.8 solver-grid coordinates per half-res cell: x alongshore, y offshore.
 * y == 0xFFFF marks a cell outside the solver domain. */
typedef struct { uint16_t x_q8, y_q8; } ocean_map_entry_t;
static const ocean_map_entry_t *ocean_map;

/* The render task samples a coherent copy of the solver's normal and foam
 * fields, taken under the lock once per frame. Reading the live fields would
 * tear whenever a 30 Hz step lands inside a 5 Hz frame. */
static int8_t *ocean_normal_snapshot;
static uint8_t *ocean_foam_snapshot;
static int64_t ocean_step_us;      /* cost of the last step */
static int64_t ocean_step_peak_us; /* worst step since the last report */
static uint32_t ocean_steps;

/* Bathymetry is 2 m cells; ocean_sim.h's default dx is for a smaller domain. */
#define OCEAN_SIM_DX_MM 2000u
#define OCEAN_SIM_DAMPING 0.9995

static bool ocean_sim_start(void)
{
    int16_t *h = heap_caps_malloc(OCEAN_CELLS * sizeof(int16_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *vel = heap_caps_malloc(OCEAN_CELLS * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *depth = heap_caps_malloc(OCEAN_CELLS,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int8_t *normal = heap_caps_malloc(2u * OCEAN_CELLS,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *foam = heap_caps_malloc(OCEAN_CELLS,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *damp_residual = heap_caps_malloc(OCEAN_CELLS * sizeof(uint16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!h || !vel || !depth || !normal || !foam || !damp_residual) {
        ESP_LOGE(TAG, "ocean solver: PSRAM allocation failed");
        return false;
    }
    memcpy(depth, nubble_runtime_ocean_depth_bin_start, OCEAN_CELLS);
    ocean_sim = heap_caps_malloc(sizeof(*ocean_sim),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ocean_map_entry_t *map_copy =
        heap_caps_malloc(OCEAN_MAP_ROWS * (LUMINARY_WIDTH / 2U) *
                         sizeof(ocean_map_entry_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ocean_normal_snapshot = heap_caps_malloc(2u * OCEAN_CELLS,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ocean_foam_snapshot = heap_caps_malloc(OCEAN_CELLS,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ocean_sim || !map_copy || !ocean_normal_snapshot || !ocean_foam_snapshot) {
        ESP_LOGE(TAG, "ocean solver: PSRAM allocation failed");
        return false;
    }
    memcpy(map_copy, nubble_runtime_ocean_map_bin_start,
           OCEAN_MAP_ROWS * (LUMINARY_WIDTH / 2U) * sizeof(ocean_map_entry_t));
    ocean_map = map_copy;
    memset(ocean_normal_snapshot, 0, 2u * OCEAN_CELLS);
    memset(ocean_foam_snapshot, 0, OCEAN_CELLS);
    ocean_lock = xSemaphoreCreateMutex();
    if (!ocean_lock) return false;
    ocean_sim_bind(ocean_sim, h, vel, depth, normal, foam, damp_residual);
    ocean_sim_prepare(ocean_sim, OCEAN_SIM_DX_MM,
                      1000u / CONFIG_LUMINARY_OCEAN_SIM_HZ, OCEAN_SIM_DAMPING);
    ocean_sim_reset_surface(ocean_sim);
    return true;
}

/* Push the live sea state into the solver. The renderer's wave components
 * carry height and period; bearing comes from the scene's dominant direction
 * until the runtime bundle carries a per-component bearing. */
static void ocean_sim_apply_conditions_locked(void)
{
    ocean_component_t comp[3];
    unsigned count = runtime_state.wave_component_count;
    if (count > 3u) count = 3u;
    for (unsigned i = 0; i < count; ++i) {
        comp[i].period_ms = runtime_state.waves[i].period_ms;
        comp[i].height_mm = runtime_state.waves[i].height_mm;
        comp[i].from_deg = LUMINARY_WAVE_FROM_DEG;
    }
    if (count == 0u) {
        comp[0].period_ms = LUMINARY_WAVE_PERIOD_MS;
        comp[0].height_mm = LUMINARY_WAVE_HEIGHT_MM;
        comp[0].from_deg = LUMINARY_WAVE_FROM_DEG;
        count = 1u;
    }
    /* Nubble faces east, so the shore normal points out along +90 degrees --
     * the same axis the bathymetry grid is built on. */
    ocean_sim_set_components(ocean_sim, comp, count, 90);
}

static void ocean_sim_task(void *argument)
{
    const TickType_t period = pdMS_TO_TICKS(1000u / CONFIG_LUMINARY_OCEAN_SIM_HZ);
    TickType_t deadline = xTaskGetTickCount();
    while (true) {
        xSemaphoreTake(ocean_lock, portMAX_DELAY);
        const int64_t started = esp_timer_get_time();
        ocean_sim_step(ocean_sim);
        ocean_step_us = esp_timer_get_time() - started;
        if (ocean_step_us > ocean_step_peak_us) ocean_step_peak_us = ocean_step_us;
        ocean_ready = true;
        xSemaphoreGive(ocean_lock);

        if ((++ocean_steps % (CONFIG_LUMINARY_OCEAN_SIM_HZ * 10u)) == 0u) {
            int normal_peak = 0;
            unsigned foam_peak = 0, breaking = 0;
            for (size_t i = 0; i < OCEAN_CELLS; ++i) {
                const int gy = ocean_sim->normal[2u * i + 1u];
                const int magnitude = gy < 0 ? -gy : gy;
                if (magnitude > normal_peak) normal_peak = magnitude;
                if (ocean_sim->foam[i]) {
                    ++breaking;
                    if (ocean_sim->foam[i] > foam_peak) foam_peak = ocean_sim->foam[i];
                }
            }
            ESP_LOGI(TAG, "ocean solver: |dh/dy| peak=%d foam peak=%u breaking=%u cells",
                     normal_peak, foam_peak, breaking);
            /* A step must fit inside the tick to hold real time. */
            ESP_LOGI(TAG, "ocean solver: step=%lld us peak=%lld us budget=%u us "
                          "(%u steps, %.1f s simulated)",
                     (long long)ocean_step_us, (long long)ocean_step_peak_us,
                     1000000u / CONFIG_LUMINARY_OCEAN_SIM_HZ, ocean_steps,
                     (double)ocean_steps / CONFIG_LUMINARY_OCEAN_SIM_HZ);
            ocean_step_peak_us = 0;
        }
        vTaskDelayUntil(&deadline, period);
    }
}
#endif /* CONFIG_LUMINARY_OCEAN_SIM */

// Sine of a Q8.8 phase, linearly interpolated between the two neighbouring
// LUT entries. Only ever called while building the per-frame tables below, so
// the interpolation costs 768 evaluations per frame rather than one per cell.
static int wave_sine_q8(uint32_t phase_q8)
{
    const unsigned index = (phase_q8 >> 8) & 0xFFU;
    const unsigned fraction = phase_q8 & 0xFFU;
    const int low = wave_sine[index];
    const int high = wave_sine[(index + 1U) & 0xFFU];
    return low + (((high - low) * (int)fraction) >> 8);
}

// Everything in the wave field that depends only on the frame -- the advancing
// time phase, the sine, and the component weight -- collapses into one table
// per component, indexed by the component's stored surface phase. The measured
// effect is large: the original per-cell form cost 1261 cycles per phase cell
// and was unchanged by IRAM placement, this form costs 40, a 31x reduction.
//
// Carrying the time phase in Q8.8 here also removes the original wave stutter
// for free. Advancing an 8-bit phase quantized each frame's step to a whole
// unit, swinging apparent wave velocity by up to 39%; the sub-unit phase is
// resolved once per table entry instead of once per cell, so the inner loop is
// no more expensive than the version that stuttered.
static int16_t wave_component_lut[3][256];
static int8_t wave_crest_lut[256];

static void build_wave_component_luts(const uint16_t *time_phase_q8, const int *weight,
                                      unsigned component_count)
{
    for (unsigned component = 0; component < 3U; ++component) {
        // Components the scene does not use contribute zero rather than being
        // skipped, so the per-cell sum stays branchless and fully unrolled.
        const int component_weight = component < component_count ? weight[component] : 0;
        const uint32_t phase = component < component_count ? time_phase_q8[component] : 0U;
        for (unsigned stored = 0; stored < 256U; ++stored) {
            const uint32_t argument = (((uint32_t)stored + 64U) << 8) - phase;
            wave_component_lut[component][stored] =
                (int16_t)(wave_sine_q8(argument & 0xFFFFU) * component_weight);
        }
    }
    // Breaker foam samples the dominant component at a shore-bent phase. Fold
    // the time phase into its own table too, so the row pass can store the raw
    // stored phase and the per-pixel lookup stays a single indexed load.
    for (unsigned stored = 0; stored < 256U; ++stored) {
        const uint32_t argument = ((uint32_t)stored << 8) - time_phase_q8[0];
        wave_crest_lut[stored] = (int8_t)wave_sine_q8(argument & 0xFFFFU);
    }
}

#if CONFIG_LUMINARY_OCEAN_SIM
// Wave row fed by the shallow-water solver. Each half-res cell samples the
// solver cell the fitted projection says it looks at: shade from the offshore
// surface gradient (the same quantity the analytic path derives from its
// sine), foam from depth-limited breaking. Cells outside the solver domain
// fall back to the analytic tables, which the caller has already built.
__attribute__((noinline)) IRAM_ATTR
static void compute_wave_row_sim(const uint8_t *phase_src, int shade_recip_q16,
                                 unsigned map_row)
{
    const ocean_map_entry_t *map = ocean_map + (size_t)map_row * (LUMINARY_WIDTH / 2U);
    const int8_t *normals = ocean_normal_snapshot;
    const uint8_t *foam = ocean_foam_snapshot;
    for (unsigned phase_x = 0; phase_x < OCEAN_PHASE_WIDTH;
         ++phase_x, phase_src += OCEAN_PHASE_COMPONENTS) {
        const unsigned gx_q8 = map[phase_x].x_q8;
        const unsigned gy_q8 = map[phase_x].y_q8;
        if (gy_q8 == OCEAN_MAP_NONE) {
            const uint8_t dominant = phase_src[0];
            const int normal_light = wave_component_lut[0][dominant] +
                                     wave_component_lut[1][phase_src[1]] +
                                     wave_component_lut[2][phase_src[2]];
            int shade = normal_light * shade_recip_q16 >> 16;
            if (shade < -32) shade = -32;
            if (shade > 31) shade = 31;
            /* Same 4x quantum as the solver cells so one dither pass in the
             * water row serves both. */
            wave_shade_row[phase_x] = (int8_t)(shade << 2);
            wave_breaker_phase_row[phase_x] = dominant;
            wave_foam_row[phase_x] = 255u;
            wave_dx_row[phase_x] = 0;
            wave_dy_row[phase_x] = 0;
            continue;
        }
        /* Bilinear sample of the solver fields. Nearest-cell sampling drew
         * the 2 m world grid on the screen as hard-edged blocks; in the near
         * field one solver cell spans tens of pixels and the frame-to-frame
         * shade steps read as a flickering checkerboard. */
        const unsigned cx = gx_q8 >> 8, cy = gy_q8 >> 8;
        const int fx = (int)(gx_q8 & 0xFFu), fy = (int)(gy_q8 & 0xFFu);
        const size_t cell00 = (size_t)cy * OCEAN_NX + cx;
        const size_t cell10 = cell00 + OCEAN_NX;

        const int n00 = normals[2u * cell00 + 1u];
        const int n01 = normals[2u * (cell00 + 1u) + 1u];
        const int n10 = normals[2u * cell10 + 1u];
        const int n11 = normals[2u * (cell10 + 1u) + 1u];
        const int top = n00 + ((n01 - n00) * fx >> 8);
        const int bottom = n10 + ((n11 - n10) * fx >> 8);
        const int gradient = top + ((bottom - top) * fy >> 8);
        /* The alongshore gradient only steers a coarse pixel displacement
         * that is then clamped and distance-tapered; a nearest tap is
         * indistinguishable from bilinear there and saves four PSRAM reads
         * per cell (measured as part of a 37 ms wave phase). */
        const int gradient_x = normals[2u * cell00];

        /* The analytic shade is the surface gradient along the propagation
         * direction, which travels shoreward: -dh/dy in solver axes. Carried
         * at four times the colour LUT's 16-level resolution; the water pass
         * dithers the quantisation, because at near-field magnification each
         * LUT step otherwise draws a hard terrace across the cell. The x24
         * gain is x6 in LUT levels, calibrated to the mild end of the live
         * conditions: a 0.5 m diffracted swell shows gradients of only a few
         * units, and mapping those to nothing left the sea visibly frozen. */
        int shade = -gradient * 24;
        if (shade < -128) shade = -128;
        if (shade > 127) shade = 127;
        wave_shade_row[phase_x] = (int8_t)shade;
        wave_breaker_phase_row[phase_x] = 0u;

        /* Refraction: the moving surface displaces where the authored
         * texture is read from, so the photograph's own chop travels with
         * the wave instead of sitting frozen under a brightness wash.
         * Alongshore slope shifts the lookup sideways, shoreward slope
         * vertically; both are bounded to stay inside the rows the base
         * copy has recently pulled through the cache. */
        int dx = gradient_x * 2;
        if (dx < -7) dx = -7;
        if (dx > 7) dx = 7;
        /* Bounded to one row: dy sets how many framebuffer rows the
         * refraction reads span, which is what couples render cost to sea
         * state. The horizontal shift stays in-row and carries most of the
         * refraction feel. */
        int dy = -gradient / 4;
        if (dy < -1) dy = -1;
        if (dy > 1) dy = 1;
        wave_dx_row[phase_x] = (int8_t)dx;
        wave_dy_row[phase_x] = (int8_t)dy;

        const int f00 = foam[cell00], f01 = foam[cell00 + 1u];
        const int f10 = foam[cell10], f11 = foam[cell10 + 1u];
        const int foam_top = f00 + ((f01 - f00) * fx >> 8);
        const int foam_bottom = f10 + ((f11 - f10) * fx >> 8);
        int breaking = foam_top + ((foam_bottom - foam_top) * fy >> 8);
        if (breaking > 254) breaking = 254;
        wave_foam_row[phase_x] = (uint8_t)breaking;
    }
}
#endif /* CONFIG_LUMINARY_OCEAN_SIM */

// Kept out of line and in IRAM deliberately. render_runtime_frame is inlined
// into its caller, so an IRAM_ATTR there placed nothing -- an earlier
// measurement concluded IRAM did not matter for exactly that reason. Measured
// on this loop: 244 cycles per cell from flash against 40 from IRAM.
__attribute__((noinline)) IRAM_ATTR
static void compute_wave_row(const uint8_t *src, int shade_recip_q16)
{
    for (unsigned phase_x = 0; phase_x < OCEAN_PHASE_WIDTH;
         ++phase_x, src += OCEAN_PHASE_COMPONENTS) {
        const uint8_t dominant = src[0];
        const int normal_light = wave_component_lut[0][dominant] +
                                 wave_component_lut[1][src[1]] +
                                 wave_component_lut[2][src[2]];
        int shade = normal_light * shade_recip_q16 >> 16;
        if (shade < -32) shade = -32;
        if (shade > 31) shade = 31;
        /* Shade rows are stored at 4x the colour LUT's quantum everywhere so
         * the water pass can dither one representation. */
        wave_shade_row[phase_x] = (int8_t)(shade << 2);
        wave_breaker_phase_row[phase_x] = dominant;
    }
}

// The whole sky row, in IRAM. Cloud compositing was costing over a second per
// frame because composite_cloud_shell is flash-resident and was called once per
// shell per sky pixel -- 894k calls, each re-deriving an atlas row that is
// constant across the row. Hoisting the atlas row out of the pixel loop and
// moving the loop here took the sky phase from 1105 ms to 95 ms.
//
// Shell-outer/pixel-inner keeps the per-pixel blend order identical to the old
// pixel-outer/shell-inner form -- each pixel still sees high, mid, then low --
// while letting each shell's atlas row be walked sequentially.
//
// Measured, and rejected: fusing the shells and the grading into a single
// per-pixel pass, so each pixel is read once and written once, is 40x slower
// (95 ms -> 3792 ms) whether it reads the authored frame directly or the
// cache-hot copied row. Making the three-shell loop the inner loop puts its
// hoisted per-shell state into stack arrays that are reloaded per pixel, and
// that costs far more than the row traversals it saves. Separate sequential
// passes over a row that is already hot in cache are what this core wants.
__attribute__((noinline)) IRAM_ATTR
static void render_sky_row(uint8_t *row, unsigned y, const int shift_x[3],
                           const int shift_y[3], unsigned cloud_cover_permille)
{
    if (cloud_cover_permille > 0U) {
        // Feather the bottom 22 rows so no shell can create a false horizon.
        const unsigned clearance = LUMINARY_RUNTIME_HORIZON - y;
        const unsigned feather = clearance < 22U ? clearance : 22U;
        // Compose the three shells once per QUARTER-res column into a single
        // transmission plus premultiplied colour, then blend each pixel once.
        // The shells only vary at atlas resolution -- 256 texels across 1024
        // pixels -- so quarter-column granularity loses nothing visible, and
        // it replaces three per-pixel alpha blends with one.
        //
        // Shell-outer, column-inner is load-bearing: the pixel-outer form
        // with per-iteration shell indexing through stack arrays measured
        // 1.2M cycles per row against 13k for the blend loop below -- the
        // same order-of-magnitude collapse as the abandoned fused-sky
        // experiment. Each shell pass here walks its atlas row and the
        // compose rows sequentially. Measured: the whole composed sky is
        // 3.4x cheaper per overcast frame than the three per-pixel passes.
        memset(cloud_row_trans, 255, LUMINARY_WIDTH / 4U);
        memset(cloud_row_add, 0, (LUMINARY_WIDTH / 4U) * 3U);
        for (unsigned shell = 0; shell < 3U; ++shell) {
            const cloud_shell_t *const shell_state = &runtime_state.shells[shell];
            // The atlas row depends only on y and this frame's wind offset.
            const int atlas_y = mirror_cloud_y((int)cloud_y_lut[y] + shift_y[shell]);
            const uint8_t *const atlas_row = shell_state->atlas +
                (size_t)atlas_y * LUMINARY_CLOUD_TEXTURE_WIDTH * 2U;
            const unsigned bias = shell_state->blue_bias;
            const unsigned half_bias = bias / 2U;
            const int offset_x = shift_x[shell];
            const uint8_t *const column_lut = cloud_x_lut;
            for (unsigned quarter = 0; quarter < LUMINARY_WIDTH / 4U; ++quarter) {
                const unsigned atlas_x = (unsigned)wrap_cloud_x(
                    (int)column_lut[quarter * 4U] + offset_x);
                const uint8_t *const texel = atlas_row + (size_t)atlas_x * 2U;
                unsigned alpha = texel[1] * cloud_cover_permille / 1000U;
                alpha = alpha * feather / 22U;
                if (alpha == 0U) continue;
                const unsigned luminance = texel[0];
                const unsigned cloud_g = luminance > half_bias ? luminance - half_bias : 0U;
                const unsigned cloud_r = luminance > bias ? luminance - bias : 0U;
                const unsigned keep = 255U - alpha;
                uint8_t *const add = cloud_row_add + (size_t)quarter * 3U;
                add[0] = (uint8_t)((add[0] * keep + luminance * alpha) / 255U);
                add[1] = (uint8_t)((add[1] * keep + cloud_g * alpha) / 255U);
                add[2] = (uint8_t)((add[2] * keep + cloud_r * alpha) / 255U);
                cloud_row_trans[quarter] = (uint8_t)(cloud_row_trans[quarter] * keep / 255U);
            }
        }
        uint8_t *bgr = row;
        for (unsigned x = 0; x < LUMINARY_WIDTH; ++x, bgr += LUMINARY_BPP) {
            const unsigned quarter = x >> 2U;
            const unsigned trans = cloud_row_trans[quarter];
            if (trans == 255U) continue;
            const uint8_t *const add = cloud_row_add + (size_t)quarter * 3U;
            bgr[0] = (uint8_t)(bgr[0] * trans / 255U + add[0]);
            bgr[1] = (uint8_t)(bgr[1] * trans / 255U + add[1]);
            bgr[2] = (uint8_t)(bgr[2] * trans / 255U + add[2]);
        }
    }

    // Grade the full atmosphere after cloud compositing so real clouds catch
    // the same golden-hour illumination. Per-pixel calls into flash-resident
    // helpers used to dominate the frame budget (measured: 3.0 s of a 3.5 s
    // frame, ~3640 cycles/pixel for ~40 cycles of work), so the solar state is
    // hoisted to row constants and the inner loop makes no calls.
    const int shift_b = (int)runtime_state.sky_b - (int)LUMINARY_SKY_B;
    const int shift_g = (int)runtime_state.sky_g - (int)LUMINARY_SKY_G;
    const int shift_r = (int)runtime_state.sky_r - (int)LUMINARY_SKY_R;
    const unsigned warmth = sunset_warmth_255();
    unsigned glow_row = 0U;
    if (warmth > 0U) {
        const unsigned distance = LUMINARY_RUNTIME_HORIZON - y;
        const unsigned vertical = distance < 190U ? (190U - distance) * 255U / 190U : 0U;
        glow_row = warmth * vertical / 255U;
    }
    const bool sunset_left = runtime_state.sun_relative_azimuth_deci_deg < 0;
    unsigned mode = 0U;
    unsigned light = 255U;
    // Integer clamp, not fmin() on doubles: the P4 FPU is single-precision
    // only, so a double compare per sky pixel is soft-float emulation.
    if (runtime_state.sun_mode == 1U && runtime_state.sun_altitude_deci_deg < 0) {
        const int negative_altitude = -runtime_state.sun_altitude_deci_deg;
        const unsigned depth = (unsigned)(negative_altitude > 60 ? 60 : negative_altitude);
        light = 255U - depth * 85U / 60U;
        mode = 1U;
    } else if (runtime_state.sun_mode == 2U) {
        mode = 2U;
    } else if (runtime_state.sun_mode == 3U) {
        mode = 3U;
    }

    for (unsigned x = 0; x < LUMINARY_WIDTH; ++x) {
        uint8_t *bgr = row + (size_t)x * LUMINARY_BPP;
        bgr[0] = clamp_channel((int)bgr[0] + shift_b);
        bgr[1] = clamp_channel((int)bgr[1] + shift_g);
        bgr[2] = clamp_channel((int)bgr[2] + shift_r);
        if (warmth > 0U) {
            const unsigned horizontal = sunset_left ?
                255U - x * 96U / (LUMINARY_WIDTH - 1U) :
                159U + x * 96U / (LUMINARY_WIDTH - 1U);
            const unsigned glow = glow_row * horizontal / 255U;
            bgr[0] = (uint8_t)((bgr[0] * (255U - glow) + 78U * glow) / 255U);
            bgr[1] = (uint8_t)((bgr[1] * (255U - glow) + 126U * glow) / 255U);
            bgr[2] = (uint8_t)((bgr[2] * (255U - glow) + 248U * glow) / 255U);
        }
        if (mode == 1U) {
            bgr[0] = (uint8_t)(bgr[0] * light / 255U);
            bgr[1] = (uint8_t)(bgr[1] * light / 255U);
            bgr[2] = (uint8_t)(bgr[2] * light / 255U);
        } else if (mode == 2U) {
            bgr[0] = (uint8_t)(bgr[0] * 34U / 100U);
            bgr[1] = (uint8_t)(bgr[1] * 24U / 100U);
            bgr[2] = (uint8_t)(bgr[2] * 15U / 100U);
        } else if (mode == 3U) {
            bgr[0] = (uint8_t)(bgr[0] * 22U / 100U);
            bgr[1] = (uint8_t)(bgr[1] * 12U / 100U);
            bgr[2] = (uint8_t)(bgr[2] * 7U / 100U);
        }
    }

    // Only the few pixels inside the moon's disc are revisited.
    if (runtime_state.moon_visible) {
        const int dy = (int)y - runtime_state.moon_y;
        if (dy * dy <= 144) {
            const int terminator = 12 - (int)(runtime_state.moon_illumination_permille * 24U / 1000U);
            for (int dx = -12; dx <= 12; ++dx) {
                const int mx = runtime_state.moon_x + dx;
                if (mx < 0 || mx >= (int)LUMINARY_WIDTH) continue;
                if (dx * dx + dy * dy > 144 || dx < terminator) continue;
                uint8_t *bgr = row + (size_t)mx * LUMINARY_BPP;
                const unsigned alpha = 185U;
                bgr[0] = (uint8_t)((bgr[0] * (255U - alpha) + 226U * alpha) / 255U);
                bgr[1] = (uint8_t)((bgr[1] * (255U - alpha) + 226U * alpha) / 255U);
                bgr[2] = (uint8_t)((bgr[2] * (255U - alpha) + 214U * alpha) / 255U);
            }
        }
    }
}

// Pixel-outer with per-pixel cell lookups, deliberately. A cell-outer
// restructure sharing the LUT rows, displacement and foam across each
// two-pixel cell read 6x SLOWER on target (74k -> 400k cycles per row)
// with byte-identical memory streams; the cause was never pinned down
// (not IRAM placement, not divisions, not the row copy, not an explicit
// prefetch of the displacement window). This shape is the one that
// measures fast; do not "clean it up" without the cycle counter running.
//
// noinline + IRAM is load-bearing, recorded here for the THIRD time: this
// attribute line was silently lost in a refactor splice and the pass ran
// inlined from flash for hours, costing anywhere from 63 to 940 ms in
// layout-and-contention roulette that got misattributed to sea state,
// copies, and pacing in turn. If this function is missing from the ELF
// symbol table, that is the bug.
__attribute__((noinline)) IRAM_ATTR
static void render_water_row(uint8_t *row, const uint8_t *base, unsigned y,
                             unsigned water_warmth)
{
    const size_t row_pixel = (size_t)y * LUMINARY_WIDTH;
    const unsigned sun_mode = runtime_state.sun_mode;
    /* Hoisted: the LUT lives behind a static pointer now, and the byte
     * writes below alias everything, so without this local the compiler
     * reloads that pointer for every one of the three lookups per pixel. */
    const wave_color_lut_t *const colour = (const wave_color_lut_t *)wave_color_lut_mem;
#if CONFIG_LUMINARY_OCEAN_SIM
    /* Row constant; recomputing it per pixel put a division inside the
     * hottest loop in the renderer. */
    const int reach = (int)(y - LUMINARY_RUNTIME_HORIZON);
    const int taper_q8 = reach >= 160 ? 256 : reach * 256 / 160;
#endif
    unsigned glow = 0U;
    if (water_warmth > 0U) {
        const unsigned distance = y - LUMINARY_RUNTIME_HORIZON;
        const unsigned vertical = distance < 170U ? (170U - distance) * 255U / 170U : 0U;
        glow = water_warmth * vertical / 255U * 128U / 255U;
    }
    for (unsigned x = 0; x < LUMINARY_WIDTH; ++x) {
        const size_t pixel = row_pixel + x;
        // Printed rock and shoreline below the horizon: the authored pixel is
        // already in place from the row copy and no sea shading applies.
        if (!runtime_water_pixel(pixel)) continue;
        uint8_t *const bgr = row + (size_t)x * LUMINARY_BPP;

        // Shade rows carry four bits of sub-LUT precision; the ordered
        // dither trades the hard terrace each 16-level step would draw
        // across a magnified near-field cell for fine noise.
        const unsigned phase_x = x >> 1U;
        int quantised = (int)wave_shade_row[phase_x] + 128 +
                        (int)shade_dither[y & 3U][x & 3U] - 8;
        if (quantised < 0) quantised = 0;
        if (quantised > 255) quantised = 255;
        const unsigned shade_index = (unsigned)quantised >> 4U;
#if CONFIG_LUMINARY_OCEAN_SIM
        // Refraction: read the authored texture through the moving surface.
        // The displaced rows are within a few lines of the current one, so
        // the reads stay inside what the row copies already cached. The
        // displacement scales with distance below the horizon: a fixed pixel
        // shift up near the horizon would correspond to a world-space shift
        // of tens of metres, and the far field visibly sheared.
        {
            int sx = (int)x + (wave_dx_row[phase_x] * taper_q8 >> 8);
            if (sx < 0) sx = 0;
            if (sx >= (int)LUMINARY_WIDTH) sx = (int)LUMINARY_WIDTH - 1;
            int sy = (int)y + (wave_dy_row[phase_x] * taper_q8 >> 8);
            if (sy < (int)LUMINARY_RUNTIME_HORIZON) sy = (int)LUMINARY_RUNTIME_HORIZON;
            if (sy >= (int)LUMINARY_HEIGHT) sy = (int)LUMINARY_HEIGHT - 1;
            const uint8_t *source = base +
                ((size_t)sy * LUMINARY_WIDTH + (size_t)sx) * LUMINARY_BPP;
            bgr[0] = (*colour)[0][shade_index][source[0]];
            bgr[1] = (*colour)[1][shade_index][source[1]];
            bgr[2] = (*colour)[2][shade_index][source[2]];
        }
#else
        bgr[0] = (*colour)[0][shade_index][bgr[0]];
        bgr[1] = (*colour)[1][shade_index][bgr[1]];
        bgr[2] = (*colour)[2][shade_index][bgr[2]];
#endif

#if CONFIG_LUMINARY_OCEAN_SIM
        // Solver foam: depth-limited breaking measured on the bathymetry,
        // already shaped in space by the shoaling surf. 255 = no solver data.
        const unsigned sim_foam = wave_foam_row[phase_x];
        if (sim_foam != 255u) {
            if (sim_foam > 0u) {
                const unsigned bounded_foam = sim_foam > 240u ? 240u : sim_foam;
                for (unsigned channel = 0; channel < 3U; ++channel) {
                    const unsigned value = bgr[channel];
                    bgr[channel] = (uint8_t)(value +
                        ((255U - value) * bounded_foam >> 8));
                }
            }
        } else
#endif
        // Shore distance is zero at physical rock. The phase bends over the
        // final pixels so an incoming crest steepens and rises into foam at
        // that exact boundary, never over land.
        {
        const uint8_t shore = shore_distance_ram[pixel];
        if (shore < 36U) {
            // wave_breaker_phase_row holds the stored surface phase, untouched
            // by time; wave_crest_lut carries this frame's Q8.8 advance, so the
            // foam edge moves at the same sub-unit rate as the shading.
            const int crest =
                wave_crest_lut[(uint8_t)(wave_breaker_phase_row[phase_x] + shore * 5U)];
            if (crest > 58) {
                const unsigned foam = (unsigned)((crest - 58) * (36U - shore)) / 48U;
                const unsigned bounded_foam = foam > 240U ? 240U : foam;
                for (unsigned channel = 0; channel < 3U; ++channel) {
                    const unsigned value = bgr[channel];
                    bgr[channel] = (uint8_t)(value + ((255U - value) * bounded_foam >> 8));
                }
            }
        }
        }
        if (water_warmth > 0U) {
            bgr[0] = (uint8_t)((bgr[0] * (255U - glow) + 68U * glow) / 255U);
            bgr[1] = (uint8_t)((bgr[1] * (255U - glow) + 102U * glow) / 255U);
            bgr[2] = (uint8_t)((bgr[2] * (255U - glow) + 220U * glow) / 255U);
            if (sun_mode == 2U) {
                bgr[0] = (uint8_t)(bgr[0] * 52U / 100U);
                bgr[1] = (uint8_t)(bgr[1] * 42U / 100U);
                bgr[2] = (uint8_t)(bgr[2] * 34U / 100U);
            } else if (sun_mode == 3U) {
                bgr[0] = (uint8_t)(bgr[0] * 34U / 100U);
                bgr[1] = (uint8_t)(bgr[1] * 24U / 100U);
                bgr[2] = (uint8_t)(bgr[2] * 18U / 100U);
            }
        }
    }
}

// The row-loop skeleton runs 600 times a frame and was the last per-row code
// executing from flash. That made frame time hostage to link-layout luck:
// the same binary measured 209 ms with profiling compiled in and 310 ms
// without, in the same conditions minutes apart, because unrelated code
// shifts moved this loop across flash-cache lines. (An earlier note here
// claimed IRAM placement changed nothing -- that experiment silently tested
// nothing, because the function was inlined into app_main and IRAM_ATTR
// never applied. noinline is load-bearing.)
static void render_runtime_frame(uint8_t *destination, const uint8_t *base, uint64_t elapsed_ms,
                                bool cycle_base, bool water_rows_hold_base)
{
    // Start from the authored frame in one DMA-friendly copy. Per-pixel
    // three-byte memcpy calls were dominating render time on the P4.
    PROF_ZERO(prof_wave_us = prof_sky_us = prof_water_us = prof_basecopy_us = 0);
    PROF_STAMP_MUT(prof_mark);
    const size_t row_bytes = (size_t)LUMINARY_WIDTH * LUMINARY_BPP;
    const unsigned reflected[3] = {runtime_state.sky_b, runtime_state.sky_g,
                                   runtime_state.sky_r};
    const unsigned water_warmth = sunset_warmth_255();

    if (cycle_base) {
        memcpy(destination, base, LUMINARY_FRAME_BYTES);
        const unsigned cloud_cover_permille = runtime_state.cloud_cover_permille;
        int cloud_shift_x[3] = {0};
        int cloud_shift_y[3] = {0};
        if (cloud_cover_permille > 0U) {
            const int64_t time_ms = (int64_t)(elapsed_ms % 21600000ULL);
            for (unsigned shell = 0; shell < 3; ++shell) {
                const int64_t height_mm = (int64_t)runtime_state.shells[shell].height_m * 1000LL;
                cloud_shift_x[shell] = (int)(-(int64_t)runtime_state.shells[shell].wind_north_mmps *
                    time_ms * LUMINARY_CLOUD_TEXTURE_WIDTH * 256LL / (height_mm * 1919LL) >> 8);
                cloud_shift_y[shell] = (int)((int64_t)runtime_state.shells[shell].wind_east_mmps *
                    time_ms * LUMINARY_CLOUD_TEXTURE_HEIGHT * 256LL / (height_mm * 1187LL) >> 8);
            }
        }
        for (unsigned y = 0; y < LUMINARY_HEIGHT; ++y) {
            for (unsigned x = 0; x < LUMINARY_WIDTH; ++x) {
                const size_t pixel = (size_t)y * LUMINARY_WIDTH + x;
                const size_t target = pixel * LUMINARY_BPP;
                if (!runtime_water_pixel(pixel)) {
                    if (y < LUMINARY_RUNTIME_HORIZON && cloud_cover_permille > 0U) {
                        for (unsigned shell = 0; shell < 3; ++shell) {
                            composite_cloud_shell(destination + target, &runtime_state.shells[shell],
                                                  x, y, cloud_shift_x[shell],
                                                  cloud_shift_y[shell],
                                                  cloud_cover_permille);
                        }
                    }
                    if (y < LUMINARY_RUNTIME_HORIZON) {
                        grade_sky_pixel(destination + target, x, y);
                    }
                } else if (water_warmth > 0U) {
                    grade_water_pixel(destination + target, y);
                }
            }
        }
        overlay_real_stars(destination, cloud_shift_x, cloud_shift_y, cloud_cover_permille);
        return;
    }

    // Reflection and night grading depend only on the source channel and the
    // 16 quantized normal values. Build the result once per frame instead of
    // performing six multiplies for every water pixel in PSRAM -- and only
    // when an input actually changed: the table depends on the sky colour
    // and sun mode, which move on 30-second solar updates, not per frame.
    // Rebuilding every frame cost a steady 4 ms.
    static uint32_t colour_lut_key = 0xFFFFFFFFu;
    const uint32_t colour_key = ((uint32_t)runtime_state.sky_b << 24) |
                                ((uint32_t)runtime_state.sky_g << 16) |
                                ((uint32_t)runtime_state.sky_r << 8) |
                                runtime_state.sun_mode;
    if (colour_key != colour_lut_key) {
        colour_lut_key = colour_key;
    for (unsigned channel = 0; channel < 3U; ++channel) {
        for (unsigned shade_index = 0; shade_index < 16U; ++shade_index) {
            const int shade = -30 + (int)shade_index * 4;
            const unsigned glint = shade >= 0 ? (unsigned)shade * 3U : 0U;
            for (unsigned source = 0; source < 256U; ++source) {
                int value = (int)source;
                if (shade >= 0) {
                    value += ((int)reflected[channel] - value) * (int)glint >> 7;
                }
                value = (int)clamp_channel(value + shade);
                if (runtime_state.sun_mode == 2U) {
                    static const unsigned scale[3] = {52U, 42U, 34U};
                    value = value * (int)scale[channel] / 100;
                } else if (runtime_state.sun_mode == 3U) {
                    static const unsigned scale[3] = {34U, 24U, 18U};
                    value = value * (int)scale[channel] / 100;
                }
                wave_color_lut[channel][shade_index][source] = (uint8_t)value;
            }
        }
    }
    }
    const unsigned cloud_cover_permille = runtime_state.cloud_cover_permille;
    const unsigned component_count = runtime_state.wave_component_count > 0U ?
                                     (runtime_state.wave_component_count > 3U ? 3U :
                                      runtime_state.wave_component_count) : 1U;
    // Q8.8: a whole unit of phase is 256 here. The frame step at 6 fps is a
    // few units, so rounding it to an integer each frame -- as an 8-bit phase
    // must -- is what made the swell advance unevenly.
    uint16_t wave_time_phase_q8[3] = {0U, 0U, 0U};
    int wave_weight[3] = {1, 1, 1};
    int total_wave_weight = 0;
    for (unsigned component = 0; component < component_count; ++component) {
        const wave_component_t *wave = &runtime_state.waves[component];
        const uint32_t period_ms = wave->period_ms > 500U ? wave->period_ms : 500U;
        wave_time_phase_q8[component] =
            (uint16_t)(elapsed_ms * 65536ULL / period_ms);
        wave_weight[component] = 1 + (int)(wave->height_mm / 80U > 30U ?
                                          30U : wave->height_mm / 80U);
        total_wave_weight += wave_weight[component];
    }
    build_wave_component_luts(wave_time_phase_q8, wave_weight, component_count);
    int cloud_shift_x[3] = {0};
    int cloud_shift_y[3] = {0};
    if (cloud_cover_permille > 0U) {
        const int64_t time_ms = (int64_t)(elapsed_ms % 21600000ULL);
        for (unsigned shell = 0; shell < 3; ++shell) {
            const int64_t height_mm = (int64_t)runtime_state.shells[shell].height_m * 1000LL;
            cloud_shift_x[shell] = (int)(-(int64_t)runtime_state.shells[shell].wind_north_mmps *
                time_ms * LUMINARY_CLOUD_TEXTURE_WIDTH * 256LL / (height_mm * 1919LL) >> 8);
            cloud_shift_y[shell] = (int)((int64_t)runtime_state.shells[shell].wind_east_mmps *
                time_ms * LUMINARY_CLOUD_TEXTURE_HEIGHT * 256LL / (height_mm * 1187LL) >> 8);
        }
    }
    PROF_SET(prof_lut_us, prof_mark);
    // Hoist the phase field out of the inner loop: the compiler cannot prove
    // the wave_shade_row writes do not alias runtime_state, so it reloads this
    // pointer on every component of every cell otherwise.
    const uint8_t *const ocean_phase = runtime_state.ocean_phase;
#if CONFIG_LUMINARY_OCEAN_SIM
    bool sim_frame = false;
    if (ocean_lock && ocean_ready) {
        // One coherent copy of the solver output per frame; the 30 Hz solver
        // keeps stepping while this 72 KB copy and the frame render run.
        xSemaphoreTake(ocean_lock, portMAX_DELAY);
        memcpy(ocean_normal_snapshot, ocean_sim->normal, 2u * OCEAN_CELLS);
        memcpy(ocean_foam_snapshot, ocean_sim->foam, OCEAN_CELLS);
        xSemaphoreGive(ocean_lock);
        sim_frame = true;
    }
#endif
    for (unsigned y = 0; y < LUMINARY_HEIGHT; ++y) {
        // Fold the base image copy into the row pass. The same 1.84 MB moves,
        // but each row is still hot in cache when it is processed, instead of
        // being written once as a whole frame and re-fetched row by row.
        //
        // Measured: eliminating this copy entirely, by reading the authored
        // pixels straight from `base` inside the sky and water passes, is a
        // net loss. basecopy went to 0 but sky rose 27.4 -> 52.8 ms and water
        // 39.6 -> 69.5 ms, for 311 -> 316 ms overall. The copy is not
        // redundant work: the frame has to read 1.84 MB and write 1.84 MB
        // either way, and memcpy moves it at ~75 MB/s where scattered
        // three-byte accesses inside the pixel loops move it slower.
        PROF_STAMP(prof_c0);
        // A third DMA attempt on this copy, third rejection, all measured:
        // synchronous DMA lost to memcpy outright (47.0 vs 45.9 ms), and
        // row-pipelined AXI GDMA -- issue row y+1 while shading row y --
        // cost 830 ms in completion waits while its bus traffic slowed every
        // other phase (sky 33 -> 56 ms, water 63 -> 84 ms). The engine also
        // cannot be primed further ahead without the driver's per-row cache
        // maintenance costs growing to match. Plain memcpy stays.
#if CONFIG_LUMINARY_OCEAN_SIM
        // Below the horizon the copy serves only the printed-rock pixels:
        // the refracting water pass rewrites every water pixel straight from
        // the authored frame, and the base is decoded exactly once, so a
        // framebuffer that has been through one static frame already holds
        // the right rock bytes. Measured with the pixel-outer water pass:
        // skipping saves 26 ms of copy for a 15 ms slowdown of the displaced
        // refraction reads the copy was accidentally prefetching.
        if (y < LUMINARY_RUNTIME_HORIZON || !(sim_frame && water_rows_hold_base)) {
            memcpy(destination + (size_t)y * row_bytes, base + (size_t)y * row_bytes,
                   row_bytes);
        }
#else
        memcpy(destination + (size_t)y * row_bytes, base + (size_t)y * row_bytes, row_bytes);
#endif
        PROF_STAMP(prof_w0);
        PROF_ADD(prof_basecopy_us, prof_c0, prof_w0);
        if (y >= LUMINARY_RUNTIME_HORIZON &&
            (((y & 1U) == 0U) || y == LUMINARY_RUNTIME_HORIZON)) {
            const size_t phase_row = (size_t)(y >> 1U) * OCEAN_PHASE_WIDTH *
                                     OCEAN_PHASE_COMPONENTS;
            const int shade_recip_q16 = total_wave_weight > 0 ?
                                         65536 / (total_wave_weight * 4) : 0;
#if CONFIG_LUMINARY_OCEAN_SIM
            if (sim_frame) {
                compute_wave_row_sim(ocean_phase + phase_row, shade_recip_q16,
                                     (y >> 1U) - OCEAN_MAP_ROW0);
            } else {
                memset(wave_foam_row, 255, LUMINARY_WIDTH / 2U);
                compute_wave_row(ocean_phase + phase_row, shade_recip_q16);
            }
#else
            compute_wave_row(ocean_phase + phase_row, shade_recip_q16);
#endif
        }
        PROF_STAMP(prof_r0);
        PROF_ADD(prof_wave_us, prof_w0, prof_r0);
        if (y < LUMINARY_RUNTIME_HORIZON) {
            // The water mask has no set pixels above the horizon (verified
            // against the asset), so the whole row is sky and can be graded
            // once per row instead of once per pixel.
            uint8_t *const sky_row = destination + (size_t)y * row_bytes;
            render_sky_row(sky_row, y, cloud_shift_x, cloud_shift_y, cloud_cover_permille);
            PROF_ADD(prof_sky_us, prof_r0, esp_timer_get_time());
            continue;
        }
        render_water_row(destination + (size_t)y * row_bytes, base, y, water_warmth);
        PROF_STAMP(prof_r1);
        PROF_ADD(prof_water_us, prof_r0, prof_r1);
    }
    PROF_RESTAMP(prof_mark);
    overlay_real_stars(destination, cloud_shift_x, cloud_shift_y, cloud_cover_permille);
    PROF_SET(prof_stars_us, prof_mark);
}

void app_main(void)
{
    initialize_runtime_state();
    mount_and_load_sd_cache();
    start_wifi();
    if (CONFIG_LUMINARY_WIFI_SSID[0] != '\0') {
        start_runtime_server();
    } else {
        ESP_LOGW(TAG, "Runtime API endpoints disabled: Wi-Fi SSID not configured");
    }

    const uint8_t *asset = nubble_runtime_base_jpg_start;
    const size_t asset_size = nubble_runtime_base_jpg_end - asset;

    esp_ldo_channel_handle_t mipi_ldo = NULL;
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = LUMINARY_MIPI_LDO_CHANNEL,
        .voltage_mv = LUMINARY_MIPI_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_config, &mipi_ldo));
    const esp_lcd_dsi_bus_config_t dsi_config = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&dsi_config, &dsi_bus));
    const esp_lcd_dbi_io_config_t dbi_config = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io));

    // The physical 7B panel scans RGB888. RGB565 is accepted by the API but
    // is read as 24-bit scanout on this board, producing cyan/magenta noise.
    esp_lcd_dpi_panel_config_t dpi_config = EK79007_1024_600_PANEL_60HZ_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB888);
    dpi_config.num_fbs = 2; /* Decode into the inactive frame, then swap it in. */
    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = LUMINARY_MIPI_DSI_LANES,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LUMINARY_LCD_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(dbi_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // EK79007 has no esp_lcd_panel_disp_on_off implementation. Panel init
    // already enables it; treating ESP_ERR_NOT_SUPPORTED as fatal causes a
    // reboot loop and the visible cyan flash on the 7B.

    uint8_t *framebuffer[2] = {NULL, NULL};
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 2, (void **)&framebuffer[0], (void **)&framebuffer[1]));
    uint8_t *render_buffer = heap_caps_malloc(LUMINARY_FRAME_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(render_buffer ? ESP_OK : ESP_ERR_NO_MEM);

    jpeg_decode_memory_alloc_cfg_t input_config = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    jpeg_decode_memory_alloc_cfg_t output_config = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t input_capacity = 0;
    size_t decoded_capacity = 0;
    const size_t minimum_input_capacity = LUMINARY_WAVE_CYCLE_MAX_FRAME_BYTES + 4096U;
    uint8_t *input = jpeg_alloc_decoder_mem(minimum_input_capacity, &input_config, &input_capacity);
    uint8_t *decoded = jpeg_alloc_decoder_mem(LUMINARY_DECODE_BYTES, &output_config, &decoded_capacity);
    ESP_ERROR_CHECK(input && decoded && decoded_capacity >= LUMINARY_DECODE_BYTES ? ESP_OK : ESP_ERR_NO_MEM);
    if (input_capacity < minimum_input_capacity) {
        ESP_LOGW(TAG, "JPEG input capacity %zu is below %zu; wave-cycle decode may fall back",
                 input_capacity, minimum_input_capacity);
    }

    jpeg_decoder_handle_t decoder = NULL;
    const jpeg_decode_engine_cfg_t engine_config = {.timeout_ms = 60};
    const jpeg_decode_cfg_t decode_config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        // The EK79007's RGB888 scanout consumes B,G,R byte order from the
        // P4 framebuffer even though the panel's logical element order is
        // RGB.  Supplying decoder RGB here makes blue sky render sepia/red.
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&engine_config, &decoder));

    ESP_ERROR_CHECK(asset_size <= input_capacity ? ESP_OK : ESP_ERR_INVALID_SIZE);
    memcpy(input, asset, asset_size);
    jpeg_decode_picture_info_t info = {0};
    ESP_ERROR_CHECK(jpeg_decoder_get_info(input, asset_size, &info));
    ESP_ERROR_CHECK(info.width == LUMINARY_WIDTH && info.height == LUMINARY_HEIGHT ? ESP_OK : ESP_ERR_INVALID_SIZE);
    uint32_t decoded_size = 0;
    ESP_ERROR_CHECK(jpeg_decoder_process(decoder, &decode_config, input, asset_size,
                                         decoded, decoded_capacity, &decoded_size));
    ESP_ERROR_CHECK(decoded_size >= LUMINARY_FRAME_BYTES ? ESP_OK : ESP_ERR_INVALID_SIZE);
    uint8_t *base_decoded = heap_caps_malloc(LUMINARY_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!base_decoded) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    memcpy(base_decoded, decoded, LUMINARY_FRAME_BYTES);
    initialize_wave_lut();
    if (wifi_events && CONFIG_LUMINARY_RUNTIME_BASE_URL[0] != '\0') {
        ESP_ERROR_CHECK(xTaskCreate(runtime_pull_task, "runtime-pull", 16384, NULL, 3, NULL) == pdPASS ?
                        ESP_OK : ESP_ERR_NO_MEM);
        ESP_LOGI(TAG, "Autonomous runtime polling: %s every %u seconds",
                 CONFIG_LUMINARY_RUNTIME_BASE_URL, CONFIG_LUMINARY_RUNTIME_POLL_SECONDS);
    }

#if CONFIG_LUMINARY_OCEAN_SIM
    if (ocean_sim_start()) {
        xSemaphoreTake(runtime_lock, portMAX_DELAY);
        ocean_sim_apply_conditions_locked();
        xSemaphoreGive(runtime_lock);
        // CPU1 runs this render loop, so the solver gets CPU0.
        ESP_ERROR_CHECK(xTaskCreatePinnedToCore(ocean_sim_task, "ocean-sim", 4096,
                                                NULL, 4, NULL, 0) == pdPASS ?
                        ESP_OK : ESP_ERR_NO_MEM);
        ESP_LOGI(TAG, "Ocean solver: %ux%u cells at %u Hz on CPU0, %u KB in PSRAM",
                 OCEAN_NX, OCEAN_NY, CONFIG_LUMINARY_OCEAN_SIM_HZ,
                 (unsigned)((OCEAN_CELLS * 6u) / 1024u));
    }
#endif
    TickType_t deadline = xTaskGetTickCount();
    uint8_t target = 0;
    uint32_t rendered_frames = 0;
    const int64_t started_us = esp_timer_get_time();
    time_t last_solar_update = 0;

    ESP_LOGI(TAG, "Runtime renderer: continuous %u ms swell from %u deg, height %u mm; cloud cover %u/1000",
             LUMINARY_WAVE_PERIOD_MS, LUMINARY_WAVE_FROM_DEG, LUMINARY_WAVE_HEIGHT_MM,
             LUMINARY_CLOUD_COVER_PERMILLE);
    while (true) {
        const uint64_t elapsed_ms = (uint64_t)(esp_timer_get_time() - started_us) / 1000ULL;
        const int64_t render_started_us = esp_timer_get_time();
        const time_t wall_clock = time(NULL);
        const uint8_t *cycle_base = base_decoded;
        bool use_wave_cycle = false;
        size_t cycle_bytes = 0U;
        xSemaphoreTake(runtime_lock, portMAX_DELAY);
        if (wall_clock - last_solar_update >= 30 && update_solar_position_from_clock(wall_clock)) {
            last_solar_update = wall_clock;
            ESP_LOGI(TAG, "Local solar position: alt=%.1f deg rel-az=%.1f deg; %u Hipparcos stars in view",
                     runtime_state.sun_altitude_deci_deg / 10.0,
                     runtime_state.sun_relative_azimuth_deci_deg / 10.0,
                     visible_star_count);
        }
        bool want_wave_cycle = runtime_wave_cycle.payload &&
                               runtime_wave_cycle.frame_count > 0U &&
                               runtime_wave_cycle.fps_milli > 0U;
#if CONFIG_LUMINARY_OCEAN_SIM
        // The baked wave-cycle loop and the live solver are the same idea at
        // different eras: the cycle plays authored JPEG frames of moving
        // water, the solver moves the water itself. Rendering both would
        // double-animate the sea -- refraction would read a base that is
        // already waving -- and the cycle path renders through the old
        // per-pixel branch at ~330 ms/frame besides. Once the solver is
        // running it wins, and the cycle's decode cost is skipped too.
        if (ocean_ready) want_wave_cycle = false;
#endif
        if (want_wave_cycle) {
            const uint32_t frame_index =
                (uint32_t)(elapsed_ms * (uint64_t)runtime_wave_cycle.fps_milli / 1000ULL %
                           runtime_wave_cycle.frame_count);
            const lumv_frame_t *frame = runtime_wave_cycle.frames + frame_index;
            if (frame->length > 0U && frame->length <= input_capacity) {
                memcpy(input, runtime_wave_cycle.payload + frame->offset, frame->length);
                cycle_bytes = frame->length;
                use_wave_cycle = true;
            }
        }
        const bool cloudy = runtime_state.cloud_cover_permille > 0U;
        if (use_wave_cycle) {
            uint32_t decoded_size = 0U;
            if (jpeg_decoder_process(decoder, &decode_config, input, cycle_bytes,
                                    decoded, decoded_capacity, &decoded_size) != ESP_OK ||
                decoded_size < LUMINARY_FRAME_BYTES) {
                use_wave_cycle = false;
                ESP_LOGW(TAG, "wave cycle frame decode failed; falling back to static base");
            } else {
                cycle_base = decoded;
            }
        }
        xSemaphoreGive(runtime_lock);
        // Compose in cacheable PSRAM. The DPI scanout buffers are optimized
        // for DMA rather than random CPU writes, so touch each only once with
        // a contiguous transfer after the frame is complete.
        // Compose straight into the inactive scanout buffer. The old path
        // composed into render_buffer and then copied 1.84 MB across, costing
        // ~46 ms/frame purely to move bytes between two PSRAM regions.
        // A framebuffer's rock pixels below the horizon are valid from the
        // moment it has rendered one frame of the static base; a wave-cycle
        // frame overwrites them with that cycle frame, invalidating it.
        static bool fb_water_rows_hold_base[2] = {false, false};
        render_runtime_frame(framebuffer[target], cycle_base, elapsed_ms, use_wave_cycle,
                             fb_water_rows_hold_base[target]);
        fb_water_rows_hold_base[target] = !use_wave_cycle;
        xSemaphoreTake(runtime_lock, portMAX_DELAY);
        PROF_STAMP(prof_fb0);
        PROF_SET(prof_fbcopy_us, prof_fb0);
        current_framebuffer = framebuffer[target];
        xSemaphoreGive(runtime_lock);
        const int64_t render_us = esp_timer_get_time() - render_started_us;
        // Passing the target frame buffer requests an atomic driver-side buffer swap;
        // no display copy occurs and decoding continues into the other buffer next frame.
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, LUMINARY_WIDTH, LUMINARY_HEIGHT, framebuffer[target]));

        target ^= 1U;
        ++rendered_frames;
        if (rendered_frames <= 3U || (rendered_frames % 30U) == 0U) {
            ESP_LOGI(TAG, "runtime cadence: render=%lld us, uptime=%llu ms",
                     (long long)render_us, (unsigned long long)elapsed_ms);
#if CONFIG_LUMINARY_RENDER_PROFILING
            ESP_LOGI(TAG, "  phase us: basecopy=%lld lut=%lld wave=%lld sky=%lld "
                          "water=%lld stars=%lld fbcopy=%lld cloud=%u/1000",
                     (long long)prof_basecopy_us, (long long)prof_lut_us,
                     (long long)prof_wave_us, (long long)prof_sky_us,
                     (long long)prof_water_us, (long long)prof_stars_us,
                     (long long)prof_fbcopy_us, runtime_state.cloud_cover_permille);
#endif
        }
        // Clear ocean motion sustains 6 fps. Three independently projected
        // satellite shells measure ~284 ms/frame, so cloudy scenes use 3 fps
        // rather than missing a 6 fps deadline. A long live upload cannot
        // leave the task in a permanent catch-up loop.
        const TickType_t frame_period = pdMS_TO_TICKS(1000U / (cloudy ? 3U : 6U));
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - deadline) > (int32_t)frame_period) deadline = now;
        vTaskDelayUntil(&deadline, frame_period);
    }
}
