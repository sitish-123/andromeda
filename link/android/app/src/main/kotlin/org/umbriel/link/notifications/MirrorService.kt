package org.umbriel.link.notifications

import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import org.umbriel.link.LinkApplication

/**
 * Bound by Android while the user grants notification access, which also opens the media sessions to
 * [org.umbriel.link.media.PhoneMedia]. It holds no state: every callback goes to the process-wide objects.
 */
class MirrorService : NotificationListenerService() {
    private val container get() = (application as LinkApplication).container

    override fun onListenerConnected() {
        container.mirror.attach(this)
        container.media.attach()
    }

    override fun onListenerDisconnected() {
        container.mirror.detach(this)
        container.media.detach()
    }

    override fun onNotificationPosted(sbn: StatusBarNotification, rankingMap: RankingMap) = container.mirror.posted(sbn, rankingMap)

    override fun onNotificationRemoved(sbn: StatusBarNotification) = container.mirror.removed(sbn)
}
