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

#include "driver/jpeg_decode.h"
#include "cJSON.h"
#include "esp_check.h"
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
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "luminary_runtime_state.h"

#define LUMINARY_WIDTH                  1024U
#define LUMINARY_HEIGHT                  600U
#define LUMINARY_BPP                        3U
#define LUMINARY_FRAME_BYTES (LUMINARY_WIDTH * LUMINARY_HEIGHT * LUMINARY_BPP)
#define LUMINARY_DECODE_HEIGHT             608U /* JPEG DMA output is 16-line aligned. */
#define LUMINARY_DECODE_BYTES (LUMINARY_WIDTH * LUMINARY_DECODE_HEIGHT * LUMINARY_BPP)

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

extern const uint8_t nubble_runtime_base_jpg_start[] asm("_binary_nubble_runtime_base_jpg_start");
extern const uint8_t nubble_runtime_base_jpg_end[] asm("_binary_nubble_runtime_base_jpg_end");
extern const uint8_t nubble_runtime_water_mask_bin_start[] asm("_binary_nubble_runtime_water_mask_bin_start");
extern const uint8_t nubble_runtime_shore_distance_bin_start[] asm("_binary_nubble_runtime_shore_distance_bin_start");
extern const uint8_t nubble_runtime_ocean_phase_bin_start[] asm("_binary_nubble_runtime_ocean_phase_bin_start");
extern const uint8_t nubble_runtime_cloud_low_bin_start[] asm("_binary_nubble_runtime_cloud_low_bin_start");
extern const uint8_t nubble_runtime_cloud_mid_bin_start[] asm("_binary_nubble_runtime_cloud_mid_bin_start");
extern const uint8_t nubble_runtime_cloud_high_bin_start[] asm("_binary_nubble_runtime_cloud_high_bin_start");

static const char *TAG = "luminary-anim";
static int8_t wave_sine[256];
static uint8_t cloud_x_lut[LUMINARY_WIDTH];
static uint8_t cloud_y_lut[LUMINARY_RUNTIME_HORIZON];

typedef struct {
    uint8_t *atlas; /* Interleaved luminance, alpha. */
    int32_t wind_east_mmps;
    int32_t wind_north_mmps;
    uint32_t height_m;
    uint8_t blue_bias;
} cloud_shell_t;

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
    uint8_t *ocean_phase;
    cloud_shell_t shells[3]; /* high, mid, low */
} runtime_state_t;

static runtime_state_t runtime_state;
static SemaphoreHandle_t runtime_lock;

#define CLOUD_ATLAS_BYTES (LUMINARY_CLOUD_TEXTURE_WIDTH * LUMINARY_CLOUD_TEXTURE_HEIGHT * 2U)
#define OCEAN_PHASE_BYTES (LUMINARY_WIDTH * LUMINARY_HEIGHT)
#define RUNTIME_MANIFEST_MAX_BYTES 8192U
#define RUNTIME_STATE_MAX_BYTES    8192U
#define RUNTIME_URL_MAX_BYTES       512U
#define RUNTIME_PATH_MAX_BYTES      160U
#define YORK_LATITUDE_DEG        43.1637
#define YORK_LONGITUDE_DEG      -70.6480
#define NUBBLE_CAMERA_BEARING_DEG    90.0

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1

static EventGroupHandle_t wifi_events;
static unsigned wifi_retry_count;
static char active_bundle_id[48];

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
    return true;
}

static void initialize_wave_lut(void)
{
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
        .shells = {
            {NULL, LUMINARY_HIGH_WIND_EAST_MMPS, LUMINARY_HIGH_WIND_NORTH_MMPS,
             LUMINARY_HIGH_CLOUD_HEIGHT_M, 14U},
            {NULL, LUMINARY_MID_WIND_EAST_MMPS, LUMINARY_MID_WIND_NORTH_MMPS,
             LUMINARY_MID_CLOUD_HEIGHT_M, 9U},
            {NULL, LUMINARY_LOW_WIND_EAST_MMPS, LUMINARY_LOW_WIND_NORTH_MMPS,
             LUMINARY_LOW_CLOUD_HEIGHT_M, 4U},
        },
    };
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
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "ocean phase must be 614400 bytes");
    }
    xSemaphoreTake(runtime_lock, portMAX_DELAY);
    memcpy(runtime_state.ocean_phase, incoming, OCEAN_PHASE_BYTES);
    xSemaphoreGive(runtime_lock);
    free(incoming);
    ESP_LOGI(TAG, "Perspective ocean phase map updated");
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

