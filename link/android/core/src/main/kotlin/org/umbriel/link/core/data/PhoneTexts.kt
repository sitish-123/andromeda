package org.umbriel.link.core.data

import android.Manifest
import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.database.ContentObserver
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Telephony.Sms
import android.telephony.SmsManager
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import org.umbriel.link.ffi.PhoneText
import org.umbriel.link.ffi.TextOutcome

/**
 * The phone's text messages, newest first, and sending one. Android writes a text this app sends into the provider
 * itself, since the app is not the default SMS app, so a sent text shows like any other. [texts] and [send] are called
 * off the main thread; [changed] runs on it, at most once per [SETTLE_MS] of changes.
 */
class PhoneTexts(private val context: Context, private val changed: () -> Unit) {
    private val main = Handler(Looper.getMainLooper())
    private val notify = Runnable { changed() }
    private var watching = false
    private val sends = AtomicInteger()

    fun texts(offset: UInt, limit: UInt): List<PhoneText>? {
        if (!granted(Manifest.permission.READ_SMS)) return null
        val texts = mutableListOf<PhoneText>()
        context.contentResolver.query(
            Sms.CONTENT_URI,
            arrayOf(Sms._ID, Sms.THREAD_ID, Sms.ADDRESS, Sms.BODY, Sms.DATE, Sms.TYPE),
            null,
            null,
            "${Sms.DATE} DESC, ${Sms._ID} DESC",
        )?.use { cursor ->
            if (!cursor.moveToPosition(offset.toInt())) return@use
            do {
                texts += PhoneText(
                    id = cursor.getLong(0).toULong(),
                    thread = cursor.getLong(1).toULong(),
                    address = cursor.getString(2).orEmpty(),
                    body = cursor.getString(3).orEmpty(),
                    date = cursor.getLong(4).toULong(),
                    sent = cursor.getInt(5) != Sms.MESSAGE_TYPE_INBOX,
                )
            } while (texts.size < limit.toInt() && cursor.moveToNext())
        }
        return texts
    }

    /** Sends [body] to [to] and waits until Android reports every part sent, at most [SEND_TIMEOUT_S]. */
    fun send(to: String, body: String): TextOutcome {
        if (!granted(Manifest.permission.SEND_SMS)) return TextOutcome.DENIED
        val sms = context.getSystemService(SmsManager::class.java) ?: return TextOutcome.FAILED
        val parts = sms.divideMessage(body)
        val action = "${context.packageName}.TEXT_SENT.${sends.incrementAndGet()}"
        val left = CountDownLatch(parts.size)
        val ok = AtomicBoolean(true)
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                if (resultCode != Activity.RESULT_OK) ok.set(false)
                left.countDown()
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(receiver, IntentFilter(action), Context.RECEIVER_NOT_EXPORTED)
        } else {
            context.registerReceiver(receiver, IntentFilter(action))
        }
        try {
            val sent = parts.indices.map { part ->
                val intent = Intent(action).setPackage(context.packageName)
                PendingIntent.getBroadcast(context, part, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_ONE_SHOT)
            }
            sms.sendMultipartTextMessage(to, null, parts, ArrayList(sent), null)
            val finished = left.await(SEND_TIMEOUT_S, TimeUnit.SECONDS)
            return if (finished && ok.get()) TextOutcome.SENT else TextOutcome.FAILED
        } catch (_: RuntimeException) {
            return TextOutcome.FAILED
        } finally {
            context.unregisterReceiver(receiver)
        }
    }

    /** Starts watching for texts once the permission is there, since the provider refuses observers before it. */
    fun watch() {
        if (watching || !granted(Manifest.permission.READ_SMS)) return
        val observer = object : ContentObserver(main) {
            override fun onChange(selfChange: Boolean) {
                main.removeCallbacks(notify)
                main.postDelayed(notify, SETTLE_MS)
            }
        }
        context.contentResolver.registerContentObserver(Sms.CONTENT_URI, true, observer)
        watching = true
        changed()
    }

    private fun granted(permission: String) =
        context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    private companion object {
        const val SETTLE_MS = 1_000L
        const val SEND_TIMEOUT_S = 60L
    }
}
