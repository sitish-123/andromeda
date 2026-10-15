package org.umbriel.link.clipboard

import android.content.ClipboardManager
import android.content.Context
import android.graphics.PixelFormat
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * Reads the clipboard from the background: a one-pixel overlay window takes focus, the one state in which Android lets
 * an app read it, and goes as soon as it has the clip. Android 15 and later refuse [ClipboardReadActivity] from the
 * background even with "Display over other apps", which an overlay window needs and nothing more. The URI grant a read
 * gives lasts while the clip is the primary one, so the snapshot runs after the window is gone.
 */
class ClipboardOverlay(private val context: Context, private val sync: ClipboardSync, private val scope: CoroutineScope) {
    private val windows = context.getSystemService(WindowManager::class.java)
    private val clipboard = context.getSystemService(ClipboardManager::class.java)
    private val main = Handler(Looper.getMainLooper())
    private var view: View? = null

    fun read() {
        main.post(::show)
    }

    private fun show() {
        if (view != null) return
        val window = object : View(context) {
            override fun onWindowFocusChanged(hasWindowFocus: Boolean) {
                super.onWindowFocusChanged(hasWindowFocus)
                if (hasWindowFocus) taken(this)
            }
        }
        val params = WindowManager.LayoutParams(
            1,
            1,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
            PixelFormat.TRANSLUCENT,
        ).apply { gravity = Gravity.TOP or Gravity.START }
        try {
            windows.addView(window, params)
        } catch (error: RuntimeException) {
            Log.w("link", "Clipboard overlay: ${error.message}")
            return
        }
        view = window
        main.postDelayed({ remove(window) }, FOCUS_TIMEOUT_MS)
    }

    private fun taken(window: View) {
        if (view !== window) return
        view = null
        val clip = clipboard.primaryClip
        // Removing a window during its own focus dispatch crashes ViewRootImpl.
        main.post { windows.removeView(window) }
        if (clip == null || clip.itemCount == 0) return
        scope.launch { sync.offer(clip).onFailure { Log.w("link", "Sending clipboard: ${it.message}") } }
    }

    private fun remove(window: View) {
        if (view !== window) return
        view = null
        windows.removeView(window)
    }

    private companion object {
        const val FOCUS_TIMEOUT_MS = 2_000L
    }
}
