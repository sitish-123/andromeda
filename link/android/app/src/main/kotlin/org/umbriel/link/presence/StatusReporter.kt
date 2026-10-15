package org.umbriel.link.presence

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.os.BatteryManager
import android.os.Build
import android.telephony.SignalStrength
import android.telephony.TelephonyCallback
import android.telephony.TelephonyManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository

/**
 * Reports the battery, the network, and the cellular signal to desktops as they change; the core sends it on connect
 * and rate-limits the rest. Battery changes arrive as the sticky ACTION_BATTERY_CHANGED broadcast, networks through the
 * default network callback, the signal through a telephony callback (Android 12 and later), so nothing polls.
 */
class StatusReporter(private val context: Context, private val repository: LinkRepository, private val scope: CoroutineScope) {
    private data class Status(val battery: Int, val charging: Boolean, val network: String, val signal: Int?)

    private val battery = MutableStateFlow<Pair<Int, Boolean>?>(null)
    private val network = MutableStateFlow("none")
    private val signal = MutableStateFlow<Int?>(null)

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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) watchSignal()
        scope.launch {
            kotlinx.coroutines.flow.combine(battery.filterNotNull(), network, signal) { (level, charging), kind, bars ->
                Status(level, charging, kind, bars)
            }.distinctUntilChanged().collect { repository.setStatus(it.battery, it.charging, it.network, it.signal) }
        }
    }

    /** Bars of the SIM in service; none without a SIM. */
    @androidx.annotation.RequiresApi(Build.VERSION_CODES.S)
    private fun watchSignal() {
        val telephony = context.getSystemService(TelephonyManager::class.java) ?: return
        val listener = object : TelephonyCallback(), TelephonyCallback.SignalStrengthsListener {
            override fun onSignalStrengthsChanged(strength: SignalStrength) {
                signal.value = strength.level.takeIf { telephony.simState == TelephonyManager.SIM_STATE_READY }
            }
        }
        telephony.registerTelephonyCallback(context.mainExecutor, listener)
    }

    private fun kindOf(capabilities: NetworkCapabilities): String = when {
        capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> "wifi"
        capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> "cellular"
        capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> "ethernet"
        else -> "other"
    }
}
