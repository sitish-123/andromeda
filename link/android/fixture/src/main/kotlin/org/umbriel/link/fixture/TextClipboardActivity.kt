package org.umbriel.link.fixture

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager

/** Copies the `text` extra, as any other app's copy would. */
class TextClipboardActivity : Activity() {
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus || isFinishing) return
        val text = intent.getStringExtra("text").orEmpty()
        getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newPlainText("E2E", text))
        finish()
    }
}
