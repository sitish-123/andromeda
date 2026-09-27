package org.umbriel.link.core.data

import android.content.Context
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.ParcelFileDescriptor
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.core.domain.IncomingClip
import org.umbriel.link.core.domain.IncomingShare
import org.umbriel.link.core.domain.LinkFailure
import org.umbriel.link.core.domain.LinkFailureException
import org.umbriel.link.core.domain.OfferedFile
import org.umbriel.link.core.domain.SavedFile
import org.umbriel.link.core.domain.ShareKind
import org.umbriel.link.core.domain.TransferEvent
import org.umbriel.link.ffi.LinkClient
import org.umbriel.link.ffi.LinkEvent
import org.umbriel.link.ffi.LinkException
import org.umbriel.link.ffi.OutgoingFile
import org.umbriel.link.ffi.generateIdentity
import org.umbriel.link.ffi.Desktop as FfiDesktop
import org.umbriel.link.ffi.ShareKind as FfiShareKind

/**
 * The phone side of Link over the Rust core. Every operation returns a [Result] whose failure is a
 * [LinkFailureException]; nothing throws into the UI. [desktops] follows the core's events, so its connected flags
 * are live.
 */
class LinkRepository(private val context: Context, private val deviceName: String) {

    /** Lives as long as the process: it reads the core's events for every screen and service. */
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val clientLock = Mutex()
    private var client: LinkClient? = null
    private val presenceLock = Mutex()
    private var present = false
    private val multicast = context.applicationContext.getSystemService(WifiManager::class.java)
        .createMulticastLock("umbriel-link")
        .apply { setReferenceCounted(true) }
    private val _desktops = MutableStateFlow<List<Desktop>>(emptyList())
    private val _incoming = MutableSharedFlow<IncomingShare>(extraBufferCapacity = INCOMING_BUFFER)
    private val _transfers = MutableSharedFlow<TransferEvent>(extraBufferCapacity = INCOMING_BUFFER)
    private val downloads = Downloads(context)
    private val _activeTransfers = MutableStateFlow<Set<String>>(emptySet())
    private val _clips = MutableSharedFlow<IncomingClip>(extraBufferCapacity = INCOMING_BUFFER)

    val desktops: StateFlow<List<Desktop>> = _desktops.asStateFlow()

    /** Shares desktops sent, as they arrive. */
    val incoming: SharedFlow<IncomingShare> = _incoming.asSharedFlow()

    /** Offers, progress, and results of file transfers in both directions. */
    val transfers: SharedFlow<TransferEvent> = _transfers.asSharedFlow()

    /** Desktop clipboards, as they change. */
    val clips: SharedFlow<IncomingClip> = _clips.asSharedFlow()

    /** The battery (percent) and network (wifi, cellular, ethernet, none, or other) desktops show. */
    suspend fun setStatus(battery: Int, charging: Boolean, network: String): Result<Unit> =
        call { it.setStatus(battery.coerceIn(0, 100).toUByte(), charging, network) }

    /** Offers text from this phone's clipboard to every connected desktop. */
    suspend fun offerClipText(text: String): Result<Unit> = call { it.offerClipText(text) }

    /** Writes one type of a desktop's clipboard into [fd], which the core takes over. */
    suspend fun pullClip(desktopId: String, clipId: Long, mime: String, fd: Int): Result<Long> =
        call { it.pullClip(desktopId, clipId.toULong(), mime, fd).toLong() }

    /** Transfers sent or accepted here that have not finished. */
    val activeTransfers: StateFlow<Set<String>> = _activeTransfers.asStateFlow()

    suspend fun refresh(): Result<Unit> = call { client ->
        _desktops.value = client.desktops().map { it.toDomain() }
    }

    suspend fun pairUri(uri: String): Result<Desktop> = callAndRefresh { it.pairUri(uri).toDomain() }

    /** Finds the pairing desktop by mDNS, so the multicast lock is held for the attempt. */
    suspend fun pairCode(code: String): Result<Desktop> = withMulticast {
        callAndRefresh { it.pairCode(code, emptyList()).toDomain() }
    }

    suspend fun connect(id: String): Result<Unit> = withMulticast { callAndRefresh { it.connect(id) } }

    /** Succeeds with whether the desktop was told; it is forgotten on this phone either way. */
    suspend fun unpair(id: String): Result<Boolean> = callAndRefresh { it.unpair(id) }

    /** Connects first if needed, and succeeds once the desktop acknowledged the share. */
    suspend fun share(desktopId: String, kind: ShareKind, text: String): Result<Unit> = withMulticast {
        call { it.share(desktopId, kind.toFfi(), text) }
    }

    /**
     * Offers the files behind content URIs; succeeds with the transfer id once the offer is on its way. A URI that is
     * not a regular file (a pipe from some providers) fails the whole send.
     */
    suspend fun sendFiles(desktopId: String, uris: List<String>): Result<String> = withMulticast {
        val opened = mutableListOf<Downloads.Opened>()
        val failure = withContext(Dispatchers.IO) {
            runCatching { uris.forEach { opened += downloads.open(Uri.parse(it)) } }.exceptionOrNull()
        }
        if (failure != null) {
            opened.forEach { ParcelFileDescriptor.adoptFd(it.fd).close() }
            return@withMulticast Result.failure(LinkFailureException(LinkFailure.Rejected(failure.message.orEmpty())))
        }
        val files = opened.map { OutgoingFile(it.fd, it.name, it.size.toULong(), it.mime) }
        call { it.sendFiles(desktopId, files) }.onSuccess { id -> _activeTransfers.update { it + id } }
    }

