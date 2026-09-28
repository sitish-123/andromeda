package org.umbriel.link.presence

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat

/** Restarts [PresenceService] after a reboot or an app update when "Stay connected" is on. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action !in ACTIONS || !Presence.isStayConnected(context)) return
        ContextCompat.startForegroundService(context, Intent(context, PresenceService::class.java))
    }

    private companion object {
        val ACTIONS = setOf(Intent.ACTION_BOOT_COMPLETED, Intent.ACTION_MY_PACKAGE_REPLACED)
    }
}
