package org.umbriel.link.presence

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.core.domain.Desktop

/**
 * Pairs Bluetooth with each desktop while a Wi-Fi session to it runs, so Bluetooth can carry the session later: the
 * desktop accepts connections only from devices it is paired with. Android asks the user once ("Pair with Arch?");
 * the code it shows goes to the desktop over the Link session, which confirms it there without asking. Tried once
 * per desktop per process, so a declined pairing is not asked again until the app restarts.
 */
class BluetoothBonding(
    private val context: Context,
    private val repository: LinkRepository,
    private val scope: CoroutineScope,
) {
    private val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter
    private val tried = mutableSetOf<String>()

    fun start() {
        scope.launch {
            repository.desktops.collect { desktops -> desktops.forEach(::pairIfNeeded) }
        }
    }

    @SuppressLint("MissingPermission")
    private fun pairIfNeeded(desktop: Desktop) {
        val address = desktop.bluetoothAddress ?: return
        val adapter = adapter ?: return
        if (!desktop.connected || desktop.bluetooth || !desktop.btPairing || desktop.id in tried || !allowed()) return
        if (!adapter.isEnabled) return
        val device = adapter.getRemoteDevice(address)
        if (device.bondState != BluetoothDevice.BOND_NONE) return
        tried += desktop.id
        scope.launch {
            // The desktop refuses unpaired devices until told; the pairing would fail before Android asks the user.
            if (repository.reportBtPairing(desktop.id, null).isFailure) return@launch
            delay(OPEN_MS)
            listen(desktop.id, address)
            if (!device.createBond()) Log.i(TAG, "Android did not start pairing with $address")
        }
    }

    /** Reports the code of the pairing request for [address], then stops listening once the pairing settles. */
    private fun listen(desktopId: String, address: String) {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                val device = intent.device() ?: return
                if (!device.address.equals(address, ignoreCase = true)) return
                when (intent.action) {
                    BluetoothDevice.ACTION_PAIRING_REQUEST -> {
                        val passkey = intent.getIntExtra(BluetoothDevice.EXTRA_PAIRING_KEY, -1)
                        if (passkey >= 0) scope.launch { repository.reportBtPairing(desktopId, passkey) }
                    }
                    BluetoothDevice.ACTION_BOND_STATE_CHANGED -> {
                        val state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.BOND_NONE)
                        if (state != BluetoothDevice.BOND_BONDING) {
                            Log.i(TAG, "pairing with $address: ${if (state == BluetoothDevice.BOND_BONDED) "paired" else "not paired"}")
                            context.unregisterReceiver(this)
                        }
                    }
                }
            }
        }
        val filter = IntentFilter(BluetoothDevice.ACTION_PAIRING_REQUEST).apply {
            addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
            // Ahead of Settings, which shows the dialog: this receiver only reads the code and lets it through.
            priority = IntentFilter.SYSTEM_HIGH_PRIORITY - 1
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED)
        } else {
            context.registerReceiver(receiver, filter)
        }
    }

    private fun allowed(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    private fun Intent.device(): BluetoothDevice? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
    } else {
        @Suppress("DEPRECATION")
        getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
    }

    private companion object {
        const val TAG = "BluetoothBonding"
        /** Time for the desktop's adapter to start accepting connections. */
        const val OPEN_MS = 1_500L
    }
}
