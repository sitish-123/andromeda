package org.umbriel.link.core.domain

enum class PlaybackState { Playing, Paused, Stopped }

enum class MediaCommandKind { Play, Pause, PlayPause, Next, Previous, Seek, Volume }

/** A player on either side; limits in `link/ARCHITECTURE.md`, checked by the Rust core. */
data class MediaPlayer(
    val player: String,
    val name: String,
    val state: PlaybackState,
    val title: String,
    val artist: String,
    val album: String,
    val lengthMs: Long?,
    /** When [receivedAt] (elapsed realtime ms); advance it while playing. */
    val positionMs: Long,
    val volume: Int?,
    /** PNG or JPEG. */
    val artwork: ByteArray?,
    val can: Set<MediaCommandKind>,
    val receivedAt: Long = 0,
) {
    override fun equals(other: Any?): Boolean = other is MediaPlayer && player == other.player &&
        name == other.name && state == other.state && title == other.title && artist == other.artist &&
        album == other.album && lengthMs == other.lengthMs && positionMs == other.positionMs &&
        volume == other.volume && can == other.can && receivedAt == other.receivedAt &&
        artwork.contentEquals(other.artwork)

    override fun hashCode(): Int = player.hashCode() * 31 + title.hashCode()
}

/** A desktop's player as this phone shows it. */
data class DesktopPlayer(val desktopId: String, val player: MediaPlayer)

/** What a desktop asks of this phone's player. */
data class PhoneMediaCommand(val desktopId: String, val player: String, val command: MediaCommandKind, val value: Long?)

object MediaLimits {
    const val PLAYER = 64
    const val METADATA = 512
    const val ARTWORK = 48 * 1024
}
