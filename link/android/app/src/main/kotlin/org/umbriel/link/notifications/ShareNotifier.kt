package org.umbriel.link.notifications

import android.Manifest
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import java.util.concurrent.atomic.AtomicInteger
import org.umbriel.link.R
import org.umbriel.link.core.domain.IncomingShare
import org.umbriel.link.core.domain.ShareKind

/** Posts what a desktop shared: a link opens with the Open action, text is copied with the Copy action. */
class ShareNotifier(private val context: Context) {
    private val nextId = AtomicInteger()

    fun post(share: IncomingShare) {
        if (!allowed()) return
        val id = nextId.incrementAndGet()
        val link = share.kind == ShareKind.Link
        val action = if (link) open(id, share.text) else copy(id, share.text)
        val title = if (link) R.string.share_link_from else R.string.share_text_from
        val notification = NotificationCompat.Builder(context, Channels.SHARES)
            .setSmallIcon(R.drawable.ic_stat_link)
            .setContentTitle(context.getString(title, share.desktopName))
            .setContentText(share.text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(share.text))
            .setContentIntent(action)
            .addAction(0, context.getString(if (link) R.string.share_open else R.string.share_copy), action)
            .setAutoCancel(true)
            .build()
        NotificationManagerCompat.from(context).notify(TAG, id, notification)
    }

    private fun allowed(): Boolean = Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
        ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) ==
        PackageManager.PERMISSION_GRANTED

    private fun open(id: Int, url: String): PendingIntent = PendingIntent.getActivity(
        context,
        id,
        Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
        PendingIntent.FLAG_IMMUTABLE,
    )

    private fun copy(id: Int, text: String): PendingIntent = PendingIntent.getBroadcast(
        context,
        id,
        Intent(context, CopyReceiver::class.java).putExtra(CopyReceiver.EXTRA_TEXT, text).putExtra(CopyReceiver.EXTRA_ID, id),
        PendingIntent.FLAG_IMMUTABLE,
    )

    companion object {
        const val TAG = "share"
    }
}
