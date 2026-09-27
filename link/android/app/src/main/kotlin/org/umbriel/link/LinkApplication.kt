package org.umbriel.link

import android.app.Application
import android.content.Intent
import android.util.Log
import androidx.core.content.ContextCompat
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import android.os.Build
import android.provider.Settings
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import org.umbriel.link.clipboard.ClipboardSync
import org.umbriel.link.clipboard.ClipboardWatcher
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.notifications.Channels
import org.umbriel.link.notifications.ShareNotifier
import org.umbriel.link.notifications.TransferNotifier
import org.umbriel.link.presence.Presence
import org.umbriel.link.transfer.TransferService

class LinkApplication : Application() {
    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        container = AppContainer(this)
        container.start()
    }
}

/** Every long-lived object, built once; screens receive what they need through their ViewModel's constructor. */
class AppContainer(private val application: Application) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val notifier = ShareNotifier(application)
    private val transferNotifier = TransferNotifier(application)
    val repository = LinkRepository(application, deviceName(application))
    val presence = Presence(application, repository, scope)
    val clipboard = ClipboardSync(application, repository, scope)
    val clipboardWatcher = ClipboardWatcher(application, clipboard, scope)

    fun start() {
        Channels.create(application)
        presence.start()
        clipboard.start()
        scope.launch { repository.incoming.collect(notifier::post) }
        scope.launch { repository.transfers.collect(transferNotifier::post) }
        scope.launch {
            // Transfers start from the share target or a notification action, both of which may start the service.
            repository.activeTransfers.map { it.isNotEmpty() }.distinctUntilChanged().collect { active ->
                val intent = Intent(application, TransferService::class.java)
                if (!active) {
                    application.stopService(intent)
                } else if (runCatching { ContextCompat.startForegroundService(application, intent) }.isFailure) {
                    Log.w("link", "a transfer started while in the background runs without its service")
                }
            }
        }
    }
}

/** The name desktops show; the protocol allows at most 64 characters. */
private fun deviceName(application: Application): String {
    val name = Settings.Global.getString(application.contentResolver, Settings.Global.DEVICE_NAME)
    return (name?.takeIf { it.isNotBlank() } ?: Build.MODEL).take(64)
}
