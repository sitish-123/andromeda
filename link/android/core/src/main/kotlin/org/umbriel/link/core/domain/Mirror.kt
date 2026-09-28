package org.umbriel.link.core.domain

/** What a desktop asks of this phone's screen while mirroring. Coordinates are fractions of the screen in units of
 * 1/[MIRROR_UNIT]; a scroll's (x2, y2) is a signed delta. */
sealed interface MirrorCommand {
    val desktopId: String

    /** Show Android's capture prompt; start on consent, or refuse. */
    data class Requested(override val desktopId: String) : MirrorCommand

    data class Input(
        override val desktopId: String,
        val action: MirrorAction,
        val x: Int?,
        val y: Int?,
        val x2: Int?,
        val y2: Int?,
        val ms: Int?,
        val text: String?,
    ) : MirrorCommand

    /** The desktop lost a frame: encode a keyframe next. */
    data class Keyframe(override val desktopId: String) : MirrorCommand

    data class Stopped(override val desktopId: String, val reason: String?) : MirrorCommand
}

enum class MirrorAction { Tap, LongPress, Swipe, Scroll, Back, Home, Recents, Text }

const val MIRROR_UNIT = 10_000

/** The video stream to one desktop, from [org.umbriel.link.core.data.LinkRepository.startMirror]. */
interface MirrorStream {
    /** One H.264 access unit, Annex B; suspends while the network is behind. */
    suspend fun send(ptsUs: Long, keyframe: Boolean, config: Boolean, data: ByteArray): Result<Unit>

    suspend fun finish()
}
