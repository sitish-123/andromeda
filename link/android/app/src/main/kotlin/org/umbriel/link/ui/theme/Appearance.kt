package org.umbriel.link.ui.theme

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.edit

enum class AppTheme { System, Light, Dark }

@Composable
fun rememberAppTheme(): State<AppTheme> {
    val context = LocalContext.current
    val prefs = remember(context) { preferences(context) }
    val mode = remember(prefs) { mutableStateOf(read(prefs)) }
    DisposableEffect(prefs) {
        val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (key == KEY) mode.value = read(prefs)
        }
        prefs.registerOnSharedPreferenceChangeListener(listener)
        onDispose { prefs.unregisterOnSharedPreferenceChangeListener(listener) }
    }
    return mode
}

fun setAppTheme(context: Context, theme: AppTheme) {
    preferences(context).edit { putString(KEY, theme.name) }
}

@Composable
fun preferredDarkTheme(): Boolean = when (rememberAppTheme().value) {
    AppTheme.System -> isSystemInDarkTheme()
    AppTheme.Light -> false
    AppTheme.Dark -> true
}

private fun preferences(context: Context) = context.getSharedPreferences("appearance", Context.MODE_PRIVATE)
private fun read(prefs: SharedPreferences): AppTheme =
    AppTheme.entries.firstOrNull { it.name == prefs.getString(KEY, null) } ?: AppTheme.System
private const val KEY = "theme"
