package org.umbriel.link.clipboard

import android.app.PendingIntent
import android.content.Intent
import android.os.Build
import android.service.quicksettings.TileService

/** "Send clipboard" in quick settings: one tap opens [ClipboardReadActivity], which reads and sends while focused. */
class ClipboardTileService : TileService() {
    override fun onClick() {
        val intent = Intent(this, ClipboardReadActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startActivityAndCollapse(PendingIntent.getActivity(this, 0, intent, PendingIntent.FLAG_IMMUTABLE))
        } else {
            @Suppress("DEPRECATION")
            startActivityAndCollapse(intent)
        }
    }
}
