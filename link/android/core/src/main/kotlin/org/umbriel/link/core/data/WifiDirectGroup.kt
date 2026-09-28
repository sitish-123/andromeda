package org.umbriel.link.core.data

import android.annotation.SuppressLint
import android.content.Context
import android.net.wifi.WpsInfo
import android.net.wifi.p2p.WifiP2pConfig
import android.net.wifi.p2p.WifiP2pDevice
import android.net.wifi.p2p.WifiP2pManager
import android.os.Build
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.umbriel.link.ffi.PhoneWifiDirect

/**
 * A Wi-Fi Direct group with a desktop as its client and this phone as owner, so the desktop keeps its own Wi-Fi while
 * the session runs at full speed. Both sides connect to each other at once: a connection this app starts needs no
 * confirmation on the phone, and the desktop accepts the negotiation it is waiting for. Needs Nearby devices (Android
 * 13+) or location, like the local-only hotspot.
 */
@SuppressLint("MissingPermission")
class WifiDirectGroup(private val context: Context) : PhoneWifiDirect {
    private val manager: WifiP2pManager? = context.getSystemService(WifiP2pManager::class.java)
    private var channel: WifiP2pManager.Channel? = null

    override fun name(): String? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return null
        val (manager, channel) = ready() ?: return null
        val answered = CountDownLatch(1)
        var name: String? = null
        guarded {
            manager.requestDeviceInfo(channel) { device ->
                name = device?.deviceName?.takeIf(String::isNotEmpty)
                answered.countDown()
            }
        } ?: return null
        answered.await(ANSWER_TIMEOUT_S, TimeUnit.SECONDS)
        return name
    }

    override fun connect(peer: String): Boolean {
        val (manager, channel) = ready() ?: return false
        guarded { manager.discoverPeers(channel, logged("discovering peers")) } ?: return false
        val device = find(manager, channel, peer) ?: return false.also { Log.i(TAG, "$peer did not show up") }
        val config = WifiP2pConfig().apply {
            deviceAddress = device.deviceAddress
            wps.setup = WpsInfo.PBC
            // The phone owns the group: it serves DHCP, and the desktop joins as a client beside its own network.
            groupOwnerIntent = OWNER_INTENT
        }
        guarded { manager.connect(channel, config, logged("connecting to $peer")) } ?: return false
        return formed(manager, channel)
    }

    override fun stop() {
        val (manager, channel) = ready() ?: return
        guarded {
            manager.removeGroup(channel, logged("removing the group"))
            manager.stopPeerDiscovery(channel, logged("stopping discovery"))
        }
    }

    private fun ready(): Pair<WifiP2pManager, WifiP2pManager.Channel>? {
        val manager = manager ?: return null
        val channel = synchronized(this) {
            channel ?: manager.initialize(context, Looper.getMainLooper(), null).also { channel = it }
        } ?: return null
        return manager to channel
    }

    private fun find(manager: WifiP2pManager, channel: WifiP2pManager.Channel, name: String): WifiP2pDevice? {
        val deadline = SystemClock.elapsedRealtime() + FIND_TIMEOUT_MS
        while (SystemClock.elapsedRealtime() < deadline) {
            val answered = CountDownLatch(1)
            var found: WifiP2pDevice? = null
            guarded {
                manager.requestPeers(channel) { peers ->
                    found = peers?.deviceList?.firstOrNull { it.deviceName == name }
                    answered.countDown()
                }
            } ?: return null
            answered.await(ANSWER_TIMEOUT_S, TimeUnit.SECONDS)
            found?.let { return it }
            SystemClock.sleep(POLL_MS)
        }
        return null
    }

    private fun formed(manager: WifiP2pManager, channel: WifiP2pManager.Channel): Boolean {
        val deadline = SystemClock.elapsedRealtime() + FORM_TIMEOUT_MS
        while (SystemClock.elapsedRealtime() < deadline) {
            val answered = CountDownLatch(1)
            var up = false
            guarded {
                manager.requestConnectionInfo(channel) { info ->
                    up = info?.groupFormed == true && info.isGroupOwner
                    answered.countDown()
                }
            } ?: return false
            answered.await(ANSWER_TIMEOUT_S, TimeUnit.SECONDS)
            if (up) return true
            SystemClock.sleep(POLL_MS)
        }
        Log.i(TAG, "the group did not form")
        return false
    }

    /** Runs [call] unless the app lacks the permission Wi-Fi Direct needs; null when it does. */
    private fun guarded(call: () -> Unit): Unit? = try {
        call()
    } catch (error: SecurityException) {
        Log.w(TAG, "Wi-Fi Direct: ${error.message}")
        null
    }

    private fun logged(what: String) = object : WifiP2pManager.ActionListener {
        override fun onSuccess() = Unit
        override fun onFailure(reason: Int) {
            Log.i(TAG, "$what failed: $reason")
        }
    }

    private companion object {
        const val TAG = "WifiDirectGroup"
        const val OWNER_INTENT = 15
        const val ANSWER_TIMEOUT_S = 5L
        const val FIND_TIMEOUT_MS = 15_000L
        const val FORM_TIMEOUT_MS = 20_000L
        const val POLL_MS = 500L
    }
}
