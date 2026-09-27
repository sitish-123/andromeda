package org.umbriel.link.notifications

import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import org.umbriel.link.LinkApplication

/**
 * Bound by Android while the user grants notification access. It holds no state: every callback goes to the
 * process-wide [NotificationMirror].
 */
class MirrorService : NotificationListenerService() {
    private val mirror get() = (application as LinkApplication).container.mirror

    override fun onListenerConnected() = mirror.attach(this)

    override fun onListenerDisconnected() = mirror.detach(this)

    override fun onNotificationPosted(sbn: StatusBarNotification, rankingMap: RankingMap) = mirror.posted(sbn, rankingMap)

    override fun onNotificationRemoved(sbn: StatusBarNotification) = mirror.removed(sbn)
}
