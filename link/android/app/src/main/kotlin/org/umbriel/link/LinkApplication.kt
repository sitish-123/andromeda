package org.umbriel.link

import android.app.Application
import android.os.Build
import android.provider.Settings
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.media.PhoneMedia
import org.umbriel.link.notifications.Channels
import org.umbriel.link.notifications.NotificationMirror
import org.umbriel.link.notifications.ShareNotifier
import org.umbriel.link.presence.Presence
import org.umbriel.link.ring.Ringer

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
    val repository = LinkRepository(application, deviceName(application))
    val presence = Presence(application, repository, scope)
    val mirror = NotificationMirror(application, repository, scope)
    val media = PhoneMedia(application, repository, scope)
    val ringer = Ringer(application, repository, scope)

    fun start() {
        Channels.create(application)
        presence.start()
        mirror.start()
        media.start()
        ringer.start()
        scope.launch { repository.incoming.collect(notifier::post) }
    }
}

/** The name desktops show; the protocol allows at most 64 characters. */
private fun deviceName(application: Application): String {
    val name = Settings.Global.getString(application.contentResolver, Settings.Global.DEVICE_NAME)
    return (name?.takeIf { it.isNotBlank() } ?: Build.MODEL).take(64)
}
