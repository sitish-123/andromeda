package org.umbriel.link.transfer

import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import org.umbriel.link.R
import org.umbriel.link.notifications.Channels

/**
 * Runs while a file transfer is open, since Android freezes a cached process and its sockets with it. It holds no
 * state; the per-transfer notifications carry the progress.
 */
class TransferService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = NotificationCompat.Builder(this, Channels.TRANSFERS)
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setContentTitle(getString(R.string.transfer_progress))
            .setSilent(true)
            .setOngoing(true)
            .build()
        ServiceCompat.startForeground(this, NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        return START_NOT_STICKY
    }

    private companion object {
        const val NOTIFICATION_ID = 2
    }
}
