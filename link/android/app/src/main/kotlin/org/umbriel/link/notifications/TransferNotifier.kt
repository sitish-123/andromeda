package org.umbriel.link.notifications

import android.Manifest
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.text.format.Formatter
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import org.umbriel.link.R
import org.umbriel.link.core.domain.TransferEvent
import org.umbriel.link.transfer.OfferActivity

/**
 * One notification per transfer, replaced as it moves on: the offer with Accept and Decline, then progress with
 * Cancel, then the result, which opens the received file.
 */
class TransferNotifier(private val context: Context) {

    fun post(event: TransferEvent) {
        if (!allowed()) return
        val id = event.transferId.hashCode()
        // A group of its own, so Android never folds an offer into a bundle that hides its Accept button.
        val builder = NotificationCompat.Builder(context, Channels.TRANSFERS)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setGroup(TAG + event.transferId)
        when (event) {
            is TransferEvent.Offered -> offer(builder, event)
            is TransferEvent.Progress -> builder
                .setContentTitle(context.getString(R.string.transfer_progress))
                .setContentText(context.getString(R.string.transfer_bytes, size(event.bytes), size(event.total)))
                .setProgress(PROGRESS_MAX, fraction(event.bytes, event.total), false)
                .setOnlyAlertOnce(true)
                .setOngoing(true)
                .addAction(0, context.getString(R.string.cancel), action(TransferReceiver.CANCEL, event.transferId))
            is TransferEvent.Finished -> finished(builder, event)
        }
        NotificationManagerCompat.from(context).notify(TAG, id, builder.build())
    }

    private fun offer(builder: NotificationCompat.Builder, event: TransferEvent.Offered) {
        val total = event.files.sumOf { it.size }
        val title = if (event.files.size == 1) {
            context.getString(R.string.transfer_offer_one, event.desktopName, event.files.single().name, size(total))
        } else {
            context.getString(R.string.transfer_offer, event.desktopName, event.files.size, size(total))
        }
        val open = Intent(context, OfferActivity::class.java)
            .putExtra(OfferActivity.EXTRA_TRANSFER, event.transferId)
            .putExtra(OfferActivity.EXTRA_TITLE, title)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        builder.setContentTitle(title)
            .setContentText(event.files.joinToString { it.name })
            .setContentIntent(
                PendingIntent.getActivity(context, event.transferId.hashCode(), open, PendingIntent.FLAG_IMMUTABLE),
            )
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setTimeoutAfter(OFFER_TIMEOUT_MS)
            .addAction(0, context.getString(R.string.transfer_accept), action(TransferReceiver.ACCEPT, event.transferId))
            .addAction(0, context.getString(R.string.transfer_decline), action(TransferReceiver.DECLINE, event.transferId))
    }

    private fun finished(builder: NotificationCompat.Builder, event: TransferEvent.Finished) {
        builder.setSmallIcon(android.R.drawable.stat_sys_download_done).setAutoCancel(true)
        if (event.status != "done") {
            builder.setContentTitle(context.getString(R.string.transfer_failed, event.desktopName, event.status))
            return
        }
        val title = if (event.incoming) R.string.transfer_received else R.string.transfer_sent
        builder.setContentTitle(context.getString(title, event.desktopName))
            .setContentText(event.saved.joinToString { it.name })
        event.saved.firstOrNull()?.let { saved ->
            val view = Intent(Intent.ACTION_VIEW, Uri.parse(saved.uri))
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
            builder.setContentIntent(
                PendingIntent.getActivity(context, event.transferId.hashCode(), view, PendingIntent.FLAG_IMMUTABLE),
            )
        }
    }

    private fun action(verb: String, transferId: String): PendingIntent = PendingIntent.getBroadcast(
        context,
        (verb + transferId).hashCode(),
        Intent(context, TransferReceiver::class.java).setAction(verb).putExtra(TransferReceiver.EXTRA_TRANSFER, transferId),
        PendingIntent.FLAG_IMMUTABLE,
    )

    private fun size(bytes: Long): String = Formatter.formatShortFileSize(context, bytes)

    private fun fraction(bytes: Long, total: Long): Int =
        if (total <= 0) 0 else (bytes * PROGRESS_MAX / total).toInt()

    private fun allowed(): Boolean = Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
        ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) ==
        PackageManager.PERMISSION_GRANTED

    companion object {
        const val TAG = "transfer"
        private const val PROGRESS_MAX = 1000
        /** The desktop withdraws an unanswered offer after 120 s. */
        private const val OFFER_TIMEOUT_MS = 120_000L
    }
}
