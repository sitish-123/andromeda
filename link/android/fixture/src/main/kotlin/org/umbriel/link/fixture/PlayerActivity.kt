package org.umbriel.link.fixture

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.Color
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.Bundle
import android.os.SystemClock
import android.util.Log

/** Plays "Emulator Song" on a MediaSession and logs each command it obeys under the tag `LinkFixture`. */
class PlayerActivity : Activity() {
    private lateinit var session: MediaSession
    private var playing = true
    private var position = 30_000L
    private var title = "Emulator Song"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        session = MediaSession(this, "link-fixture")
        session.setCallback(object : MediaSession.Callback() {
            override fun onPlay() = obey("play") { playing = true }
            override fun onPause() = obey("pause") { playing = false }
            override fun onSkipToNext() = obey("next") {
                title = "Emulator Song, part 2"
                position = 0
                publishMetadata()
            }
            override fun onSkipToPrevious() = obey("previous") {}
            override fun onSeekTo(pos: Long) = obey("seek $pos") { position = pos }
        })
        publishMetadata()
        publishState()
        session.isActive = true
    }

    override fun onDestroy() {
        session.release()
        super.onDestroy()
    }

    private fun obey(command: String, change: () -> Unit) {
        Log.i(TAG, command)
        change()
        publishState()
    }

    private fun publishMetadata() {
        val art = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888).apply { eraseColor(Color.rgb(0, 215, 255)) }
        session.setMetadata(
            MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, title)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, "The Emulators")
                .putString(MediaMetadata.METADATA_KEY_ALBUM, "Link E2E")
                .putLong(MediaMetadata.METADATA_KEY_DURATION, 180_000)
                .putBitmap(MediaMetadata.METADATA_KEY_ART, art)
                .build(),
        )
    }

    private fun publishState() {
        val actions = PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PAUSE or PlaybackState.ACTION_PLAY_PAUSE or
            PlaybackState.ACTION_SKIP_TO_NEXT or PlaybackState.ACTION_SKIP_TO_PREVIOUS or PlaybackState.ACTION_SEEK_TO
        val state = if (playing) PlaybackState.STATE_PLAYING else PlaybackState.STATE_PAUSED
        session.setPlaybackState(
            PlaybackState.Builder()
                .setActions(actions)
                .setState(state, position, if (playing) 1f else 0f, SystemClock.elapsedRealtime())
                .build(),
        )
    }

    private companion object {
        const val TAG = "LinkFixture"
    }
}
