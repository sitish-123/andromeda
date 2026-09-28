package org.umbriel.link.presence

import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import org.umbriel.link.LinkApplication
import org.umbriel.link.MainActivity
import org.umbriel.link.R
import org.umbriel.link.notifications.Channels

/**
 * Runs only while "Stay connected" is on, so its notification exists only then. It holds no state: [Presence] keeps
 * the phone present, and this service only lets that continue in the background.
 */
class PresenceService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val notification = NotificationCompat.Builder(this, Channels.PRESENCE)
            .setSmallIcon(R.drawable.ic_stat_link)
            .setContentTitle(getString(R.string.presence_title))
            .setContentText(getString(R.string.presence_text))
            .setContentIntent(open)
            .setOngoing(true)
            // Its own group, so Android does not bundle it with incoming shares and hide their actions.
            .setGroup(GROUP)
            .setSilent(true)
            .build()
        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            notification,
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
        )
        (application as LinkApplication).container.clipboardWatcher.start()
        return START_STICKY
    }

    override fun onDestroy() {
        (application as LinkApplication).container.clipboardWatcher.stop()
        super.onDestroy()
    }

    private companion object {
        const val NOTIFICATION_ID = 1
        const val GROUP = "presence"
    }
}
