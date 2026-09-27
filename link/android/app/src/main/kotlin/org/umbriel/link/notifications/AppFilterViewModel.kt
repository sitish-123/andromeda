package org.umbriel.link.notifications

import android.content.Intent
import android.content.pm.PackageManager
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class MirroredApp(val packageName: String, val label: String, val mirrored: Boolean)

/** The launcher's apps, each mirrored unless the user turned it off. */
class AppFilterViewModel(private val mirror: NotificationMirror, private val packages: PackageManager) : ViewModel() {
    private val apps = MutableStateFlow<List<Pair<String, String>>>(emptyList())

    val state: StateFlow<List<MirroredApp>> = combine(apps, mirror.excluded) { list, excluded ->
        list.map { (name, label) -> MirroredApp(name, label, name !in excluded) }
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(STOP_TIMEOUT_MS), emptyList())

    init {
        viewModelScope.launch {
            apps.value = withContext(Dispatchers.IO) {
                val launcher = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                packages.queryIntentActivities(launcher, 0)
                    .map { it.activityInfo.packageName to it.loadLabel(packages).toString() }
                    .distinctBy { it.first }
                    .sortedBy { it.second.lowercase() }
            }
        }
    }

    fun setMirrored(packageName: String, mirrored: Boolean) = mirror.setExcluded(packageName, !mirrored)

    private companion object {
        const val STOP_TIMEOUT_MS = 5_000L
    }
}
