package institute.castalia.luminary

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import kotlin.concurrent.thread

/**
 * Full-screen ambient viewer for a wall-mounted phone behind a 4x6 frame
 * matted to a 3x5 visible window. The scene renders at its native 1024x600
 * into a viewport sized screen-height x (height * 1024/600), centred; the
 * mat crops the ~2% aspect difference and everything outside is black.
 */
class MainActivity : Activity() {
    private lateinit var core: LuminaryCore
    @Volatile private var running = false      // solver/render loops; gated on the surface
    @Volatile private var alive = false        // conditions loop; gated on the activity

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Wall-display duty: stay on, come up over the keyguard, and turn
        // the screen on when launched.
        window.addFlags(
            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON or
                WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED or
                WindowManager.LayoutParams.FLAG_DISMISS_KEYGUARD or
                WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON,
        )

        core = LuminaryCore(assets)
        if (!core.initialize()) {
            Log.e(TAG, "core init failed")
            finish()
            return
        }

        val surface = SurfaceView(this)
        // Composite the surface layer above the window rather than through a
        // punched hole: on the API 21 test device the hole-punch path left
        // the layer hidden at alpha 0 under the opaque black window. Nothing
        // ever draws over the scene, so on-top costs nothing.
        surface.setZOrderOnTop(true)
        surface.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                core.nativeSetSurface(holder.surface)
                startLoops()
            }

            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) {}

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                running = false
                core.nativeSetSurface(null)
            }
        })

        // Landscape is locked, so the smaller display dimension is the
        // height. Fixed viewport at the scene's aspect, centred; the mat
        // crops the ~2% overshoot past the 3x5 window and the surround is
        // black regardless.
        val metrics = resources.displayMetrics
        val viewportHeight = minOf(metrics.widthPixels, metrics.heightPixels)
        val viewportWidth = viewportHeight * 2048 / 1200
        val root = FrameLayout(this)
        root.setBackgroundColor(Color.BLACK)
        // Top-aligned: the frame design seats the phone against the top of
        // the window and a 3D-printed foreground relief occupies the bottom,
        // exactly as the full-size build layers its printed rocks over the
        // panel. The print's top edge is the registration line; everything
        // below whatever it covers is physical.
        root.addView(
            surface,
            FrameLayout.LayoutParams(
                viewportWidth, viewportHeight,
                Gravity.TOP or Gravity.CENTER_HORIZONTAL,
            ),
        )
        setContentView(root)
        hideSystemUi()
        startConditionsLoop()
    }

    private fun startLoops() {
        if (running) return
        running = true
        val started = SystemClock.elapsedRealtime()

        thread(name = "luminary-solver") {
            // 30 Hz physics; the solver tick length is fixed at prepare time.
            while (running) {
                val t0 = SystemClock.elapsedRealtime()
                core.nativeSolverTick()
                val sleep = 33 - (SystemClock.elapsedRealtime() - t0)
                if (sleep > 0) SystemClock.sleep(sleep)
            }
        }

        thread(name = "luminary-render") {
            // 30 fps target; the A16 renders this frame in a few ms, the
            // 2014 test device in ~20.
            while (running) {
                val t0 = SystemClock.elapsedRealtime()
                core.nativeRenderFrame(t0 - started)
                val sleep = 33 - (SystemClock.elapsedRealtime() - t0)
                if (sleep > 0) SystemClock.sleep(sleep)
            }
        }
    }

    /**
     * The one loop that keeps the scene live. Two cadences share it:
     *
     *  - every minute, re-apply the active scene so the on-device solar clock
     *    advances the sky through the real day -- this needs no network and is
     *    what makes the sky genuinely live even offline;
     *  - every poll interval, re-fetch the same runtime bundle the P4 consumes
     *    (NDBC 44098 buoy waves, GOES cloud cover, York tide) so the surf and
     *    clouds track live data. A failed fetch simply leaves the last good
     *    conditions in place.
     */
    private fun startConditionsLoop() {
        alive = true
        thread(name = "luminary-conditions") {
            val pollEveryMs = 5 * 60 * 1000L
            var lastPoll = 0L
            // Gated on the activity, not the render surface: this loop advances
            // the solar sky and polls live conditions even before the surface is
            // created (the old `while (running)` raced surfaceCreated and exited
            // immediately, so neither ran).
            while (alive) {
                val now = System.currentTimeMillis()
                if (now - lastPoll >= pollEveryMs) {
                    lastPoll = now
                    pollLive()
                }
                // Re-apply the active scene every second so the solar clock
                // advances the sky with no visible staleness -- unnecessarily
                // often for a sun that moves a quarter-degree a minute, which
                // is the point.
                runOnUiThread { core.applyScene(core.activeScene) }
                SystemClock.sleep(1_000)
            }
        }
    }

    /** Fetch the live runtime bundle the P4 also consumes: state JSON (buoy
     * waves, cloud cover, tide) AND the three GOES cloud atlases, hot-swapping
     * the clouds so the sky tracks the real satellite -- not just a bundled
     * snapshot. Any failure leaves the last good conditions in place. */
    private fun pollLive() {
        try {
            // runtime-live is force-updated with each measured bundle. Raw
            // GitHub may cache that mutable branch URL long after the ref has
            // moved, so resolve the current commit first and fetch the entire
            // bundle through its immutable SHA.
            val branch = JSONObject(fetchText(RUNTIME_BRANCH_API))
            val runtimeSha = branch.getJSONObject("commit").getString("sha")
            val runtimeRoot = "$RAW_ROOT/$runtimeSha/site/runtime/v1"
            val manifest = JSONObject(fetchText("$runtimeRoot/manifest.json"))
            assetPath(manifest, "state")?.let { name ->
                val scene = JSONObject(fetchText("$runtimeRoot/$name"))
                runOnUiThread { core.applyScene(scene) }
                Log.i(
                    TAG,
                    "live conditions applied " +
                        "bundle=${manifest.optString("bundle_id")} " +
                        "sha=${runtimeSha.take(12)}",
                )
            }
            val low = assetPath(manifest, "cloud_low")
            val mid = assetPath(manifest, "cloud_mid")
            val high = assetPath(manifest, "cloud_high")
            if (low != null && mid != null && high != null) {
                val lo = fetchBytes("$runtimeRoot/$low")
                val md = fetchBytes("$runtimeRoot/$mid")
                val hi = fetchBytes("$runtimeRoot/$high")
                val need = core.cloudAtlasBytes
                if (lo.size.toLong() == need && md.size.toLong() == need &&
                    hi.size.toLong() == need) {
                    core.setClouds(lo, md, hi)          // thread-safe; locks natively
                    Log.i(TAG, "live GOES cloud atlas applied (${lo.size} B/shell)")
                } else {
                    Log.w(TAG, "live cloud atlas is ${lo.size} B/shell, this build " +
                        "expects $need; keeping bundled atlas")
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "live fetch failed; keeping current conditions", e)
        }
    }

    /** Manifest asset path, tolerating either the "path" or "file" key. */
    private fun assetPath(manifest: JSONObject, key: String): String? =
        manifest.optJSONObject("assets")?.optJSONObject(key)?.let {
            it.optString("path", it.optString("file", ""))
        }?.takeIf { it.isNotEmpty() }

    private fun fetchText(url: String): String =
        (URL(url).openConnection() as HttpURLConnection)
            .apply {
                connectTimeout = 10000
                readTimeout = 10000
                setRequestProperty("User-Agent", "Luminary-Android/1.0")
                setRequestProperty("Cache-Control", "no-cache")
            }
            .inputStream.bufferedReader().use { it.readText() }

    private fun fetchBytes(url: String): ByteArray =
        (URL(url).openConnection() as HttpURLConnection)
            .apply {
                connectTimeout = 15000
                readTimeout = 30000
                setRequestProperty("User-Agent", "Luminary-Android/1.0")
            }
            .inputStream.use { it.readBytes() }

    private fun hideSystemUi() {
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility =
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
            View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE
    }

    override fun onDestroy() {
        running = false
        alive = false
        super.onDestroy()
    }

    companion object {
        private const val TAG = "luminary"
        private const val RUNTIME_BRANCH_API =
            "https://api.github.com/repos/CastaliaInstitute/luminary/" +
                "branches/runtime-live"
        private const val RAW_ROOT =
            "https://raw.githubusercontent.com/CastaliaInstitute/luminary"
    }
}
