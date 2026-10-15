package org.umbriel.link.clipboard

import android.Manifest
import android.content.ClipboardManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.provider.Settings
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.ProcessLifecycleOwner
import java.io.IOException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Phone to desktop, automatic, as KDE Connect does it: Android lets only the focused app read the clipboard, but it
 * logs a denial for this app on every copy elsewhere, because this app listens for changes. With READ_LOGS (granted
 * once over adb; a sideload-build path, not Play) each denial line brings up a [ClipboardOverlay] for a moment,
 * which reads the clipboard while focused and offers it. The overlay needs "Display over other apps" as well. Runs
 * while [org.umbriel.link.presence.PresenceService] does.
 */
class ClipboardWatcher(
    private val context: Context,
    private val sync: ClipboardSync,
    private val scope: CoroutineScope,
) {
    private val overlay = ClipboardOverlay(context, sync, scope)
    private val clipboard = context.getSystemService(ClipboardManager::class.java)
    private val listener = ClipboardManager.OnPrimaryClipChangedListener {}
    private var job: Job? = null
    private var process: Process? = null
    private var wanted = false

    init {
        ProcessLifecycleOwner.get().lifecycle.addObserver(
            LifecycleEventObserver { _, event -> if (event == Lifecycle.Event.ON_START && wanted) restartIfBlind() },
        )
    }

    /**
     * Android 13+ asks before each logcat reads other apps' lines, and only while the app is in the foreground. In the
     * background it refuses without a prompt and keeps refusing for a minute, which would also refuse the foreground
     * start that follows; so there the watcher waits for the foreground.
     */
    fun start() {
        wanted = true
        if (job != null || !granted(context) || !mayAsk()) return
        clipboard.addPrimaryClipChangedListener(listener)
        job = scope.launch(Dispatchers.IO) { follow() }
    }

    /**
     * Starts a watcher that waited for the foreground, or whose logcat ended, when the app comes to the foreground; a
     * running watcher keeps its logcat, since a new one asks again.
     */
    private fun restartIfBlind() {
        if (job != null && process?.isAlive != false) return
        stop()
        start()
    }

    private fun mayAsk(): Boolean = Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
        ProcessLifecycleOwner.get().lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)

    fun stop() {
        wanted = false
        clipboard.removePrimaryClipChangedListener(listener)
        process?.destroy()
        process = null
        job?.cancel()
        job = null
    }

    private suspend fun follow() {
        // -T 1 starts at the newest line, so only denials from now on count.
        val logcat = ProcessBuilder("logcat", "-T", "1", "-v", "brief", "ClipboardService:E", "*:S").start()
        process = logcat
        val denial = "Denying clipboard access to ${context.packageName}"
        try {
            logcat.inputStream.bufferedReader().useLines { lines ->
                for (line in lines) {
                    if (!kotlinx.coroutines.currentCoroutineContext().isActive) break
                    if (denial in line) {
                        sync.changedLocally()
                        overlay.read()
                    }
                }
            }
        } catch (_: IOException) {
            // stop() destroys logcat under a blocked read, which is how this ends.
        }
    }

    companion object {
        fun granted(context: Context): Boolean =
            context.checkSelfPermission(Manifest.permission.READ_LOGS) == PackageManager.PERMISSION_GRANTED &&
                Settings.canDrawOverlays(context)
    }
}
