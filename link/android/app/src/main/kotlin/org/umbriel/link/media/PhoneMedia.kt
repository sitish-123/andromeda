package org.umbriel.link.media

import android.content.ComponentName
import android.content.Context
import android.graphics.Bitmap
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState as AndroidPlaybackState
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import java.io.ByteArrayOutputStream
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.core.domain.MediaCommandKind
import org.umbriel.link.core.domain.MediaLimits
import org.umbriel.link.core.domain.MediaPlayer
import org.umbriel.link.core.domain.PhoneMediaCommand
import org.umbriel.link.core.domain.PlaybackState
import org.umbriel.link.core.domain.truncateUtf8
import org.umbriel.link.notifications.MirrorService

/**
 * This phone's playing media for desktops: the session Android lists first, the one its own media controls show.
 * `getActiveSessions` is allowed to a notification listener, so this works only while notification access is granted
 * and is attached from [MirrorService]. Desktops' commands go to that session's transport controls.
 */
class PhoneMedia(
    private val context: Context,
    private val repository: LinkRepository,
    private val scope: CoroutineScope,
) {
    private val sessions = context.getSystemService(MediaSessionManager::class.java)
    private val listener = ComponentName(context, MirrorService::class.java)
    private val main = Handler(Looper.getMainLooper())
    private var controller: MediaController? = null
    /** The last description desktops received, with the elapsed-realtime moment its position was true. */
    private var sent: MediaPlayer? = null
    private var artwork: Pair<Bitmap, ByteArray?>? = null

    private val changed = MediaSessionManager.OnActiveSessionsChangedListener { follow(it.orEmpty().firstOrNull()) }

    private val callback = object : MediaController.Callback() {
        override fun onPlaybackStateChanged(state: AndroidPlaybackState?) = update()
        override fun onMetadataChanged(metadata: MediaMetadata?) = update()
        override fun onAudioInfoChanged(info: MediaController.PlaybackInfo) = update()
        override fun onSessionDestroyed() = follow(null)
    }

    fun start() {
        scope.launch { repository.connections.collect { sent?.let { publish(it.copy(positionMs = position(it))) } } }
        scope.launch { repository.mediaCommands.collect(::obey) }
    }

    fun attach() {
        runCatching {
            sessions.addOnActiveSessionsChangedListener(changed, listener, main)
            follow(sessions.getActiveSessions(listener).firstOrNull())
        }.onFailure { Log.w(TAG, "media sessions: ${it.message}") }
    }

    fun detach() {
        runCatching { sessions.removeOnActiveSessionsChangedListener(changed) }
        follow(null)
    }

    private fun follow(next: MediaController?) {
        if (next?.sessionToken == controller?.sessionToken) return
        controller?.unregisterCallback(callback)
        controller = next
        next?.registerCallback(callback, main)
        if (next == null) {
            sent?.let { gone -> scope.launch { repository.playerGone(gone.player) } }
            sent = null
            return
        }
        update()
    }

    private fun update() {
        val player = describe(controller ?: return) ?: return
        val previous = sent
        if (previous != null && previous.player != player.player) scope.launch { repository.playerGone(previous.player) }
        if (previous != null && previous.player == player.player && !differs(previous, player)) return
        publish(player)
    }

    /** A new description is worth sending when anything but the steady advance of the position changed. */
    private fun differs(old: MediaPlayer, new: MediaPlayer): Boolean =
        old.copy(positionMs = 0, receivedAt = 0) != new.copy(positionMs = 0, receivedAt = 0) ||
            abs(position(old, new.receivedAt) - new.positionMs) > SEEK_TOLERANCE_MS

    private fun publish(player: MediaPlayer) {
        sent = player
        scope.launch { repository.publishPlayer(player).onFailure { Log.w(TAG, "publishing: ${it.message}") } }
    }

    private fun position(player: MediaPlayer, at: Long = SystemClock.elapsedRealtime()): Long {
        val advanced = if (player.state == PlaybackState.Playing) player.positionMs + (at - player.receivedAt) else player.positionMs
        return player.lengthMs?.let { advanced.coerceAtMost(it) } ?: advanced
    }

    private fun describe(controller: MediaController): MediaPlayer? {
        val playback = controller.playbackState ?: return null
        val metadata = controller.metadata
        val now = SystemClock.elapsedRealtime()
        val state = when (playback.state) {
            AndroidPlaybackState.STATE_PLAYING, AndroidPlaybackState.STATE_BUFFERING -> PlaybackState.Playing
            AndroidPlaybackState.STATE_PAUSED -> PlaybackState.Paused
            else -> PlaybackState.Stopped
        }
        val position = if (state == PlaybackState.Playing && playback.lastPositionUpdateTime > 0) {
            playback.position + ((now - playback.lastPositionUpdateTime) * playback.playbackSpeed).toLong()
        } else {
            playback.position
        }
        val info = controller.playbackInfo
        val volume = info.takeIf { it.volumeControl != android.media.VolumeProvider.VOLUME_CONTROL_FIXED && it.maxVolume > 0 }
            ?.let { (it.currentVolume * 100f / it.maxVolume).roundToInt() }
        val label = runCatching {
            context.packageManager.getApplicationLabel(context.packageManager.getApplicationInfo(controller.packageName, 0))
        }.getOrNull()?.toString() ?: controller.packageName
        val text = { key: String -> metadata?.getString(key).orEmpty().truncateUtf8(MediaLimits.METADATA) }
        return MediaPlayer(
            player = controller.packageName.truncateUtf8(MediaLimits.PLAYER),
            name = label.ifBlank { controller.packageName }.truncateUtf8(MediaLimits.PLAYER),
            state = state,
            title = text(MediaMetadata.METADATA_KEY_TITLE),
            artist = text(MediaMetadata.METADATA_KEY_ARTIST),
            album = text(MediaMetadata.METADATA_KEY_ALBUM),
            lengthMs = metadata?.getLong(MediaMetadata.METADATA_KEY_DURATION)?.takeIf { it > 0 },
            positionMs = position.coerceAtLeast(0),
            volume = volume,
            artwork = art(metadata),
            can = commands(playback.actions, volume != null),
            receivedAt = now,
        )
    }

    private fun commands(actions: Long, volume: Boolean): Set<MediaCommandKind> = buildSet {
        val has = { action: Long -> actions and action != 0L }
        if (has(AndroidPlaybackState.ACTION_PLAY)) add(MediaCommandKind.Play)
        if (has(AndroidPlaybackState.ACTION_PAUSE)) add(MediaCommandKind.Pause)
        if (has(AndroidPlaybackState.ACTION_PLAY_PAUSE) || has(AndroidPlaybackState.ACTION_PLAY) ||
            has(AndroidPlaybackState.ACTION_PAUSE)
        ) {
            add(MediaCommandKind.PlayPause)
        }
        if (has(AndroidPlaybackState.ACTION_SKIP_TO_NEXT)) add(MediaCommandKind.Next)
        if (has(AndroidPlaybackState.ACTION_SKIP_TO_PREVIOUS)) add(MediaCommandKind.Previous)
        if (has(AndroidPlaybackState.ACTION_SEEK_TO)) add(MediaCommandKind.Seek)
        if (volume) add(MediaCommandKind.Volume)
    }

    /** The artwork as a JPEG of at most 256 px and the protocol's size; compressed once per bitmap. */
    private fun art(metadata: MediaMetadata?): ByteArray? {
        val bitmap = metadata?.getBitmap(MediaMetadata.METADATA_KEY_ART)
            ?: metadata?.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
            ?: return null
        artwork?.takeIf { it.first === bitmap }?.let { return it.second }
        val scale = ART_PX.toFloat() / maxOf(bitmap.width, bitmap.height)
        val sized = if (scale < 1f) {
            Bitmap.createScaledBitmap(bitmap, (bitmap.width * scale).roundToInt(), (bitmap.height * scale).roundToInt(), true)
        } else {
            bitmap
        }
        val jpeg = generateSequence(85) { it - 15 }.takeWhile { it > 0 }.map { quality ->
            ByteArrayOutputStream().also { sized.compress(Bitmap.CompressFormat.JPEG, quality, it) }.toByteArray()
        }.firstOrNull { it.size <= MediaLimits.ARTWORK }
        artwork = bitmap to jpeg
        return jpeg
    }

    private fun obey(command: PhoneMediaCommand) {
        val target = controller ?: return
        if (sent?.player != command.player || sent?.can?.contains(command.command) != true) return
        val controls = target.transportControls
        val value = command.value ?: 0
        when (command.command) {
            MediaCommandKind.Play -> controls.play()
            MediaCommandKind.Pause -> controls.pause()
            MediaCommandKind.PlayPause ->
                if (target.playbackState?.state == AndroidPlaybackState.STATE_PLAYING) controls.pause() else controls.play()
            MediaCommandKind.Next -> controls.skipToNext()
            MediaCommandKind.Previous -> controls.skipToPrevious()
            MediaCommandKind.Seek -> controls.seekTo(value)
            MediaCommandKind.Volume -> {
                val max = target.playbackInfo.maxVolume
                target.setVolumeTo((value * max / 100f).roundToInt(), 0)
            }
        }
    }

    private companion object {
        const val TAG = "PhoneMedia"
        const val ART_PX = 256
        const val SEEK_TOLERANCE_MS = 2000
    }
}
