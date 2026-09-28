package org.umbriel.link.core.data

import android.Manifest
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.ParcelFileDescriptor
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
    override fun open(address: String): Int {
        if (!allowed()) return NOT_CONNECTED
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter ?: return NOT_CONNECTED
        if (!adapter.isEnabled) return NOT_CONNECTED
        val socket = try {
            // Insecure: Link's TLS authenticates both ends with the paired keys, so no Bluetooth bond is needed.
            adapter.getRemoteDevice(address).createInsecureRfcommSocketToServiceRecord(SERVICE).apply { connect() }
        } catch (error: IOException) {
            Log.i(TAG, "RFCOMM to $address: ${error.message}")
            return NOT_CONNECTED
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
        const val NOT_CONNECTED = -1
        const val BUFFER = 16 * 1024
        /** The desktop's `bluetooth::SERVICE_UUID`. */
        val SERVICE: UUID = UUID.fromString("5c3b1e5a-7d2f-4a8e-9b61-3f0c2d4e8a17")
    }
}