    suspend fun acceptTransfer(transferId: String): Result<Boolean> = call { it.acceptTransfer(transferId) }
        .onSuccess { accepted -> if (accepted) _activeTransfers.update { it + transferId } }

    suspend fun declineTransfer(transferId: String): Result<Boolean> = call { it.declineTransfer(transferId) }

    suspend fun cancelTransfer(transferId: String): Result<Boolean> = call { it.cancelTransfer(transferId) }

    /**
     * While present, every paired desktop stays connected and is redialled when it drops; the multicast lock is held
     * so the redial can fall back to mDNS.
     */
    suspend fun setPresent(present: Boolean): Result<Unit> = presenceLock.withLock {
        if (present == this.present) return@withLock Result.success(Unit)
        if (present) multicast.acquire() else multicast.release()
        this.present = present
        call { it.setPresent(present) }
    }

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
            val incoming = context.noBackupFilesDir.resolve("incoming").apply { mkdirs() }
            LinkClient(identity, context.filesDir.resolve("devices.json").absolutePath, deviceName, incoming.absolutePath)
        }.also {
            client = it
            scope.launch { follow(it) }
        }
    }

    private suspend fun follow(client: LinkClient) {
        while (true) {
            when (val event = client.nextEvent() ?: return) {
                is LinkEvent.Connected -> markConnected(event.desktopId, true)
                is LinkEvent.Disconnected -> markConnected(event.desktopId, false)
                is LinkEvent.Received -> _incoming.emit(
                    IncomingShare(event.desktopId, nameOf(event.desktopId), event.kind.toDomain(), event.text),
                )
                is LinkEvent.Unpaired -> scope.launch { refresh() }
                is LinkEvent.TransferOffered -> _transfers.emit(
                    TransferEvent.Offered(event.transferId, nameOf(event.desktopId), event.files.map { OfferedFile(it.name, it.size.toLong()) }),
                )
                is LinkEvent.TransferProgress ->
                    _transfers.emit(TransferEvent.Progress(event.transferId, event.bytes.toLong(), event.total.toLong()))
                is LinkEvent.TransferFinished -> scope.launch { finished(event) }
                is LinkEvent.ClipOffered ->
                    _clips.emit(IncomingClip(event.desktopId, event.clipId.toLong(), event.mimes, event.text))
            }
        }
    }

    /** Publishes what an incoming transfer received; off the event loop, since copying a large file takes a while. */
    private suspend fun finished(event: LinkEvent.TransferFinished) {
        val saved = withContext(Dispatchers.IO) {
            event.files.map { received ->
                val file = File(received.path)
                val uri = downloads.publish(file, received.sha256)
                if (uri == null) file.delete()
                uri?.let { SavedFile(file.name, it.toString()) }
            }
        }
        _activeTransfers.update { it - event.transferId }
        val status = if (saved.any { it == null }) "failed" else event.status
        _transfers.emit(
            TransferEvent.Finished(event.transferId, nameOf(event.desktopId), event.incoming, status, saved.filterNotNull()),
        )
    }

    /** A desktop not listed yet was just paired; the list is reread outside the event loop, which must keep reading. */
    private fun markConnected(id: String, connected: Boolean) {
        if (_desktops.value.none { it.id == id }) {
            scope.launch { refresh() }
            return
        }
        _desktops.update { list -> list.map { if (it.id == id) it.copy(connected = connected) else it } }
    }

    private fun nameOf(id: String): String = _desktops.value.firstOrNull { it.id == id }?.name ?: id

    private suspend fun <T> withMulticast(block: suspend () -> T): T {
        multicast.acquire()
        try {
            return block()
        } finally {
            multicast.release()
        }
    }

    private companion object {
        const val INCOMING_BUFFER = 16
    }
}

private fun FfiDesktop.toDomain() = Desktop(id = id, name = name, connected = connected, lastSeen = lastSeen.toLong())

private fun ShareKind.toFfi(): FfiShareKind = when (this) {
    ShareKind.Text -> FfiShareKind.TEXT
    ShareKind.Link -> FfiShareKind.LINK
}

private fun FfiShareKind.toDomain(): ShareKind = when (this) {
    FfiShareKind.TEXT -> ShareKind.Text
    FfiShareKind.LINK -> ShareKind.Link
}

private fun LinkException.toFailure(): LinkFailure = when (this) {
    is LinkException.WrongCode -> LinkFailure.WrongCode
    is LinkException.Unpaired -> LinkFailure.Unpaired
    is LinkException.Unreachable -> LinkFailure.Unreachable
    is LinkException.Rejected -> LinkFailure.Rejected(reason)
    is LinkException.Failed -> LinkFailure.Other(reason)
}
