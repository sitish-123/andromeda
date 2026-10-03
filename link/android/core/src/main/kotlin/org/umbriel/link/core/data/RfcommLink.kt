package org.umbriel.link.core.data

import android.Manifest
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import android.util.Log
import java.io.Closeable
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID
import kotlin.concurrent.thread
import org.umbriel.link.ffi.BluetoothLink

/**
 * Reaches a desktop's Link service over RFCOMM for the core, which needs a descriptor: `BluetoothSocket` exposes only
 * streams, so two threads pump it through a socket pair and the core gets the other end.
 */
class RfcommLink(private val context: Context) : BluetoothLink {
    override fun open(address: String, pair: Boolean): Int {
        if (!allowed()) return NOT_CONNECTED
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter ?: return NOT_CONNECTED
        if (!adapter.isEnabled) return NOT_CONNECTED
        val device = adapter.getRemoteDevice(address)
        val socket = try {
            if (pair && !bonded(device)) return NOT_CONNECTED
            connect(device) ?: return NOT_CONNECTED
        } catch (error: SecurityException) {
            Log.w(TAG, "RFCOMM to $address: ${error.message}")
            return NOT_CONNECTED
        }
        val (ours, theirs) = ParcelFileDescriptor.createSocketPair()
        val writer = ours.dup()
        val closeAll = { listOf<Closeable>(socket, ours, writer).forEach { runCatching { it.close() } } }
        pump("rfcomm-in", socket.inputStream, FileOutputStream(writer.fileDescriptor)) {
            // The core reads end-of-stream only once no end can write, so shut down ours before closing.
            runCatching { Os.shutdown(writer.fileDescriptor, OsConstants.SHUT_WR) }
            closeAll()
        }
        pump("rfcomm-out", FileInputStream(ours.fileDescriptor), socket.outputStream, closeAll)
        return theirs.detachFd()
    }

    /**
     * The encrypted socket first when the phone and desktop are already paired, since Android may refuse an
     * unencrypted one to a bonded device; else the unencrypted one, which needs no pairing because Link's TLS
     * authenticates both ends. A failure refreshes the desktop's service list, which Android caches from pairing and
     * which predates Link's profile.
     */
    private fun connect(device: BluetoothDevice): BluetoothSocket? {
        val bonded = device.bondState == BluetoothDevice.BOND_BONDED
        val attempts = if (bonded) listOf(true, false) else listOf(false)
        for (secure in attempts) {
            val socket = if (secure) {
                device.createRfcommSocketToServiceRecord(SERVICE)
            } else {
                device.createInsecureRfcommSocketToServiceRecord(SERVICE)
            }
            try {
                socket.connect()
                return socket
            } catch (error: IOException) {
                Log.i(TAG, "RFCOMM to ${device.address} (${if (secure) "encrypted" else "unencrypted"}): ${error.message}")
                runCatching { socket.close() }
            }
        }
        device.fetchUuidsWithSdp()
        return null
    }

    /**
     * Pairs with [device] unless already paired, and waits for the user to confirm it on both screens: the desktop
     * accepts connections only from devices it is paired with.
     */
    private fun bonded(device: BluetoothDevice): Boolean {
        if (device.bondState == BluetoothDevice.BOND_BONDED) return true
        if (device.bondState == BluetoothDevice.BOND_NONE && !device.createBond()) {
            Log.i(TAG, "Android did not start pairing with ${device.address}")
            return false
        }
        val deadline = SystemClock.elapsedRealtime() + BOND_TIMEOUT_MS
        while (device.bondState == BluetoothDevice.BOND_BONDING && SystemClock.elapsedRealtime() < deadline) {
            SystemClock.sleep(POLL_MS)
        }
        Log.i(TAG, "pairing with ${device.address}: ${if (device.bondState == BluetoothDevice.BOND_BONDED) "paired" else "not paired"}")
        return device.bondState == BluetoothDevice.BOND_BONDED
    }

    private fun allowed(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    private fun pump(name: String, from: InputStream, to: OutputStream, done: () -> Unit) {
        thread(name = name, isDaemon = true) {
            val buffer = ByteArray(BUFFER)
            try {
                while (true) {
                    val read = from.read(buffer)
                    if (read < 0) break
                    to.write(buffer, 0, read)
                    to.flush()
                }
            } catch (_: IOException) {
                // Either side closing ends the link; the core sees it as the connection lost.
            } finally {
                done()
            }
        }
    }

    private companion object {
        const val TAG = "RfcommLink"
        /** Long enough for the user to read the code on both screens and confirm. */
        const val BOND_TIMEOUT_MS = 60_000L
        const val POLL_MS = 250L
        const val NOT_CONNECTED = -1
        const val BUFFER = 16 * 1024
        /** The desktop's `bluetooth::SERVICE_UUID`. */
        val SERVICE: UUID = UUID.fromString("5c3b1e5a-7d2f-4a8e-9b61-3f0c2d4e8a17")
    }
}
