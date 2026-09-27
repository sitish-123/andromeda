package org.umbriel.link.notifications

import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import org.umbriel.link.R

object Channels {
    const val PRESENCE = "presence"
    const val SHARES = "shares"
    const val TRANSFERS = "transfers"

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
    }
}
