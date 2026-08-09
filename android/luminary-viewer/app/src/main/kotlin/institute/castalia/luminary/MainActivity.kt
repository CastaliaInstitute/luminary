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
    @Volatile private var running = false

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
        val viewportWidth = viewportHeight * 1024 / 600
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
        fetchLiveScene()
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

    /** Same public runtime state the P4 polls; bundled scene is the offline
     * fallback, so a dead network changes nothing but freshness. */
    private fun fetchLiveScene() {
        thread(name = "luminary-scene") {
            try {
                val url = URL(
                    "https://raw.githubusercontent.com/CastaliaInstitute/luminary/" +
                        "runtime-live/site/runtime/v1/manifest.json",
                )
                val manifest = JSONObject(url.openConnection().let {
                    (it as HttpURLConnection).apply { connectTimeout = 10000 }
                        .inputStream.bufferedReader().readText()
                })
                val stateName = manifest.getJSONObject("assets")
                    .getJSONObject("state").getString("file")
                val state = URL(
                    "https://raw.githubusercontent.com/CastaliaInstitute/luminary/" +
                        "runtime-live/site/runtime/v1/$stateName",
                ).readText()
                runOnUiThread { core.applyScene(JSONObject(state)) }
                Log.i(TAG, "live scene applied: $stateName")
            } catch (e: Exception) {
                Log.w(TAG, "live scene fetch failed; bundled scene stays", e)
            }
        }
    }

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
        super.onDestroy()
    }

    companion object {
        private const val TAG = "luminary"
    }
}
