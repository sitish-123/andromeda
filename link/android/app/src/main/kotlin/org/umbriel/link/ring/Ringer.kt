package org.umbriel.link.ring

import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.RingtoneManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.umbriel.link.LinkApplication
import org.umbriel.link.R
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.notifications.Channels

/**
 * Find my phone: rings on the alarm stream at full volume, through silent mode, until stopped here or by the desktop,
 * or for 2 minutes. Do Not Disturb lets alarms through unless the user blocked them; before Android 15 DND access also
 * lifts it for the ring. What it changed is put back.
 */
class Ringer(private val context: Context, private val repository: LinkRepository, private val scope: CoroutineScope) {
    private val audio = context.getSystemService(AudioManager::class.java)
    private val notifications = context.getSystemService(NotificationManager::class.java)
    private val main = Handler(Looper.getMainLooper())
    private var player: MediaPlayer? = null
    private var restoreVolume: Int? = null
    private var restoreFilter: Int? = null
    private val _ringing = MutableStateFlow(false)

    val ringing: StateFlow<Boolean> = _ringing.asStateFlow()

    /** Whether the ring can override Do Not Disturb. */
    fun dndGranted(): Boolean = notifications.isNotificationPolicyAccessGranted

    fun dndSettings(): Intent = Intent(Settings.ACTION_NOTIFICATION_POLICY_ACCESS_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

    fun start() {
        scope.launch { repository.ringRequests.collect { (_, on) -> if (on) ring() else stop() } }
    }

    fun ring() {
        if (player == null) {
            begin()
        }
        report()
    }

    fun stop() {
        player?.let { end(it) }
        report()
    }

    private fun begin() {
        // From Android 15 an app's filter only drives its own zen rule and cannot lift the user's Do Not Disturb.
        val canLift = Build.VERSION.SDK_INT < Build.VERSION_CODES.VANILLA_ICE_CREAM && dndGranted()
        if (canLift && notifications.currentInterruptionFilter != NotificationManager.INTERRUPTION_FILTER_ALL) {
            restoreFilter = notifications.currentInterruptionFilter
            notifications.setInterruptionFilter(NotificationManager.INTERRUPTION_FILTER_ALL)
        }
        restoreVolume = audio.getStreamVolume(AudioManager.STREAM_ALARM)
        audio.setStreamVolume(AudioManager.STREAM_ALARM, audio.getStreamMaxVolume(AudioManager.STREAM_ALARM), 0)
        val sound = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM)
            ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE)
        player = runCatching {
            MediaPlayer().apply {
                setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_ALARM)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build(),
                )
                setDataSource(context, sound)
                isLooping = true
                prepare()
                start()
            }
        }.onFailure { Log.w(TAG, "ringing: ${it.message}") }.getOrNull()
        _ringing.value = true
        main.postDelayed(::stop, LIMIT_MS)
        showStop()
    }

    private fun end(playing: MediaPlayer) {
        main.removeCallbacksAndMessages(null)
        runCatching { playing.stop() }
        playing.release()
        player = null
        restoreVolume?.let { audio.setStreamVolume(AudioManager.STREAM_ALARM, it, 0) }
        restoreVolume = null
        restoreFilter?.let { if (dndGranted()) notifications.setInterruptionFilter(it) }
        restoreFilter = null
        _ringing.value = false
        NotificationManagerCompat.from(context).cancel(NOTIFICATION_ID)
    }

    private fun report() {
        val on = player != null
        scope.launch { repository.reportRinging(on) }
    }

    private fun showStop() {
        val stop = PendingIntent.getBroadcast(
            context,
            0,
            Intent(context, StopRingReceiver::class.java),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val notification = NotificationCompat.Builder(context, Channels.RING)
            .setSmallIcon(android.R.drawable.ic_lock_idle_alarm)
            .setContentTitle(context.getString(R.string.ring_title))
            .setContentText(context.getString(R.string.ring_text))
            .setCategory(NotificationCompat.CATEGORY_ALARM)
            .setPriority(NotificationCompat.PRIORITY_MAX)
            .setOngoing(true)
            .setContentIntent(stop)
            .addAction(0, context.getString(R.string.ring_stop), stop)
            .build()
        runCatching { NotificationManagerCompat.from(context).notify(NOTIFICATION_ID, notification) }
    }

    private companion object {
        const val TAG = "Ringer"
        const val NOTIFICATION_ID = 2
        const val LIMIT_MS = 2 * 60 * 1000L
    }
}

/** The Stop action of the ringing notification. */
class StopRingReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        (context.applicationContext as LinkApplication).container.ringer.stop()
    }
}
