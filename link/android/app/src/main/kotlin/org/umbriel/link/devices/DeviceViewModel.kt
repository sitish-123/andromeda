package org.umbriel.link.devices

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.core.domain.Feature
import org.umbriel.link.core.domain.LinkFailure
import org.umbriel.link.core.domain.linkFailure

data class DeviceState(val desktop: Desktop? = null, val busy: Boolean = false, val failure: LinkFailure? = null)

/** One desktop: this phone's switches for it, reconnecting, and unpairing. */
class DeviceViewModel(private val repository: LinkRepository, private val desktopId: String) : ViewModel() {
    private val busy = MutableStateFlow(false)
    private val failure = MutableStateFlow<LinkFailure?>(null)
    private val _unpaired = MutableStateFlow(false)

    val state: StateFlow<DeviceState> = combine(repository.desktops, busy, failure) { desktops, working, failed ->
        DeviceState(desktops.firstOrNull { it.id == desktopId }, working, failed)
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(STOP_TIMEOUT_MS), DeviceState())

    /** True once the desktop is unpaired, so the screen leaves. */
    val unpaired: StateFlow<Boolean> = _unpaired

    fun setSharing(feature: Feature, on: Boolean) = act { repository.setSharing(desktopId, feature, on) }

    /** Dials now instead of waiting for the next redial. */
    fun connect() = act { repository.connect(desktopId) }

    fun unpair() = act { repository.unpair(desktopId).onSuccess { _unpaired.value = true } }

    fun failureShown() {
        failure.value = null
    }

    private fun act(block: suspend () -> Result<*>) {
        viewModelScope.launch {
            busy.value = true
            block().exceptionOrNull()?.let { failure.value = it.linkFailure() }
            busy.value = false
        }
    }

    private companion object {
        const val STOP_TIMEOUT_MS = 5_000L
    }
}
