package org.umbriel.link.devices

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.core.domain.LinkFailure
import org.umbriel.link.core.domain.linkFailure
import org.umbriel.link.notifications.NotificationMirror
import org.umbriel.link.presence.Presence

data class DevicesState(
    val desktops: List<Desktop> = emptyList(),
    val busy: Set<String> = emptySet(),
    val stayConnected: Boolean = false,
    val mirrorGranted: Boolean = false,
)

sealed interface DevicesMessage {
    data class Paired(val name: String) : DevicesMessage
    data class Unpaired(val name: String) : DevicesMessage
    data class Failed(val failure: LinkFailure) : DevicesMessage
}

class DevicesViewModel(
    private val repository: LinkRepository,
    private val presence: Presence,
    private val mirror: NotificationMirror,
) : ViewModel() {
    private val busy = MutableStateFlow(emptySet<String>())
    private val messageChannel = Channel<DevicesMessage>(Channel.BUFFERED)

    val state: StateFlow<DevicesState> =
        combine(repository.desktops, busy, presence.stayConnected, mirror.granted, ::DevicesState)
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(STOP_TIMEOUT_MS), DevicesState())
    val messages: Flow<DevicesMessage> = messageChannel.receiveAsFlow()

    /** Desktops ringing because this phone asked. */
    val ringingDesktops: StateFlow<Set<String>> = repository.ringingDesktops

    init {
        viewModelScope.launch { repository.refresh() }
    }

    /** Dials now instead of waiting for the next redial. */
    fun connect(desktop: Desktop) = act(desktop) { repository.connect(desktop.id) }

    fun unpair(desktop: Desktop) = act(desktop, DevicesMessage.Unpaired(desktop.name)) { repository.unpair(desktop.id) }

    fun setStayConnected(stay: Boolean) = presence.setStayConnected(stay)

    fun ring(desktop: Desktop, on: Boolean) = act(desktop) { repository.ringDesktop(desktop.id, on) }

    fun notificationsAllowed() = presence.notificationsAllowed()

    fun mirrorAccessSettings() = mirror.accessSettings()

    fun announce(desktop: Desktop) {
        messageChannel.trySend(DevicesMessage.Paired(desktop.name))
    }

    private fun act(desktop: Desktop, onSuccess: DevicesMessage? = null, block: suspend () -> Result<*>) {
        viewModelScope.launch {
            busy.update { it + desktop.id }
            val result = block()
            busy.update { it - desktop.id }
            val message = result.exceptionOrNull()?.let { DevicesMessage.Failed(it.linkFailure()) } ?: onSuccess
            message?.let { messageChannel.send(it) }
        }
    }

    private companion object {
        const val STOP_TIMEOUT_MS = 5_000L
    }
}
