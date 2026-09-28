package org.umbriel.link.presence

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.os.BatteryManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository

/**
 * Reports the battery and the network to desktops as they change; the core sends it on connect and rate-limits the
 * rest. Battery changes arrive as the sticky ACTION_BATTERY_CHANGED broadcast, networks through the default network
 * callback, so nothing polls.
 */
class StatusReporter(private val context: Context, private val repository: LinkRepository, private val scope: CoroutineScope) {
    private data class Status(val battery: Int, val charging: Boolean, val network: String)

    private val battery = MutableStateFlow<Pair<Int, Boolean>?>(null)
    private val network = MutableStateFlow("none")

    fun start() {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
                val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100)
                if (level < 0 || scale <= 0) return
                val plugged = intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0
                battery.value = (level * 100 / scale) to plugged
            }
        }
        context.registerReceiver(receiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        context.getSystemService(ConnectivityManager::class.java).registerDefaultNetworkCallback(
            object : ConnectivityManager.NetworkCallback() {
                override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) {
                    this@StatusReporter.network.value = kindOf(capabilities)
                }

                override fun onLost(network: Network) {
                    this@StatusReporter.network.value = "none"
                }
            },
        )
        scope.launch {
            kotlinx.coroutines.flow.combine(battery.filterNotNull(), network) { (level, charging), kind ->
                Status(level, charging, kind)
            }.distinctUntilChanged().collect { repository.setStatus(it.battery, it.charging, it.network) }
        }
    }

    private fun kindOf(capabilities: NetworkCapabilities): String = when {
        capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> "wifi"
        capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> "cellular"
        capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> "ethernet"
        else -> "other"
    }
}
