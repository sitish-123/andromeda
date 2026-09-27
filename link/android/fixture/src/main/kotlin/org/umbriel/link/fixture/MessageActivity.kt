package org.umbriel.link.fixture

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Person
import android.app.RemoteInput
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.graphics.drawable.Icon
import android.os.Bundle
import android.util.Log

/** Posts a chat message with a RemoteInput Reply, as a messaging app does, then finishes. */
class MessageActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        post(this, intent.getStringExtra(EXTRA_TEXT) ?: "Are we still on for dinner?", reply = null)
        finish()
    }

    companion object {
        const val EXTRA_TEXT = "text"
        const val TAG = "LinkFixture"
        private const val CHANNEL = "chat"
        private const val ID = 7

        fun post(context: Context, text: String, reply: String?) {
            val manager = context.getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(NotificationChannel(CHANNEL, "Chat", NotificationManager.IMPORTANCE_HIGH))
            val input = RemoteInput.Builder(ReplyReceiver.KEY).setLabel("Reply").build()
            val intent = PendingIntent.getBroadcast(
                context,
                0,
                Intent(context, ReplyReceiver::class.java),
                PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
            val action = Notification.Action.Builder(Icon.createWithResource(context, android.R.drawable.ic_menu_send), "Reply", intent)
                .addRemoteInput(input)
                .build()
            val sam = Person.Builder().setName("Sam").build()
            val style = Notification.MessagingStyle(Person.Builder().setName("You").build())
                .addMessage(text, System.currentTimeMillis(), sam)
            if (reply != null) style.addMessage(reply, System.currentTimeMillis(), null as Person?)
            val notification = Notification.Builder(context, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_notify_chat)
                .setStyle(style)
                .setCategory(Notification.CATEGORY_MESSAGE)
                .addAction(action)
                .build()
            manager.notify(ID, notification)
        }
    }
}

/** Receives the RemoteInput reply, logs it, and shows it in the conversation as messaging apps do. */
class ReplyReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val reply = RemoteInput.getResultsFromIntent(intent)?.getCharSequence(KEY)?.toString() ?: return
        Log.i(MessageActivity.TAG, "reply $reply")
        MessageActivity.post(context, "Are we still on for dinner?", reply)
    }

    companion object {
        const val KEY = "reply"
    }
}
