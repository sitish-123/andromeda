package org.umbriel.link.onboarding

import android.Manifest
import android.annotation.SuppressLint
import android.app.Application
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.Settings
import androidx.core.content.ContextCompat
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import org.umbriel.link.clipboard.ClipboardWatcher
import org.umbriel.link.notifications.NotificationMirror
import org.umbriel.link.ring.Ringer

/** One grant the app asks for, and how it is given: a Settings page, or runtime permissions. */
enum class Grant { NotificationAccess, DoNotDisturb, PostNotifications, Battery, Calls, Bluetooth, AutoClipboard }

/** Grants the setup banner does not ask for: automatic clipboard needs a computer once, which not everyone has. */
private val OPTIONAL = setOf(Grant.AutoClipboard)

data class OnboardingState(val granted: Set<Grant> = emptySet()) {
    val complete: Boolean get() = granted.containsAll(Grant.entries - OPTIONAL)
}

/** The permission onboarding: what is granted, rechecked whenever the app returns from Settings or a prompt. */
class OnboardingViewModel(
    private val application: Application,
    mirror: NotificationMirror,
    private val ringer: Ringer,
) : ViewModel() {
    private val checked = MutableStateFlow(check())

    val state: StateFlow<OnboardingState> = combine(checked, mirror.granted) { others, listener ->
        OnboardingState(if (listener) others + Grant.NotificationAccess else others - Grant.NotificationAccess)
    }.stateIn(viewModelScope, SharingStarted.Eagerly, OnboardingState(checked.value))

    fun refresh() {
        checked.value = check()
    }

    /** The two halves of automatic clipboard, which the page shows apart since only one is given on the phone. */
    fun overlayAllowed(): Boolean = Settings.canDrawOverlays(application)

    fun logsAllowed(): Boolean = held(Manifest.permission.READ_LOGS)

    /** The one command to run from a computer with the phone plugged in. */
    fun adbCommand(): String = "adb shell pm grant ${application.packageName} android.permission.READ_LOGS"

    /** The runtime permissions a grant asks for; empty for one given in Settings. */
    fun permissions(grant: Grant): Array<String> = when (grant) {
        Grant.PostNotifications -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            arrayOf(Manifest.permission.POST_NOTIFICATIONS)
        } else {
            emptyArray()
        }
        Grant.Calls -> CALL_PERMISSIONS
        Grant.Bluetooth -> when {
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU ->
                arrayOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.NEARBY_WIFI_DEVICES)
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
                arrayOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.ACCESS_FINE_LOCATION)
            // The local-only hotspot needs location before Android 13.
            else -> arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        else -> emptyArray()
    }

    /** The Settings page that gives a grant, for those not asked at runtime. */
    @SuppressLint("BatteryLife")
    fun settings(grant: Grant, mirrorSettings: Intent): Intent? = when (grant) {
        Grant.NotificationAccess -> mirrorSettings
        Grant.DoNotDisturb -> ringer.dndSettings()
        // Stay connected is a foreground service Android would otherwise stop to save power; the exemption is the
        // documented way for a companion app to keep it, and the page explains it first.
        Grant.Battery -> Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:${application.packageName}"))
        Grant.AutoClipboard -> Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:${application.packageName}"))
        else -> null
    }?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

    private fun check(): Set<Grant> = buildSet {
        if (ringer.dndGranted()) add(Grant.DoNotDisturb)
        if (permissions(Grant.PostNotifications).all(::held)) add(Grant.PostNotifications)
        if (application.getSystemService(PowerManager::class.java).isIgnoringBatteryOptimizations(application.packageName)) {
            add(Grant.Battery)
        }
        if (CALL_PERMISSIONS.all(::held)) add(Grant.Calls)
        if (permissions(Grant.Bluetooth).all(::held)) add(Grant.Bluetooth)
        if (ClipboardWatcher.granted(application)) add(Grant.AutoClipboard)
    }

    private fun held(permission: String) =
        ContextCompat.checkSelfPermission(application, permission) == PackageManager.PERMISSION_GRANTED

    private companion object {
        val CALL_PERMISSIONS = arrayOf(
            Manifest.permission.READ_PHONE_STATE,
            Manifest.permission.READ_CALL_LOG,
            Manifest.permission.READ_CONTACTS,
            Manifest.permission.ANSWER_PHONE_CALLS,
        )
    }
}
