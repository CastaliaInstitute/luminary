package institute.castalia.luminary

import android.content.res.AssetManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.Log
import android.view.Surface
import org.json.JSONArray
import org.json.JSONObject

/** Kotlin face of the native core: asset loading, scene state, frame loop. */
class LuminaryCore(private val assets: AssetManager) {

    external fun nativeInit(
        baseRgb: ByteArray, waterMask: ByteArray, landMask: ByteArray,
        shore: ByteArray, map: ByteArray, depth: ByteArray,
        cloudLow: ByteArray, cloudMid: ByteArray, cloudHigh: ByteArray,
    ): Boolean

    external fun nativeSetConditions(
        skyR: Int, skyG: Int, skyB: Int, sunMode: Int,
        sunAltDeci: Int, sunAzDeci: Int, cloudPermille: Int,
        waveFields: IntArray, shellFields: IntArray,
    )

    external fun nativeSolverTick()
    external fun nativeSetSurface(surface: Surface?)
    external fun nativeRenderFrame(elapsedMs: Long): Boolean

    /** Live cloud atlas hot-swap. `nativeCloudAtlasBytes` is this build's
     * expected per-shell size; a fetched atlas of a different size is ignored
     * natively so a stale bundle never corrupts the sky. */
    external fun nativeSetClouds(low: ByteArray, mid: ByteArray, high: ByteArray)
    external fun nativeCloudAtlasBytes(): Long

    val cloudAtlasBytes: Long get() = nativeCloudAtlasBytes()
    fun setClouds(low: ByteArray, mid: ByteArray, high: ByteArray) =
        nativeSetClouds(low, mid, high)

    /** The scene currently driving conditions -- the bundled fallback until a
     * live bundle is fetched. Waves (buoy) and clouds (GOES) come from here;
     * the sun is always computed on-device from the clock. */
    @Volatile var activeScene: JSONObject = JSONObject()

    fun initialize(): Boolean {
        val ok = nativeInit(
            decodeBaseRgb(),
            read("nubble_runtime_water_mask.bin"),
            read("nubble_runtime_land_mask.bin"),
            read("nubble_runtime_shore_distance.bin"),
            read("nubble_runtime_ocean_map.bin"),
            read("nubble_runtime_ocean_depth.bin"),
            read("nubble_runtime_cloud_low.bin"),
            read("nubble_runtime_cloud_mid.bin"),
            read("nubble_runtime_cloud_high.bin"),
        )
        if (ok) {
            activeScene = JSONObject(String(read("nubble-runtime-scene-v1.json")))
            applyScene(activeScene)
        }
        return ok
    }

