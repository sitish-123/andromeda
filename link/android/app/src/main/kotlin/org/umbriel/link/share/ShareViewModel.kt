package org.umbriel.link.share

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.core.domain.LinkFailure
import org.umbriel.link.core.domain.ShareKind
import org.umbriel.link.core.domain.linkFailure
import org.umbriel.link.core.domain.shareKindOf

sealed interface ShareState {
    data object Loading : ShareState
    data class Choose(val desktops: List<Desktop>) : ShareState
    data class Sending(val desktop: Desktop) : ShareState
}

sealed interface ShareOutcome {
    data class Sent(val desktop: Desktop) : ShareOutcome
    data object NoDesktop : ShareOutcome
    data object Cancelled : ShareOutcome
    data class Failed(val failure: LinkFailure) : ShareOutcome
}

/** Sends one shared text: to the only paired desktop at once, or to the one the user picks. */
class ShareViewModel(private val repository: LinkRepository, text: String) : ViewModel() {
    private val kind = shareKindOf(text.trim())
    private val payload = if (kind == ShareKind.Link) text.trim() else text
    private val _state = MutableStateFlow<ShareState>(ShareState.Loading)
    private val outcomeChannel = Channel<ShareOutcome>(Channel.BUFFERED)

    val state: StateFlow<ShareState> = _state.asStateFlow()
    val outcome: Flow<ShareOutcome> = outcomeChannel.receiveAsFlow()

    init {
        viewModelScope.launch {
            repository.refresh()
            val desktops = repository.desktops.value
            when (desktops.size) {
                0 -> outcomeChannel.send(ShareOutcome.NoDesktop)
                1 -> send(desktops.single())
                else -> _state.value = ShareState.Choose(desktops)
            }
        }
    }

    fun send(desktop: Desktop) {
        _state.value = ShareState.Sending(desktop)
        viewModelScope.launch {
            val result = repository.share(desktop.id, kind, payload)
            val outcome = result.exceptionOrNull()?.let { ShareOutcome.Failed(it.linkFailure()) } ?: ShareOutcome.Sent(desktop)
            outcomeChannel.send(outcome)
        }
    }

    fun cancel() {
        outcomeChannel.trySend(ShareOutcome.Cancelled)
    }
}
