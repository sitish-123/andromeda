package org.umbriel.link.notifications

import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import org.umbriel.link.R

object Channels {
    const val PRESENCE = "presence"
    const val SHARES = "shares"
    const val TRANSFERS = "transfers"
    const val RING = "ring"
    const val SCREEN = "screen"

    fun create(context: Context) {
        val manager = context.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(PRESENCE, context.getString(R.string.channel_presence), NotificationManager.IMPORTANCE_LOW),
        )
        manager.createNotificationChannel(
            NotificationChannel(SHARES, context.getString(R.string.channel_shares), NotificationManager.IMPORTANCE_HIGH),
        )
        manager.createNotificationChannel(
            NotificationChannel(TRANSFERS, context.getString(R.string.channel_transfers), NotificationManager.IMPORTANCE_HIGH),
        )
        manager.createNotificationChannel(
            NotificationChannel(SCREEN, context.getString(R.string.channel_screen), NotificationManager.IMPORTANCE_HIGH),
        )
        // Silent itself: the ring plays on the alarm stream, and the channel only carries Stop.
        manager.createNotificationChannel(
            NotificationChannel(RING, context.getString(R.string.channel_ring), NotificationManager.IMPORTANCE_HIGH)
                .apply { setSound(null, null) },
        )
    }
}