    /** Same scene schema the P4 consumes; bundled copy is the offline
     * fallback, and the caller may re-apply a freshly fetched one. */
    fun applyScene(scene: JSONObject) {
        activeScene = scene
        val sky = scene.optJSONObject("sky") ?: JSONObject()
        val legacySky = scene.optJSONObject("sky_color") ?: JSONObject()
        val palette = sky.optJSONArray("palette_rgb")
        val skyR = palette.channel(0, legacySky.optInt("r", 168))
        val skyG = palette.channel(1, legacySky.optInt("g", 208))
        val skyB = palette.channel(2, legacySky.optInt("b", 228))
        // Runtime v1 nests cloud cover and shells under `sky`; older bundles
        // used a top-level `clouds` object. Accept both without silently
        // dropping live GOES conditions.
        val clouds = scene.optJSONObject("clouds") ?: sky
        val ocean = scene.optJSONObject("ocean") ?: JSONObject()

        val waves = ArrayList<Int>()
        val components = ocean.optJSONArray("components")
        if (components != null) {
            for (i in 0 until minOf(components.length(), 3)) {
                val component = components.getJSONObject(i)
                waves.add((component.optDouble("height_m", 0.5) * 1000).toInt())
                waves.add((component.optDouble("period_s", 7.0) * 1000).toInt())
                waves.add(
                    component.optDouble(
                        "wave_from_deg",
                        ocean.optDouble("wave_from_deg", 137.0),
                    ).toInt(),
                )
            }
        }
        if (waves.isEmpty()) waves.addAll(listOf(500, 7000, 137))

        // Shell winds: the state JSON carries measured winds per shell where
        // available; height/bias defaults mirror the firmware's.
        val shellDefaults = listOf(
            Triple("high", 10000, 14),
            Triple("mid", 5000, 9),
            Triple("low", 2000, 4),
        )
        val shells = ArrayList<Int>()
        val shellArray = clouds.optJSONArray("shells")
        val shellsByName = (0 until (shellArray?.length() ?: 0))
            .mapNotNull { shellArray?.optJSONObject(it) }
            .associateBy { it.optString("name").lowercase() }
        for ((name, defaultHeight, blueBias) in shellDefaults) {
            val shell = shellsByName[name]
            val advection = shell?.optJSONObject("advection")
            val east = shell?.optDouble(
                "wind_east_mps",
                advection?.optDouble("east_mps", 4.0) ?: 4.0,
            ) ?: 4.0
            val north = shell?.optDouble(
                "wind_north_mps",
                advection?.optDouble("north_mps", 2.0) ?: 2.0,
            ) ?: 2.0
            val height = shell?.optInt(
                "height_m",
                shell.optInt("projection_height_m", defaultHeight),
            ) ?: defaultHeight
            shells.add((east * 1000).toInt())
            shells.add((north * 1000).toInt())
            shells.add(height)
            shells.add(blueBias)
        }

        // Sun is computed from the device clock for York, so the sky tracks
        // the real day continuously and offline -- the scene JSON's sun field
        // is a stale snapshot and is deliberately ignored.
        val solar = SolarPosition.current()
        val altitudeDeci = (solar.altitudeDeg * 10).toInt()
        val sunMode = when {
            altitudeDeci >= 0 -> 0        // day
            altitudeDeci >= -60 -> 1      // civil twilight
            altitudeDeci >= -120 -> 2     // nautical
            else -> 3                     // night
        }
        // Bearing from shore is due east (90 deg); relative azimuth drives the
        // side-weighted golden-hour glow.
        nativeSetConditions(
            skyR, skyG, skyB,
            sunMode, altitudeDeci,
            ((solar.azimuthDeg - 90.0) * 10).toInt(),
            (clouds.optDouble(
                "observed_cloud_fraction",
                clouds.optDouble(
                    "cover_fraction",
                    scene.optDouble("cloud_cover", 0.0),
                ),
            ).coerceIn(0.0, 1.0) * 1000).toInt(),
            waves.toIntArray(), shells.toIntArray(),
        )
        Log.i(
            TAG,
            "conditions: sky=$skyR,$skyG,$skyB " +
                "cloud=${clouds.optDouble("observed_cloud_fraction", clouds.optDouble("cover_fraction", 0.0))} " +
                "waves=${waves.size / 3} shells=${shellsByName.keys.sorted()}",
        )
    }

    private fun JSONArray?.channel(index: Int, fallback: Int): Int =
        this?.optInt(index, fallback)?.coerceIn(0, 255) ?: fallback

    private fun read(name: String): ByteArray =
        assets.open(name).use { it.readBytes() }

    private fun decodeBaseRgb(): ByteArray {
        val options = BitmapFactory.Options().apply {
            inPreferredConfig = Bitmap.Config.ARGB_8888
            inScaled = false
        }
        val bitmap = assets.open("nubble_runtime_base.jpg").use {
            BitmapFactory.decodeStream(it, null, options)
        } ?: error("base frame did not decode")
        val pixels = IntArray(bitmap.width * bitmap.height)
        bitmap.getPixels(pixels, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
        bitmap.recycle()
        val rgb = ByteArray(pixels.size * 3)
        for (i in pixels.indices) {
            val p = pixels[i]
            rgb[i * 3] = ((p shr 16) and 0xFF).toByte()
            rgb[i * 3 + 1] = ((p shr 8) and 0xFF).toByte()
            rgb[i * 3 + 2] = (p and 0xFF).toByte()
        }
        return rgb
    }

    companion object {
        private const val TAG = "luminary"

        init {
            System.loadLibrary("luminary_core")
        }
    }
}