static void start_runtime_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t state_uri = {.uri = "/runtime/state", .method = HTTP_POST,
                                   .handler = state_upload_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &state_uri));
    const char *uris[] = {"/runtime/cloud/high", "/runtime/cloud/mid", "/runtime/cloud/low"};
    for (unsigned index = 0; index < 3; ++index) {
        const httpd_uri_t uri = {.uri = uris[index], .method = HTTP_POST,
                                 .handler = cloud_upload_handler, .user_ctx = (void *)(uintptr_t)index};
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
    }
    const httpd_uri_t ocean_uri = {.uri = "/runtime/ocean-phase", .method = HTTP_POST,
                                    .handler = ocean_phase_upload_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ocean_uri));
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
    if (!parse_remote_asset(assets, "cloud_high", CLOUD_ATLAS_BYTES, &descriptor[0]) ||
        !parse_remote_asset(assets, "cloud_mid", CLOUD_ATLAS_BYTES, &descriptor[1]) ||
        !parse_remote_asset(assets, "cloud_low", CLOUD_ATLAS_BYTES, &descriptor[2]) ||
        !parse_remote_asset(assets, "ocean_phase", OCEAN_PHASE_BYTES, &descriptor[3]) ||
        !parse_remote_asset(assets, "state", 0, &descriptor[4])) {
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

    /* One lock protects the complete bundle swap; rendering sees old or new. */
    xSemaphoreTake(runtime_lock, portMAX_DELAY);
    for (unsigned index = 0; index < 3; ++index) {
        memcpy(runtime_state.shells[index].atlas, cloud[index], CLOUD_ATLAS_BYTES);
    }
    memcpy(runtime_state.ocean_phase, ocean, OCEAN_PHASE_BYTES);
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
    return (nubble_runtime_water_mask_bin_start[pixel >> 3] >> (pixel & 7U)) & 1U;
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
        bgr[0] = (uint8_t)(bgr[0] * 56U / 100U);
        bgr[1] = (uint8_t)(bgr[1] * 43U / 100U);
        bgr[2] = (uint8_t)(bgr[2] * 34U / 100U);
    } else if (runtime_state.sun_mode == 3U) {
        bgr[0] = (uint8_t)(bgr[0] * 42U / 100U);
        bgr[1] = (uint8_t)(bgr[1] * 28U / 100U);
        bgr[2] = (uint8_t)(bgr[2] * 20U / 100U);
        const uint32_t hash = (x * 73856093U) ^ (y * 19349663U);
        if ((hash & 0x1fffU) == 0U && y + 44U < LUMINARY_RUNTIME_HORIZON) {
            bgr[0] = 235U; bgr[1] = 220U; bgr[2] = 210U;
        }
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

static void render_runtime_frame(uint8_t *destination, const uint8_t *base, uint64_t elapsed_ms)
{
    const unsigned cloud_cover_permille = runtime_state.cloud_cover_permille;
    const uint32_t wave_period_ms = runtime_state.wave_period_ms > 500U ? runtime_state.wave_period_ms : 500U;
    const uint32_t phase = (uint32_t)((elapsed_ms * 256ULL / wave_period_ms) & 255ULL);
    const int amplitude = 2 + (int)(runtime_state.wave_height_mm / 450U);
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
                memcpy(destination + target, base + target, LUMINARY_BPP);
                if (y < LUMINARY_RUNTIME_HORIZON && cloud_cover_permille > 0U) {
                    for (unsigned shell = 0; shell < 3; ++shell) {
                        composite_cloud_shell(destination + target, &runtime_state.shells[shell], x, y,
                                              cloud_shift_x[shell], cloud_shift_y[shell],
                                              cloud_cover_permille);
                    }
                }
                if (y < LUMINARY_RUNTIME_HORIZON) {
                    // Grade the full atmosphere after cloud compositing so
                    // real clouds catch the same golden-hour illumination.
                    grade_sky_pixel(destination + target, x, y);
                }
                continue;
            }
            const unsigned projected = runtime_state.ocean_phase[pixel];
            // phase(x,t) = k·x - omega*t: crests travel in the measured
            // wave-to direction on the perspective-projected sea plane.
            const uint8_t p1 = (uint8_t)((int)projected - (int)phase);
            const uint8_t p2 = (uint8_t)(((int)projected * 3) / 2 -
                                         (int)(phase * 2U) + 47);
            int shift = (wave_sine[p1] * amplitude + wave_sine[p2] * (amplitude / 2 + 1)) / 127;
            int source_x = (int)x + shift;
            if (source_x < 0) source_x = 0;
            if (source_x >= (int)LUMINARY_WIDTH) source_x = LUMINARY_WIDTH - 1;
            const size_t source = ((size_t)y * LUMINARY_WIDTH + (unsigned)source_x) * LUMINARY_BPP;
            memcpy(destination + target, base + source, LUMINARY_BPP);

            // Shore-distance is 0 at rock and grows into open water. A crest
            // can brighten only the first ~12 px of registered water; no
            // screen-wide white bands and no foam over physical land.
            const uint8_t shore = nubble_runtime_shore_distance_bin_start[pixel];
            const int crest = wave_sine[p1];
            if (shore < 32U && crest > 72) {
                const unsigned foam = (unsigned)((crest - 72) * (32U - shore)) / 60U;
                for (unsigned channel = 0; channel < 3; ++channel) {
                    unsigned value = destination[target + channel];
                    destination[target + channel] = (uint8_t)(value + ((255U - value) * foam >> 8));
                }
            }
            grade_water_pixel(destination + target, y);
        }
    }
}

