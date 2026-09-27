package org.umbriel.link

import android.app.Application
import android.os.Build
import android.provider.Settings
import org.umbriel.link.core.data.LinkRepository

class LinkApplication : Application() {
    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        container = AppContainer(this)
    }
}

/** Every long-lived object, built once; screens receive what they need through their ViewModel's constructor. */
class AppContainer(application: Application) {
    val repository = LinkRepository(application, deviceName(application))
}

/** The name desktops show; the protocol allows at most 64 characters. */
private fun deviceName(application: Application): String {
    val name = Settings.Global.getString(application.contentResolver, Settings.Global.DEVICE_NAME)
    return (name?.takeIf { it.isNotBlank() } ?: Build.MODEL).take(64)
}
