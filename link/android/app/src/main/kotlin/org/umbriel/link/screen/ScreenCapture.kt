package org.umbriel.link.screen

import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import android.view.WindowManager
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.IntentCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import org.umbriel.link.LinkApplication
import org.umbriel.link.R
import org.umbriel.link.core.domain.MirrorStream
import org.umbriel.link.notifications.Channels

/**
 * Captures the screen into the hardware H.264 encoder and streams its output to one desktop: at most 1080 pixels on
 * the short side, 60 fps, realtime priority, a keyframe every 2 s or on the desktop's request. Codec configuration
 * goes in front of every keyframe, so the desktop can start from any of them.
 */
class ScreenCapture : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val main = Handler(Looper.getMainLooper())
    private var projection: MediaProjection? = null
    private var display: VirtualDisplay? = null
    private var codec: MediaCodec? = null
    private var stream: MirrorStream? = null
    private var desktop: String? = null

    @Volatile
    private var stopping = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            finish(tellDesktop = true, reason = "stopped on the phone")
            return START_NOT_STICKY
        }
        val desktop = intent?.getStringExtra(ScreenMirror.EXTRA_DESKTOP)
        val data = intent?.let { IntentCompat.getParcelableExtra(it, EXTRA_DATA, Intent::class.java) }
        if (desktop == null || data == null || running) {
            stopSelf()
            return START_NOT_STICKY
        }
        this.desktop = desktop
        val repository = (application as LinkApplication).container.repository
        val name = repository.desktops.value.firstOrNull { it.id == desktop }?.name ?: desktop
        // Android requires the foreground service before it hands out the projection.
        ServiceCompat.startForeground(this, NOTIFICATION_ID, notification(name), ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        instance = this
        running = true
        val projection = getSystemService(MediaProjectionManager::class.java)
            .getMediaProjection(intent.getIntExtra(EXTRA_CODE, 0), data)
        if (projection == null) {
            finish(tellDesktop = true, reason = "Android refused the capture")
            return START_NOT_STICKY
        }
        projection.registerCallback(
            object : MediaProjection.Callback() {
                override fun onStop() = finish(tellDesktop = true, reason = "stopped on the phone")
            },
            main,
        )
        this.projection = projection
        scope.launch { capture(desktop, projection) }
        return START_NOT_STICKY
    }

    private suspend fun capture(desktop: String, projection: MediaProjection) {
        val (width, height) = videoSize()
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, BITRATE)
            setInteger(MediaFormat.KEY_FRAME_RATE, FPS)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, KEYFRAME_EVERY_S)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            setInteger(MediaFormat.KEY_LATENCY, 1)
        }
        val codec = try {
            MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC).apply {
                configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            }
        } catch (error: Exception) {
            Log.w(TAG, "no H.264 encoder: ${error.message}")
            return finish(tellDesktop = true, reason = "the phone has no H.264 encoder for ${width}x$height")
        }
        val surface = codec.createInputSurface()
        codec.start()
        this.codec = codec
        display = projection.createVirtualDisplay(
            "Link mirror", width, height, resources.displayMetrics.densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, surface, null, null,
        )
        val repository = (application as LinkApplication).container.repository
        val stream = repository.startMirror(desktop, width, height).getOrElse {
            return finish(tellDesktop = false, reason = null)
        }
        this.stream = stream
        drain(codec, stream)
    }

    private suspend fun drain(codec: MediaCodec, stream: MirrorStream) {
        val info = MediaCodec.BufferInfo()
        var config: ByteArray? = null
        while (!stopping) {
            val index = try {
                codec.dequeueOutputBuffer(info, DEQUEUE_TIMEOUT_US)
            } catch (_: IllegalStateException) {
                break
            }
            if (index < 0) continue
            val bytes = ByteArray(info.size)
            codec.getOutputBuffer(index)?.apply {
                position(info.offset)
                get(bytes)
            }
            codec.releaseOutputBuffer(index, false)
            if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                config = bytes
                continue
            }
            val keyframe = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
            val prefix = config.takeIf { keyframe }
            val unit = if (prefix != null) prefix + bytes else bytes
            if (stream.send(info.presentationTimeUs, keyframe, prefix != null, unit).isFailure) {
                main.post { finish(tellDesktop = false, reason = null) }
                break
            }
        }
    }

    /** The screen scaled to at most [MAX_SHORT_SIDE] on its short side, in multiples of 16 as encoders want. */
    private fun videoSize(): Pair<Int, Int> {
        val (width, height) = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getSystemService(WindowManager::class.java).maximumWindowMetrics.bounds.let { it.width() to it.height() }
        } else {
            resources.displayMetrics.let { it.widthPixels to it.heightPixels }
        }
        val scale = minOf(1.0, MAX_SHORT_SIDE.toDouble() / minOf(width, height))
        fun fit(side: Int) = (side * scale).toInt() / ALIGN * ALIGN
        return fit(width) to fit(height)
    }

    private fun keyframe() {
        try {
            codec?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        } catch (error: IllegalStateException) {
            Log.i(TAG, "keyframe: ${error.message}")
        }
    }

    private fun finish(tellDesktop: Boolean, reason: String?) {
        if (stopping) return
        stopping = true
        display?.release()
        runCatching { codec?.stop() }
        runCatching { codec?.release() }
        projection?.stop()
        val (desktop, stream) = desktop to stream
        val repository = (application as LinkApplication).container.repository
        scope.launch {
            stream?.finish()
            if (tellDesktop && desktop != null) repository.stopMirror(desktop, reason)
            scope.cancel()
        }
        running = false
        instance = null
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        finish(tellDesktop = true, reason = "stopped on the phone")
        super.onDestroy()
    }

    private fun notification(name: String) = NotificationCompat.Builder(this, Channels.SCREEN)
        .setSmallIcon(R.drawable.ic_stat_link)
        .setContentTitle(getString(R.string.screen_mirroring, name))
        .setOngoing(true)
        .setSilent(true)
        .addAction(
            0,
            getString(R.string.screen_stop),
            PendingIntent.getService(
                this, 0, Intent(this, ScreenCapture::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE,
            ),
        )
        .build()

    companion object {
        @Volatile
        var running = false
            private set

        @Volatile
        private var instance: ScreenCapture? = null

        fun start(context: Context, desktop: String, code: Int, data: Intent) {
            val intent = Intent(context, ScreenCapture::class.java)
                .putExtra(ScreenMirror.EXTRA_DESKTOP, desktop)
                .putExtra(EXTRA_CODE, code)
                .putExtra(EXTRA_DATA, data)
            context.startForegroundService(intent)
        }

        fun requestKeyframe() {
            instance?.keyframe()
        }

        /** The desktop stopped; nothing is sent back to it. */
        fun stop(context: Context, desktop: String) {
            val capture = instance ?: return
            if (capture.desktop == desktop) Handler(context.mainLooper).post { capture.finish(tellDesktop = false, reason = null) }
        }

        private const val TAG = "ScreenCapture"
        private const val ACTION_STOP = "org.umbriel.link.screen.STOP"
        private const val EXTRA_CODE = "code"
        private const val EXTRA_DATA = "data"
        private const val NOTIFICATION_ID = 41
        private const val MAX_SHORT_SIDE = 1080
        private const val ALIGN = 16
        private const val BITRATE = 8_000_000
        private const val FPS = 60
        private const val KEYFRAME_EVERY_S = 2
        private const val DEQUEUE_TIMEOUT_US = 20_000L
    }
}