void app_main(void)
{
    initialize_runtime_state();
    start_wifi();
    start_runtime_server();

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

    jpeg_decode_memory_alloc_cfg_t input_config = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    jpeg_decode_memory_alloc_cfg_t output_config = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t input_capacity = 0;
    size_t decoded_capacity = 0;
    uint8_t *input = jpeg_alloc_decoder_mem(asset_size + 4096, &input_config, &input_capacity);
    uint8_t *decoded = jpeg_alloc_decoder_mem(LUMINARY_DECODE_BYTES, &output_config, &decoded_capacity);
    ESP_ERROR_CHECK(input && decoded && decoded_capacity >= LUMINARY_DECODE_BYTES ? ESP_OK : ESP_ERR_NO_MEM);

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
    initialize_wave_lut();
    if (wifi_events && CONFIG_LUMINARY_RUNTIME_BASE_URL[0] != '\0') {
        ESP_ERROR_CHECK(xTaskCreate(runtime_pull_task, "runtime-pull", 16384, NULL, 3, NULL) == pdPASS ?
                        ESP_OK : ESP_ERR_NO_MEM);
        ESP_LOGI(TAG, "Autonomous runtime polling: %s every %u seconds",
                 CONFIG_LUMINARY_RUNTIME_BASE_URL, CONFIG_LUMINARY_RUNTIME_POLL_SECONDS);
    }

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
        xSemaphoreTake(runtime_lock, portMAX_DELAY);
        if (wall_clock - last_solar_update >= 30 && update_solar_position_from_clock(wall_clock)) {
            last_solar_update = wall_clock;
            ESP_LOGI(TAG, "Local solar position: alt=%.1f deg rel-az=%.1f deg",
                     runtime_state.sun_altitude_deci_deg / 10.0,
                     runtime_state.sun_relative_azimuth_deci_deg / 10.0);
        }
        const bool cloudy = runtime_state.cloud_cover_permille > 0U;
        render_runtime_frame(framebuffer[target], decoded, elapsed_ms);
        xSemaphoreGive(runtime_lock);
        const int64_t render_us = esp_timer_get_time() - render_started_us;
        // Passing the target frame buffer requests an atomic driver-side buffer swap;
        // no display copy occurs and decoding continues into the other buffer next frame.
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, LUMINARY_WIDTH, LUMINARY_HEIGHT, framebuffer[target]));

        target ^= 1U;
        if ((++rendered_frames % 60U) == 0U) {
            ESP_LOGI(TAG, "runtime cadence: render=%lld us, uptime=%llu ms",
                     (long long)render_us, (unsigned long long)elapsed_ms);
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
