package org.umbriel.link.clipboard

import android.Manifest
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.provider.Settings
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Phone to desktop, automatic, as KDE Connect does it: Android lets only the focused app read the clipboard, but it
 * logs a denial for this app on every copy elsewhere, because this app listens for changes. With READ_LOGS (granted
 * once over adb; a sideload-build path, not Play) each denial line brings up [ClipboardReadActivity] for a moment,
 * which reads the clipboard while focused and offers it. Starting it from the background needs "Display over other
 * apps" as well. Runs while [org.umbriel.link.presence.PresenceService] does.
 */
class ClipboardWatcher(
    private val context: Context,
    private val sync: ClipboardSync,
    private val scope: CoroutineScope,
) {
    private val clipboard = context.getSystemService(ClipboardManager::class.java)
    private val listener = ClipboardManager.OnPrimaryClipChangedListener {}
    private var job: Job? = null
    private var process: Process? = null
    /** Whether logcat has shown this watcher any line; `-T 1` shows the newest at once when it may read them. */
    @Volatile private var sees = false

    fun start() {
        if (job != null || !granted(context)) return
        clipboard.addPrimaryClipChangedListener(listener)
        job = scope.launch(Dispatchers.IO) { follow() }
    }

    /**
     * Restarts a watcher that has seen nothing: Android 13+ asks before an app reads other apps' log lines, and a
     * watcher started in the background is refused without a prompt. Called when the app comes to the foreground.
     */
    fun restartIfBlind() {
        if (job != null && sees) return
        stop()
        start()
    }

    fun stop() {
        clipboard.removePrimaryClipChangedListener(listener)
        process?.destroy()
        job?.cancel()
        job = null
    }

    private suspend fun follow() {
        // -T 1 starts at the newest line, so only denials from now on count.
        val logcat = ProcessBuilder("logcat", "-T", "1", "-v", "brief", "ClipboardService:E", "*:S").start()
        process = logcat
        sees = false
        val denial = "Denying clipboard access to ${context.packageName}"
        logcat.inputStream.bufferedReader().useLines { lines ->
            for (line in lines) {
                if (!kotlinx.coroutines.currentCoroutineContext().isActive) break
                sees = true
                if (denial in line) {
                    sync.changedLocally()
                    context.startActivity(
                        Intent(context, ClipboardReadActivity::class.java).addFlags(
                            Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION,
                        ),
                    )
                }
            }
        }
    }

    companion object {
        fun granted(context: Context): Boolean =
            context.checkSelfPermission(Manifest.permission.READ_LOGS) == PackageManager.PERMISSION_GRANTED &&
                Settings.canDrawOverlays(context)
    }
}
