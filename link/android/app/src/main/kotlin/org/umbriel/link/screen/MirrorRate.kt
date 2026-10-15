package org.umbriel.link.screen

/**
 * The encoder's bitrate, stepped by how far the sent video falls behind capture. A send suspends while the
 * connection is behind, so loss or a slow link shows up as lag: the time from capture to queued, minus the
 * smallest such time seen, which cancels the clocks' offset.
 */
class MirrorRate(private val max: Int, private val min: Int) {
    var bitrate = max
        private set

    /** True while the stream skips frames until the next keyframe, after falling far behind. */
    var skipping = false
        private set

    private var floorUs = Long.MAX_VALUE
    private var windowStartUs = 0L
    private var windowWorstUs = 0L
    private var lastChangeUs = 0L
    private var calmSinceUs = 0L

    /**
     * One unit queued at [nowUs] that was captured at [ptsUs]; returns the new bitrate when it changes. A keyframe
     * ends skipping.
     */
    fun sent(ptsUs: Long, nowUs: Long, keyframe: Boolean): Int? {
        if (keyframe) skipping = false
        val transit = nowUs - ptsUs
        floorUs = minOf(floorUs, transit)
        val lag = transit - floorUs
        if (lag > SKIP_LAG_US) skipping = true
        if (windowStartUs == 0L) {
            windowStartUs = nowUs
            calmSinceUs = nowUs
        }
        windowWorstUs = maxOf(windowWorstUs, lag)
        if (nowUs - windowStartUs < WINDOW_US) return null
        val worst = windowWorstUs
        windowStartUs = nowUs
        windowWorstUs = 0
        if (worst > CALM_LAG_US) calmSinceUs = nowUs
        val next = when {
            worst > BEHIND_LAG_US && nowUs - lastChangeUs > HOLD_US -> maxOf(min, bitrate * DOWN_PERCENT / 100)
            nowUs - calmSinceUs > CALM_US && nowUs - lastChangeUs > CALM_US -> minOf(max, bitrate * UP_PERCENT / 100)
            else -> return null
        }
        if (next == bitrate) return null
        bitrate = next
        lastChangeUs = nowUs
        return next
    }

    /** While skipping, every unit but a keyframe is dropped. */
    fun shouldSkip(keyframe: Boolean): Boolean = skipping && !keyframe

    private companion object {
        const val WINDOW_US = 500_000L
        const val BEHIND_LAG_US = 120_000L
        const val CALM_LAG_US = 40_000L
        const val SKIP_LAG_US = 1_000_000L
        const val HOLD_US = 1_000_000L
        const val CALM_US = 5_000_000L
        const val DOWN_PERCENT = 60
        const val UP_PERCENT = 125
    }
}
