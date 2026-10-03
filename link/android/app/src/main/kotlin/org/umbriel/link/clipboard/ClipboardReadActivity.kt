package org.umbriel.link.clipboard

import android.app.Activity
import android.content.ClipboardManager
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.view.WindowManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import org.umbriel.link.LinkApplication

/**
 * A transparent activity that exists only to be focused, the one state in which Android lets an app read the
 * clipboard. It reads, offers the content to connected desktops unless it came from one, and finishes. Also the target of
 * the quick-settings tile and of the text-selection action (which hands it the selected text instead).
 */
class ClipboardReadActivity : Activity() {
    private var selected: String? = null
    private var reading = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Focus is all it needs: a one-pixel corner window leaves the app beneath, its system bars, and touches
        // alone, so the moment it holds focus barely shows.
        window.setLayout(1, 1)
        window.setGravity(Gravity.TOP or Gravity.START)
        window.setDimAmount(0f)
        window.addFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL)
        selected = intent.takeIf { it.action == Intent.ACTION_PROCESS_TEXT }
            ?.getCharSequenceExtra(Intent.EXTRA_PROCESS_TEXT)?.toString()
        if (selected != null) send(selected!!)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus || selected != null || reading || isFinishing) return
        reading = true
        val clip = getSystemService(ClipboardManager::class.java).primaryClip
        if (clip == null || clip.itemCount == 0) {
            finish()
            return
        }
        val container = (application as LinkApplication).container
        CoroutineScope(Dispatchers.Main).launch {
            try {
                container.clipboard.offer(clip).onFailure { Log.w("link", "Sending clipboard: ${it.message}") }
            } finally {
                finish()
            }
        }
    }

    override fun finish() {
        super.finish()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            overrideActivityTransition(OVERRIDE_TRANSITION_CLOSE, 0, 0)
        } else {
            @Suppress("DEPRECATION")
            overridePendingTransition(0, 0)
        }
    }

    private fun send(text: String) {
        val container = (application as LinkApplication).container
        if (!container.clipboard.isEcho(text)) {
            CoroutineScope(Dispatchers.Default).launch {
                container.repository.offerClipText(text).onFailure { Log.w("link", "Sending clipboard: ${it.message}") }
            }
        }
        finish()
    }
}
