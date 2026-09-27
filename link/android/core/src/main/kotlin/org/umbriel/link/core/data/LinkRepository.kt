package org.umbriel.link.core.data

import android.content.Context
import android.net.wifi.WifiManager
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
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.core.domain.DesktopPlayer
import org.umbriel.link.core.domain.MediaCommandKind
import org.umbriel.link.core.domain.MediaPlayer
import org.umbriel.link.core.domain.PhoneMediaCommand
import org.umbriel.link.core.domain.PlaybackState
import org.umbriel.link.core.domain.Feature
import org.umbriel.link.core.domain.IncomingShare
import org.umbriel.link.core.domain.LinkFailure
import org.umbriel.link.core.domain.LinkFailureException
import org.umbriel.link.core.domain.NotificationCommand
import org.umbriel.link.core.domain.PhoneNotification
import org.umbriel.link.core.domain.ShareKind
import org.umbriel.link.core.domain.Sharing
import org.umbriel.link.ffi.LinkClient
import org.umbriel.link.ffi.LinkEvent
import org.umbriel.link.ffi.LinkException
import org.umbriel.link.ffi.generateIdentity
import org.umbriel.link.ffi.Desktop as FfiDesktop
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
    private val _connections = MutableSharedFlow<String>(extraBufferCapacity = INCOMING_BUFFER)
    private val _notificationCommands = MutableSharedFlow<NotificationCommand>(extraBufferCapacity = INCOMING_BUFFER)
    private val _mediaCommands = MutableSharedFlow<PhoneMediaCommand>(extraBufferCapacity = INCOMING_BUFFER)
    private val _desktopPlayers = MutableStateFlow<List<DesktopPlayer>>(emptyList())

    val desktops: StateFlow<List<Desktop>> = _desktops.asStateFlow()

    /** Shares desktops sent, as they arrive. */
    val incoming: SharedFlow<IncomingShare> = _incoming.asSharedFlow()

    /** The id of each desktop as a session to it opens. */
    val connections: SharedFlow<String> = _connections.asSharedFlow()

    /** Actions and dismissals desktops ask of mirrored notifications. */
    val notificationCommands: SharedFlow<NotificationCommand> = _notificationCommands.asSharedFlow()

    /** Commands desktops send this phone's player. */
    val mediaCommands: SharedFlow<PhoneMediaCommand> = _mediaCommands.asSharedFlow()

    /** Every connected desktop's players; a desktop's go when its session ends. */
    val desktopPlayers: StateFlow<List<DesktopPlayer>> = _desktopPlayers.asStateFlow()

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
            LinkClient(identity, context.filesDir.resolve("devices.json").absolutePath, deviceName)
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
                is LinkEvent.PlayerCommand -> _mediaCommands.emit(
                    PhoneMediaCommand(event.desktopId, event.player, event.command.toDomain(), event.value?.toLong()),
                )
                is LinkEvent.Received -> _incoming.emit(
                    IncomingShare(event.desktopId, nameOf(event.desktopId), event.kind.toDomain(), event.text),
                )
                is LinkEvent.Unpaired -> scope.launch { refresh() }
                is LinkEvent.NotificationAction -> _notificationCommands.emit(
                    NotificationCommand.Action(event.desktopId, event.id, event.action, event.replyText),
                )
                is LinkEvent.NotificationDismissed ->
                    _notificationCommands.emit(NotificationCommand.Dismiss(event.desktopId, event.id))
            }
        }
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
    sharing = Sharing(sharing.notifications, sharing.media, sharing.ring, sharing.calls),
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
