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
import android.os.SystemClock
import org.umbriel.link.core.domain.CallAction
import org.umbriel.link.core.domain.CallState
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.core.domain.DesktopPlayer
import org.umbriel.link.core.domain.Feature
import org.umbriel.link.core.domain.IncomingClip
import org.umbriel.link.core.domain.IncomingShare
import org.umbriel.link.core.domain.LinkFailure
import org.umbriel.link.core.domain.LinkFailureException
import org.umbriel.link.core.domain.MediaCommandKind
import org.umbriel.link.core.domain.MediaPlayer
import org.umbriel.link.core.domain.NotificationCommand
import org.umbriel.link.core.domain.OfferedFile
import org.umbriel.link.core.domain.PhoneMediaCommand
import org.umbriel.link.core.domain.PhoneNotification
import org.umbriel.link.core.domain.PlaybackState
import org.umbriel.link.core.domain.SavedFile
import org.umbriel.link.core.domain.ShareKind
import org.umbriel.link.core.domain.Sharing
import org.umbriel.link.core.domain.TransferEvent
import org.umbriel.link.ffi.LinkClient
import org.umbriel.link.ffi.LinkEvent
import org.umbriel.link.ffi.LinkException
import org.umbriel.link.ffi.OutgoingFile
import org.umbriel.link.ffi.generateIdentity
import org.umbriel.link.ffi.Desktop as FfiDesktop
import org.umbriel.link.ffi.CallAction as FfiCallAction
import org.umbriel.link.ffi.CallState as FfiCallState
import org.umbriel.link.ffi.Feature as FfiFeature
import org.umbriel.link.ffi.MediaCommandKind as FfiCommand
import org.umbriel.link.ffi.MediaPlayer as FfiPlayer
import org.umbriel.link.ffi.PlaybackState as FfiState
import org.umbriel.link.ffi.NotificationButton as FfiNotificationButton
import org.umbriel.link.ffi.PhoneNotification as FfiPhoneNotification
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
    private val _connections = MutableSharedFlow<String>(extraBufferCapacity = INCOMING_BUFFER)
    private val _notificationCommands = MutableSharedFlow<NotificationCommand>(extraBufferCapacity = INCOMING_BUFFER)
    private val _mediaCommands = MutableSharedFlow<PhoneMediaCommand>(extraBufferCapacity = INCOMING_BUFFER)
    private val _desktopPlayers = MutableStateFlow<List<DesktopPlayer>>(emptyList())
    private val _ringRequests = MutableSharedFlow<Pair<String, Boolean>>(extraBufferCapacity = INCOMING_BUFFER)
    private val _ringingDesktops = MutableStateFlow<Set<String>>(emptySet())
    private val _callActions = MutableSharedFlow<CallAction>(extraBufferCapacity = INCOMING_BUFFER)

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

    /** The id of each desktop as a session to it opens. */
    val connections: SharedFlow<String> = _connections.asSharedFlow()

    /** Actions and dismissals desktops ask of mirrored notifications. */
    val notificationCommands: SharedFlow<NotificationCommand> = _notificationCommands.asSharedFlow()

    /** Commands desktops send this phone's player. */
    val mediaCommands: SharedFlow<PhoneMediaCommand> = _mediaCommands.asSharedFlow()

    /** Every connected desktop's players; a desktop's go when its session ends. */
    val desktopPlayers: StateFlow<List<DesktopPlayer>> = _desktopPlayers.asStateFlow()

    /** A desktop asks this phone to ring (true) or stop, as (desktop id, on). */
    val ringRequests: SharedFlow<Pair<String, Boolean>> = _ringRequests.asSharedFlow()

    /** Desktops ringing because this phone asked, as they report it. */
    val ringingDesktops: StateFlow<Set<String>> = _ringingDesktops.asStateFlow()

    /** What desktops ask of a ringing call. */
    val callActions: SharedFlow<CallAction> = _callActions.asSharedFlow()

    /** Tells connected desktops that take calls; `number` and `name` only when known. */
    suspend fun reportCall(state: CallState, number: String?, name: String?): Result<Int> = call {
        val ffiState = when (state) {
            CallState.Ringing -> FfiCallState.RINGING
            CallState.Active -> FfiCallState.ACTIVE
            CallState.Idle -> FfiCallState.IDLE
        }
        it.reportCall(ffiState, number, name).toInt()
    }

    /** Connects first if needed. */
    suspend fun ringDesktop(desktopId: String, on: Boolean): Result<Unit> =
        withMulticast { call { it.ringDesktop(desktopId, on) } }

    /** Tells connected desktops that may ring this phone whether it rings. */
    suspend fun reportRinging(on: Boolean): Result<Int> = call { it.reportRinging(on).toInt() }

    /** Succeeds with how many connected desktops took it. */
    suspend fun publishPlayer(player: MediaPlayer): Result<Int> = call { it.publishPlayer(player.toFfi()).toInt() }

    suspend fun playerGone(player: String): Result<Int> = call { it.playerGone(player).toInt() }

    /** Connects first if needed. */
    suspend fun mediaCommand(desktopId: String, player: String, command: MediaCommandKind, value: Long? = null): Result<Unit> =
        call { it.mediaCommand(desktopId, player, command.toFfi(), value?.toULong()) }

    /** Succeeds with how many connected desktops took it; none are dialled. */
    suspend fun postNotification(notification: PhoneNotification): Result<Int> = call {
        it.postNotification(notification.toFfi()).toInt()
    }

    suspend fun removeNotification(id: String): Result<Int> = call { it.removeNotification(id).toInt() }

    suspend fun setSharing(desktopId: String, feature: Feature, on: Boolean): Result<Unit> =
        callAndRefresh { it.setSharing(desktopId, feature.toFfi(), on) }

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
                is LinkEvent.Connected -> {
                    markConnected(event.desktopId, true)
                    _connections.emit(event.desktopId)
                }
                is LinkEvent.Disconnected -> {
                    markConnected(event.desktopId, false)
                    _desktopPlayers.update { list -> list.filter { it.desktopId != event.desktopId } }
                }
                is LinkEvent.PlayerChanged -> {
                    val player = event.player.toDomain(SystemClock.elapsedRealtime())
                    _desktopPlayers.update { list ->
                        list.filterNot { it.desktopId == event.desktopId && it.player.player == player.player } +
                            DesktopPlayer(event.desktopId, player)
                    }
                }
                is LinkEvent.PlayerGone -> _desktopPlayers.update { list ->
                    list.filterNot { it.desktopId == event.desktopId && it.player.player == event.player }
                }
                is LinkEvent.RingRequested -> _ringRequests.emit(event.desktopId to event.on)
                is LinkEvent.CallActionRequested -> _callActions.emit(
                    if (event.action == FfiCallAction.MUTE) CallAction.Mute else CallAction.Decline,
                )
                is LinkEvent.DesktopRinging -> _ringingDesktops.update {
                    if (event.on) it + event.desktopId else it - event.desktopId
                }
                is LinkEvent.PlayerCommand -> _mediaCommands.emit(
                    PhoneMediaCommand(event.desktopId, event.player, event.command.toDomain(), event.value?.toLong()),
                )
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
                is LinkEvent.NotificationAction -> _notificationCommands.emit(
                    NotificationCommand.Action(event.desktopId, event.id, event.action, event.replyText),
                )
                is LinkEvent.NotificationDismissed ->
                    _notificationCommands.emit(NotificationCommand.Dismiss(event.desktopId, event.id))
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

