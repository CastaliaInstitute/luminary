/* JNI surface for the Luminary core: bind assets, run the solver from a
 * Kotlin thread, render straight into the ANativeWindow. */
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "luminary_render_core.h"

#define TAG "luminary-core"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

/* The render thread writes into the window's buffer; the UI thread swaps the
 * window on surfaceCreated/Destroyed (screen sleep/wake on a wall display).
 * Without this lock the render thread can write into a window the UI thread
 * has just released -- a use-after-free that SIGSEGVs in lum_render_frame. */
static ANativeWindow *window;
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;

/* Asset bytes arrive as Java byte[] copies and are kept for the process
 * lifetime; the core reads them in place. */
static uint8_t *own(JNIEnv *env, jbyteArray array)
{
    if (!array) return NULL;
    const jsize length = (*env)->GetArrayLength(env, array);
    uint8_t *copy = malloc((size_t)length);
    if (copy) (*env)->GetByteArrayRegion(env, array, 0, length, (jbyte *)copy);
    return copy;
}

JNIEXPORT jboolean JNICALL
Java_institute_castalia_luminary_LuminaryCore_nativeInit(
    JNIEnv *env, jobject self, jbyteArray base_rgb, jbyteArray water_mask,
    jbyteArray land_mask, jbyteArray shore, jbyteArray map, jbyteArray depth,
    jbyteArray cloud_low, jbyteArray cloud_mid, jbyteArray cloud_high)
{
    (void)self;
    lum_assets_t assets = {
        .base_rgb = own(env, base_rgb),
        .water_mask = own(env, water_mask),
        .land_mask = own(env, land_mask),
        .shore_distance = own(env, shore),
        .ocean_map = own(env, map),
        .ocean_depth = own(env, depth),
        .cloud_low = own(env, cloud_low),
        .cloud_mid = own(env, cloud_mid),
        .cloud_high = own(env, cloud_high),
    };
    if (!assets.base_rgb || !assets.water_mask || !assets.land_mask ||
        !assets.shore_distance ||
        !assets.ocean_map || !assets.ocean_depth || !assets.cloud_low ||
        !assets.cloud_mid || !assets.cloud_high) {
        return JNI_FALSE;
    }
    const bool ok = lum_init(&assets);
    LOGI("core init %s", ok ? "ok" : "FAILED");
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_institute_castalia_luminary_LuminaryCore_nativeSetConditions(
    JNIEnv *env, jobject self, jint sky_r, jint sky_g, jint sky_b,
    jint sun_mode, jint sun_alt_deci, jint sun_az_deci, jint cloud_permille,
    jintArray wave_fields, jintArray shell_fields)
{
    (void)self;
    lum_conditions_t c;
    memset(&c, 0, sizeof(c));
    c.sky_r = (uint8_t)sky_r;
    c.sky_g = (uint8_t)sky_g;
    c.sky_b = (uint8_t)sky_b;
    c.sun_mode = (uint8_t)sun_mode;
    c.sun_altitude_deci_deg = sun_alt_deci;
    c.sun_relative_azimuth_deci_deg = sun_az_deci;
    c.cloud_cover_permille = (uint16_t)cloud_permille;
    /* wave_fields: triples of (height_mm, period_ms, from_deg), up to 3 */
    jint waves[9] = {0};
    const jsize wave_len = (*env)->GetArrayLength(env, wave_fields);
    (*env)->GetIntArrayRegion(env, wave_fields, 0, wave_len > 9 ? 9 : wave_len, waves);
    c.wave_count = (uint32_t)((wave_len > 9 ? 9 : wave_len) / 3);
    for (unsigned i = 0; i < c.wave_count; ++i) {
        c.waves[i].height_mm = (uint32_t)waves[i * 3];
        c.waves[i].period_ms = (uint32_t)waves[i * 3 + 1];
        c.waves[i].from_deg = waves[i * 3 + 2];
    }
    /* shell_fields: quads of (wind_east_mmps, wind_north_mmps, height_m, blue_bias) x3 */
    jint shells[12] = {0};
    const jsize shell_len = (*env)->GetArrayLength(env, shell_fields);
    (*env)->GetIntArrayRegion(env, shell_fields, 0, shell_len > 12 ? 12 : shell_len, shells);
    for (unsigned s = 0; s < 3u && (jsize)(s * 4 + 3) < shell_len; ++s) {
        c.shells[s].wind_east_mmps = shells[s * 4];
        c.shells[s].wind_north_mmps = shells[s * 4 + 1];
        c.shells[s].height_m = (uint32_t)shells[s * 4 + 2];
        c.shells[s].blue_bias = (uint8_t)shells[s * 4 + 3];
    }
    lum_set_conditions(&c);
}

JNIEXPORT void JNICALL
Java_institute_castalia_luminary_LuminaryCore_nativeSolverTick(JNIEnv *env, jobject self)
{
    (void)env; (void)self;
    lum_solver_tick();
}

JNIEXPORT void JNICALL
Java_institute_castalia_luminary_LuminaryCore_nativeSetSurface(
    JNIEnv *env, jobject self, jobject surface)
{
    (void)self;
    pthread_mutex_lock(&window_lock);
    if (window) {
        ANativeWindow_release(window);
        window = NULL;
    }
    if (surface) {
        window = ANativeWindow_fromSurface(env, surface);
        if (window) {
            ANativeWindow_setBuffersGeometry(window, LUM_WIDTH, LUM_HEIGHT,
                                             WINDOW_FORMAT_RGBX_8888);
        }
    }
    pthread_mutex_unlock(&window_lock);
}

JNIEXPORT jboolean JNICALL
Java_institute_castalia_luminary_LuminaryCore_nativeRenderFrame(
    JNIEnv *env, jobject self, jlong elapsed_ms)
{
    (void)env; (void)self;
    pthread_mutex_lock(&window_lock);
    if (!window) { pthread_mutex_unlock(&window_lock); return JNI_FALSE; }
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window, &buffer, NULL) != 0) {
        pthread_mutex_unlock(&window_lock);
        return JNI_FALSE;
    }
    lum_render_frame((uint32_t *)buffer.bits, buffer.stride, (uint64_t)elapsed_ms);
    ANativeWindow_unlockAndPost(window);
    pthread_mutex_unlock(&window_lock);
    return JNI_TRUE;
}
