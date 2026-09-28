package org.umbriel.link.core.data

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.umbriel.link.ffi.HotspotCredentials
import org.umbriel.link.ffi.PhoneHotspot

/**
 * The phone's local-only hotspot, for moving a Bluetooth session to full speed: no tethering, no mobile data. Android
 * picks the SSID and passphrase; on Android 13+ it may run WPA3 in transition mode, which the desktop joins as WPA2.
 */
class LocalHotspot(private val context: Context) : PhoneHotspot {
    private var reservation: WifiManager.LocalOnlyHotspotReservation? = null

    override fun start(): HotspotCredentials? {
        synchronized(this) { reservation?.let { return credentials(it) } }
        val wifi = context.getSystemService(WifiManager::class.java) ?: return null
        val answered = CountDownLatch(1)
        var got: WifiManager.LocalOnlyHotspotReservation? = null
        val callback = object : WifiManager.LocalOnlyHotspotCallback() {
            override fun onStarted(started: WifiManager.LocalOnlyHotspotReservation) {
                got = started
                answered.countDown()
            }

            override fun onFailed(reason: Int) {
                Log.i(TAG, "local-only hotspot failed: $reason")
                answered.countDown()
            }
        }
        try {
            wifi.startLocalOnlyHotspot(callback, Handler(Looper.getMainLooper()))
        } catch (error: SecurityException) {
            Log.w(TAG, "local-only hotspot: ${error.message}")
            return null
        } catch (error: IllegalStateException) {
            Log.w(TAG, "local-only hotspot: ${error.message}")
            return null
        }
        answered.await(START_TIMEOUT_S, TimeUnit.SECONDS)
        val running = got ?: return null
        synchronized(this) { reservation = running }
        return credentials(running)
    }

    override fun stop() {
        synchronized(this) {
            reservation?.close()
            reservation = null
        }
    }

    @Suppress("DEPRECATION")
    private fun credentials(reservation: WifiManager.LocalOnlyHotspotReservation): HotspotCredentials? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val config = reservation.softApConfiguration
            val ssid = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                // WifiSsid prints a UTF-8 SSID in double quotes.
                config.wifiSsid?.toString()?.removeSurrounding("\"")
            } else {
                config.ssid
            }
            val passphrase = config.passphrase
            if (ssid != null && passphrase != null) HotspotCredentials(ssid, passphrase) else null
        } else {
            val config = reservation.wifiConfiguration
            val ssid = config?.SSID?.removeSurrounding("\"")
            val passphrase = config?.preSharedKey?.removeSurrounding("\"")
            if (ssid != null && passphrase != null) HotspotCredentials(ssid, passphrase) else null
        }

    private companion object {
        const val TAG = "LocalHotspot"
        const val START_TIMEOUT_S = 20L
    }
}