private fun FfiDesktop.toDomain() = Desktop(
    id = id,
    name = name,
    connected = connected,
    lastSeen = lastSeen.toLong(),
    sharing = Sharing(sharing.clipboard, sharing.files, sharing.notifications, sharing.media, sharing.ring, sharing.calls),
)

private fun MediaCommandKind.toFfi(): FfiCommand = when (this) {
    MediaCommandKind.Play -> FfiCommand.PLAY
    MediaCommandKind.Pause -> FfiCommand.PAUSE
    MediaCommandKind.PlayPause -> FfiCommand.PLAY_PAUSE
    MediaCommandKind.Next -> FfiCommand.NEXT
    MediaCommandKind.Previous -> FfiCommand.PREVIOUS
    MediaCommandKind.Seek -> FfiCommand.SEEK
    MediaCommandKind.Volume -> FfiCommand.VOLUME
}

private fun FfiCommand.toDomain(): MediaCommandKind = MediaCommandKind.entries.first { it.toFfi() == this }

private fun PlaybackState.toFfi(): FfiState = when (this) {
    PlaybackState.Playing -> FfiState.PLAYING
    PlaybackState.Paused -> FfiState.PAUSED
    PlaybackState.Stopped -> FfiState.STOPPED
}

private fun MediaPlayer.toFfi() = FfiPlayer(
    player = player,
    name = name,
    state = state.toFfi(),
    title = title,
    artist = artist,
    album = album,
    lengthMs = lengthMs?.toULong(),
    positionMs = positionMs.toULong(),
    volume = volume?.toUByte(),
    artwork = artwork,
    can = can.map { it.toFfi() },
)

private fun FfiPlayer.toDomain(receivedAt: Long) = MediaPlayer(
    player = player,
    name = name,
    state = PlaybackState.entries.first { it.toFfi() == state },
    title = title,
    artist = artist,
    album = album,
    lengthMs = lengthMs?.toLong(),
    positionMs = positionMs.toLong(),
    volume = volume?.toInt(),
    artwork = artwork,
    can = can.map { it.toDomain() }.toSet(),
    receivedAt = receivedAt,
)

private fun Feature.toFfi(): FfiFeature = when (this) {
    Feature.Clipboard -> FfiFeature.CLIPBOARD
    Feature.Files -> FfiFeature.FILES
    Feature.Notifications -> FfiFeature.NOTIFICATIONS
    Feature.Media -> FfiFeature.MEDIA
    Feature.Ring -> FfiFeature.RING
    Feature.Calls -> FfiFeature.CALLS
}

private fun PhoneNotification.toFfi() = FfiPhoneNotification(
    id = id,
    app = app,
    title = title,
    text = text,
    icon = icon,
    actions = actions.map { FfiNotificationButton(it.id, it.label, it.reply) },
)

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
