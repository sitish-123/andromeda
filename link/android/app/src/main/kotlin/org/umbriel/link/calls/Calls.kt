package org.umbriel.link.calls

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioManager
import android.net.Uri
import android.provider.ContactsContract
import android.telecom.TelecomManager
import android.telephony.TelephonyManager
import android.util.Log
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import org.umbriel.link.LinkApplication
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.core.domain.CallAction
import org.umbriel.link.core.domain.CallLimits
import org.umbriel.link.core.domain.CallState
import org.umbriel.link.core.domain.truncateUtf8

/**
 * The phone's calls for desktops: each state change from [CallStateReceiver], with the number when Android gives it
 * (`READ_CALL_LOG`) and the contact's name when the app may read contacts. Desktops mute the ringer or decline.
 */
class Calls(private val context: Context, private val repository: LinkRepository, private val scope: CoroutineScope) {
    private val audio = context.getSystemService(AudioManager::class.java)
    private var state = CallState.Idle
    private var muted = false

    fun start() {
        scope.launch { repository.callActions.collect(::act) }
    }

    fun changed(next: CallState, number: String?) {
        state = next
        if (next == CallState.Idle && muted) {
            audio.adjustStreamVolume(AudioManager.STREAM_RING, AudioManager.ADJUST_UNMUTE, 0)
            muted = false
        }
        val shownNumber = number?.takeIf { it.isNotBlank() }?.truncateUtf8(CallLimits.NUMBER)
        val name = shownNumber?.let(::contactName)?.truncateUtf8(CallLimits.NAME)
        scope.launch { repository.reportCall(next, shownNumber, name).onFailure { Log.w(TAG, "reporting a call: ${it.message}") } }
    }

    private fun act(action: CallAction) {
        if (state != CallState.Ringing) return
        when (action) {
            CallAction.Mute -> {
                audio.adjustStreamVolume(AudioManager.STREAM_RING, AudioManager.ADJUST_MUTE, 0)
                muted = true
            }
            CallAction.Decline -> decline()
        }
    }

    @Suppress("DEPRECATION")
    private fun decline() {
        if (!granted(Manifest.permission.ANSWER_PHONE_CALLS)) {
            Log.i(TAG, "declining needs ANSWER_PHONE_CALLS")
            return
        }
        runCatching { context.getSystemService(TelecomManager::class.java).endCall() }
            .onFailure { Log.w(TAG, "declining: ${it.message}") }
    }

    private fun contactName(number: String): String? {
        if (!granted(Manifest.permission.READ_CONTACTS)) return null
        val uri = Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode(number))
        return runCatching {
            context.contentResolver.query(uri, arrayOf(ContactsContract.PhoneLookup.DISPLAY_NAME), null, null, null)
                ?.use { cursor -> if (cursor.moveToFirst()) cursor.getString(0) else null }
        }.getOrNull()?.takeIf { it.isNotBlank() }
    }

    private fun granted(permission: String) =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

    private companion object {
        const val TAG = "Calls"
    }
}

/**
 * Android's call-state broadcast, which reaches a manifest receiver of an app holding `READ_PHONE_STATE`; it carries
 * the number only with `READ_CALL_LOG`, and then arrives a second time with it.
 */
class CallStateReceiver : BroadcastReceiver() {
    @Suppress("DEPRECATION")
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != TelephonyManager.ACTION_PHONE_STATE_CHANGED) return
        val state = when (intent.getStringExtra(TelephonyManager.EXTRA_STATE)) {
            TelephonyManager.EXTRA_STATE_RINGING -> CallState.Ringing
            TelephonyManager.EXTRA_STATE_OFFHOOK -> CallState.Active
            else -> CallState.Idle
        }
        val number = intent.getStringExtra(TelephonyManager.EXTRA_INCOMING_NUMBER)
        (context.applicationContext as LinkApplication).container.calls.changed(state, number)
    }
}
