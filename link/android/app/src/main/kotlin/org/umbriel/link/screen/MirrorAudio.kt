package org.umbriel.link.screen

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.MediaCodec
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.util.Log
import org.umbriel.link.core.domain.MirrorStream

/**
 * What apps play, captured through the screen's projection and encoded as 48 kHz stereo Opus, in 20 ms packets on
 * the video's stream. Needs the microphone permission, which Android requires for any playback capture; without it
 * the mirror is silent. Apps that opt out of capture stay silent too.
 */
class MirrorAudio(private val context: Context, private val projection: MediaProjection) {
    @Volatile
    private var stopping = false

    /** Captures and sends until [stop] or the stream fails; blocks, so it runs on an IO thread. */
    suspend fun run(stream: MirrorStream) {
        if (context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            Log.i(TAG, "no microphone permission: mirroring without sound")
            return
        }
        val record = record() ?: return
        val codec = try {
            MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS).apply {
                val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, RATE, CHANNELS)
                format.setInteger(MediaFormat.KEY_BIT_RATE, BITRATE)
                configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                start()
            }
        } catch (error: Exception) {
            Log.w(TAG, "no Opus encoder: ${error.message}")
            record.release()
            return
        }
        try {
            record.startRecording()
            encode(record, codec, stream)
        } finally {
            runCatching { record.stop() }
            record.release()
            runCatching { codec.stop() }
            codec.release()
        }
    }

    fun stop() {
        stopping = true
    }

    @SuppressLint("MissingPermission")
    private fun record(): AudioRecord? {
        val capture = AudioPlaybackCaptureConfiguration.Builder(projection)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .build()
        val format = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(RATE)
            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
            .build()
        val minimum = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        return try {
            AudioRecord.Builder()
                .setAudioFormat(format)
                .setBufferSizeInBytes(maxOf(minimum, PACKET_BYTES) * 4)
                .setAudioPlaybackCaptureConfig(capture)
                .build()
        } catch (error: Exception) {
            Log.w(TAG, "no playback capture: ${error.message}")
            null
        }
    }

    private suspend fun encode(record: AudioRecord, codec: MediaCodec, stream: MirrorStream) {
        val info = MediaCodec.BufferInfo()
        while (!stopping) {
            val input = codec.dequeueInputBuffer(TIMEOUT_US)
            if (input >= 0) {
                val buffer = codec.getInputBuffer(input) ?: continue
                val read = record.read(buffer, minOf(buffer.capacity(), PACKET_BYTES), AudioRecord.READ_BLOCKING)
                codec.queueInputBuffer(input, 0, maxOf(read, 0), System.nanoTime() / 1000, 0)
            }
            while (true) {
                val output = codec.dequeueOutputBuffer(info, 0)
                if (output < 0) break
                val bytes = ByteArray(info.size)
                codec.getOutputBuffer(output)?.apply {
                    position(info.offset)
                    get(bytes)
                }
                codec.releaseOutputBuffer(output, false)
                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0 || bytes.isEmpty()) continue
                if (stream.sendAudio(info.presentationTimeUs, bytes).isFailure) return
            }
        }
    }

    private companion object {
        const val TAG = "MirrorAudio"
        const val RATE = 48_000
        const val CHANNELS = 2
        const val BITRATE = 96_000
        // 20 ms of 16-bit stereo: one Opus packet.
        const val PACKET_BYTES = RATE / 50 * CHANNELS * 2
        const val TIMEOUT_US = 20_000L
    }
}
