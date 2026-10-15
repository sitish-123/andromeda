package org.umbriel.link.fixture

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Color
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Bundle
import android.util.Log
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import kotlin.math.PI
import kotlin.math.sin
import kotlin.random.Random

/**
 * For the mirror's latency and sound: the whole screen flips between black and white at each touch, logged under
 * `LinkFixture`, and a 440 Hz tone plays as media while it is open. Started again with `noise`, it draws random
 * blocks every frame instead, which no encoder compresses well.
 */
class MirrorProbeActivity : Activity() {
    private var white = false
    private var tone: AudioTrack? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val view = View(this).apply { setBackgroundColor(Color.BLACK) }
        view.setOnTouchListener { touched, event ->
            if (event.actionMasked == MotionEvent.ACTION_DOWN) {
                white = !white
                touched.setBackgroundColor(if (white) Color.WHITE else Color.BLACK)
                Log.i(TAG, "flip ${if (white) "white" else "black"}")
            }
            true
        }
        setContentView(view)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        if (intent.getBooleanExtra("noise", false)) setContentView(Noise(this))
    }

    private class Noise(context: Context) : View(context) {
        private val paint = Paint()

        override fun onDraw(canvas: Canvas) {
            val side = width / COLUMNS
            for (row in 0..height / side) {
                for (column in 0 until COLUMNS) {
                    paint.color = Color.rgb(Random.nextInt(256), Random.nextInt(256), Random.nextInt(256))
                    canvas.drawRect(
                        (column * side).toFloat(), (row * side).toFloat(),
                        ((column + 1) * side).toFloat(), ((row + 1) * side).toFloat(), paint,
                    )
                }
            }
            postInvalidateOnAnimation()
        }
    }

    override fun onResume() {
        super.onResume()
        tone = toneTrack().apply { play() }
    }

    override fun onPause() {
        tone?.release()
        tone = null
        super.onPause()
    }

    private fun toneTrack(): AudioTrack {
        val samples = ShortArray(RATE) { i -> (sin(2 * PI * HZ * i / RATE) * AMPLITUDE).toInt().toShort() }
        val track = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build())
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build(),
            )
            .setTransferMode(AudioTrack.MODE_STATIC)
            .setBufferSizeInBytes(samples.size * 2)
            .build()
        track.write(samples, 0, samples.size)
        track.setLoopPoints(0, samples.size, -1)
        return track
    }

    private companion object {
        const val TAG = "LinkFixture"
        const val RATE = 48_000
        const val HZ = 440
        const val AMPLITUDE = 12_000
        const val COLUMNS = 48
    }
}
