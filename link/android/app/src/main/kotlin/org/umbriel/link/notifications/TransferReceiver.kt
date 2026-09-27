package org.umbriel.link.notifications

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import androidx.core.app.NotificationManagerCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import org.umbriel.link.LinkApplication

/** Accept, Decline, and Cancel on a transfer notification. */
class TransferReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val transferId = intent.getStringExtra(EXTRA_TRANSFER) ?: return
        val repository = (context.applicationContext as LinkApplication).container.repository
        if (intent.action == DECLINE) {
            NotificationManagerCompat.from(context).cancel(TransferNotifier.TAG, transferId.hashCode())
        }
        val pending = goAsync()
        CoroutineScope(Dispatchers.Default).launch {
            when (intent.action) {
                ACCEPT -> repository.acceptTransfer(transferId)
                DECLINE -> repository.declineTransfer(transferId)
                CANCEL -> repository.cancelTransfer(transferId)
            }
            pending.finish()
        }
    }

    companion object {
        const val ACCEPT = "org.umbriel.link.transfer.ACCEPT"
        const val DECLINE = "org.umbriel.link.transfer.DECLINE"
        const val CANCEL = "org.umbriel.link.transfer.CANCEL"
        const val EXTRA_TRANSFER = "transfer"
    }
}
