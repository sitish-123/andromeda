package org.umbriel.link.devices

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.core.domain.LinkFailure
import org.umbriel.link.core.domain.linkFailure
import org.umbriel.link.core.domain.shareKindOf
import org.umbriel.link.notifications.NotificationMirror
import org.umbriel.link.presence.Presence

data class HomeState(
    val desktops: List<Desktop> = emptyList(),
    val stayConnected: Boolean = false,
    val ringing: Set<String> = emptySet(),
    val mirrorGranted: Boolean = false,
    val refreshing: Boolean = false,
) {
    /** The desktop the hero shows: a connected one first. */
    val primary: Desktop? get() = desktops.firstOrNull { it.connected } ?: desktops.firstOrNull()
    val others: List<Desktop> get() = desktops.filter { it != primary }
}

sealed interface HomeMessage {
    data class Paired(val name: String) : HomeMessage
    data class Sent(val name: String) : HomeMessage
    data object ClipboardEmpty : HomeMessage
    data class Failed(val failure: LinkFailure) : HomeMessage
}

class HomeViewModel(
    private val repository: LinkRepository,
    private val presence: Presence,
    mirror: NotificationMirror,
) : ViewModel() {
    private val refreshing = MutableStateFlow(false)
    private val _message = MutableStateFlow<HomeMessage?>(null)

    val state: StateFlow<HomeState> = combine(
        repository.desktops,
        presence.stayConnected,
        repository.ringingDesktops,
        mirror.granted,
        refreshing,
    ) { desktops, stay, ringing, granted, busy -> HomeState(desktops, stay, ringing, granted, busy) }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(STOP_TIMEOUT_MS), HomeState())

    /** The one-line message the screen shows, until [messageShown]. */
    val message: StateFlow<HomeMessage?> = _message.asStateFlow()

    init {
        viewModelScope.launch { repository.refresh() }
    }

    /** Pull to refresh: rereads the desktops and dials the ones not connected now, instead of at the next redial. */
    fun refresh() {
        viewModelScope.launch {
            refreshing.value = true
            repository.refresh()
            state.value.desktops.filterNot { it.connected }.forEach { repository.connect(it.id) }
            refreshing.value = false
        }
    }

    fun setStayConnected(stay: Boolean) = presence.setStayConnected(stay)

    fun notificationsAllowed() = presence.notificationsAllowed()

    fun ring(desktop: Desktop, on: Boolean) = act { repository.ringDesktop(desktop.id, on) }

    /** Sends what the clipboard holds, as a link when it is one; the app may read it only while in front. */
    fun sendClipboard(desktop: Desktop, text: String?) {
        if (text.isNullOrBlank()) {
            _message.value = HomeMessage.ClipboardEmpty
            return
        }
        val kind = shareKindOf(text.trim())
        act(HomeMessage.Sent(desktop.name)) { repository.share(desktop.id, kind, text) }
    }

    fun announce(desktop: Desktop) {
        _message.value = HomeMessage.Paired(desktop.name)
    }

    fun messageShown() = _message.update { null }

    private fun act(onSuccess: HomeMessage? = null, block: suspend () -> Result<*>) {
        viewModelScope.launch {
            val failure = block().exceptionOrNull()
            _message.value = failure?.let { HomeMessage.Failed(it.linkFailure()) } ?: onSuccess ?: _message.value
        }
    }

    private companion object {
        const val STOP_TIMEOUT_MS = 5_000L
    }
}
