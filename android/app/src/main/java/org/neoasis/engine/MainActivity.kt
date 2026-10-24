package org.neoasis.engine

import com.google.androidgamesdk.GameActivity
import android.os.Bundle
import android.view.View
import android.view.WindowManager

/**
 * Thin host activity.
 *
 * GameActivity owns the SurfaceView + input plumbing and calls into
 * `android_main` in libneoasis.so. Everything game-related lives in C++; this
 * class only configures the window (immersive mode, sustained performance).
 */
class MainActivity : GameActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Immersive, edge-to-edge: the engine renders under the cutout too.
        window.decorView.systemUiVisibility =
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_FULLSCREEN

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        // Ask the platform for the highest sustained clocks. This is advisory;
        // the renderer's dynamic resolution controller is what actually keeps
        // frame times flat on thermally limited hardware.
        @Suppress("DEPRECATION")
        window.attributes = window.attributes.apply {
            preferredDisplayModeId = 0
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            window.decorView.systemUiVisibility =
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                    View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                    View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                    View.SYSTEM_UI_FLAG_FULLSCREEN
        }
    }
}
