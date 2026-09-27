package org.umbriel.link.core.data

import android.content.Context
import android.net.wifi.WifiManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.core.domain.LinkFailure
import org.umbriel.link.core.domain.LinkFailureException
import org.umbriel.link.ffi.LinkClient
import org.umbriel.link.ffi.LinkException
import org.umbriel.link.ffi.generateIdentity
import org.umbriel.link.ffi.Desktop as FfiDesktop

/**
 * The phone side of Link over the Rust core. Every operation returns a [Result] whose failure is a
 * [LinkFailureException]; nothing throws into the UI.
 */
class LinkRepository(private val context: Context, private val deviceName: String) {

    private val clientLock = Mutex()
    private var client: LinkClient? = null
    private val wifi = context.applicationContext.getSystemService(WifiManager::class.java)
    private val _desktops = MutableStateFlow<List<Desktop>>(emptyList())

    val desktops: StateFlow<List<Desktop>> = _desktops.asStateFlow()

    suspend fun refresh(): Result<Unit> = call { client ->
        _desktops.value = client.desktops().map { it.toDomain() }
    }

    suspend fun pairUri(uri: String): Result<Desktop> = callAndRefresh { it.pairUri(uri).toDomain() }

    /** Finds the pairing desktop by mDNS, so the multicast lock is held for the attempt. */
    suspend fun pairCode(code: String): Result<Desktop> = withMulticast {
        callAndRefresh { it.pairCode(code, emptyList()).toDomain() }
    }

    suspend fun connect(id: String): Result<Desktop> = withMulticast { callAndRefresh { it.connect(id).toDomain() } }

    suspend fun disconnect(id: String): Result<Unit> = callAndRefresh { it.disconnect(id) }

    /** Succeeds with whether the desktop was told; it is forgotten on this phone either way. */
    suspend fun unpair(id: String): Result<Boolean> = callAndRefresh { it.unpair(id) }

    private suspend fun <T> callAndRefresh(block: suspend (LinkClient) -> T): Result<T> {
        val result = call(block)
        refresh()
        return result
    }

    private suspend fun <T> call(block: suspend (LinkClient) -> T): Result<T> = try {
        Result.success(block(client()))
    } catch (error: LinkException) {
        Result.failure(LinkFailureException(error.toFailure()))
    }

    private suspend fun client(): LinkClient = clientLock.withLock {
        client ?: withContext(Dispatchers.IO) {
            val identity = IdentityStore(context.filesDir.resolve("identity.bin")).loadOrCreate(::generateIdentity)
            LinkClient(identity, context.filesDir.resolve("devices.json").absolutePath, deviceName)
        }.also { client = it }
    }

    private suspend fun <T> withMulticast(block: suspend () -> T): T {
        val lock = wifi.createMulticastLock("umbriel-link").apply { setReferenceCounted(false) }
        lock.acquire()
        try {
            return block()
        } finally {
            lock.release()
        }
    }
}

private fun FfiDesktop.toDomain() = Desktop(id = id, name = name, connected = connected, lastSeen = lastSeen.toLong())

private fun LinkException.toFailure(): LinkFailure = when (this) {
    is LinkException.WrongCode -> LinkFailure.WrongCode
    is LinkException.Unpaired -> LinkFailure.Unpaired
    is LinkException.Unreachable -> LinkFailure.Unreachable
    is LinkException.Failed -> LinkFailure.Other(reason)
}
