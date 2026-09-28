package org.umbriel.link.clipboard

import android.app.Activity
import android.content.ClipboardManager
import android.content.Intent
import android.os.Bundle
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import org.umbriel.link.LinkApplication

/**
 * A transparent activity that exists only to be focused, the one state in which Android lets an app read the
 * clipboard. It reads, offers the text to connected desktops unless it came from one, and finishes. Also the target of
 * the quick-settings tile and of the text-selection action (which hands it the selected text instead).
 */
class ClipboardReadActivity : Activity() {
    private var selected: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        selected = intent.takeIf { it.action == Intent.ACTION_PROCESS_TEXT }
            ?.getCharSequenceExtra(Intent.EXTRA_PROCESS_TEXT)?.toString()
        if (selected != null) send(selected!!)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus || selected != null || isFinishing) return
        val clip = getSystemService(ClipboardManager::class.java).primaryClip
        val text = clip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(this)?.toString()
        if (text.isNullOrEmpty()) finish() else send(text)
    }

    private fun send(text: String) {
        val container = (application as LinkApplication).container
        if (!container.clipboard.isEcho(text)) {
            CoroutineScope(Dispatchers.Default).launch { container.repository.offerClipText(text) }
        }
        finish()
    }
}
