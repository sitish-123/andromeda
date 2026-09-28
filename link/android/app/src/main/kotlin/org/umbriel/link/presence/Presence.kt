package org.umbriel.link.presence

import android.app.Application
import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.Network
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository

/**
 * Decides when the phone is present: while any of its screens is in the foreground, and while "Stay connected" is on,
 * which also keeps [PresenceService] running so Android lets the connection live in the background. "Stay connected"
 * turns on with the first pairing, and the service comes back after a reboot ([BootReceiver]).
 */
class Presence(
    private val application: Application,
    private val repository: LinkRepository,
    private val scope: CoroutineScope,
) {
    private val prefs = application.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val foreground = MutableStateFlow(false)
    private val _stayConnected = MutableStateFlow(isStayConnected(application))
    private var network: Network? = null

    val stayConnected: StateFlow<Boolean> = _stayConnected.asStateFlow()

    fun start() {
        ProcessLifecycleOwner.get().lifecycle.addObserver(object : DefaultLifecycleObserver {
            override fun onStart(owner: LifecycleOwner) {
                foreground.value = true
                // The service may start only from the foreground, so a process restarted in the background waits here.
                syncService()
            }

            override fun onStop(owner: LifecycleOwner) {
                foreground.value = false
            }
        })
        scope.launch {
            combine(foreground, _stayConnected) { visible, stay -> visible || stay }
                .distinctUntilChanged()
                .collect { repository.setPresent(it) }
        }
        // A session on the old network looks alive until it times out; going absent and back redials at once.
        application.getSystemService(ConnectivityManager::class.java).registerDefaultNetworkCallback(
            object : ConnectivityManager.NetworkCallback() {
                override fun onAvailable(available: Network) {
                    val changed = network != null && network != available
                    network = available
                    if (changed && (foreground.value || _stayConnected.value)) {
                        scope.launch {
                            repository.setPresent(false)
                            repository.setPresent(true)
                        }
                    }
                }
            },
        )
    }

    /** Called after a pairing: the first one turns "Stay connected" on unless the user already chose. */
    fun paired() {
        if (!prefs.contains(KEY_STAY_CONNECTED)) setStayConnected(true)
    }

    fun setStayConnected(stay: Boolean) {
        prefs.edit { putBoolean(KEY_STAY_CONNECTED, stay) }
        _stayConnected.value = stay
        syncService()
    }

    /** A service notification posted before the grant stays hidden; starting the service again posts it. */
    fun notificationsAllowed() = syncService()

    private fun syncService() {
        val intent = Intent(application, PresenceService::class.java)
        if (_stayConnected.value) ContextCompat.startForegroundService(application, intent) else application.stopService(intent)
    }

    companion object {
        private const val PREFS = "presence"
        private const val KEY_STAY_CONNECTED = "stay_connected"

        fun isStayConnected(context: Context): Boolean =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY_STAY_CONNECTED, false)
    }
}
