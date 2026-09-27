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

/** What the share target received: text (sent as a link when it is one), or files behind content URIs. */
sealed interface SharePayload {
    data class Text(val text: String) : SharePayload
    data class Files(val uris: List<String>) : SharePayload
}

sealed interface ShareOutcome {
    data class Sent(val desktop: Desktop, val files: Boolean) : ShareOutcome
    data object NoDesktop : ShareOutcome
    data object Cancelled : ShareOutcome
    data class Failed(val failure: LinkFailure) : ShareOutcome
}

/** Sends one share: to the only paired desktop at once, or to the one the user picks. */
class ShareViewModel(private val repository: LinkRepository, private val payload: SharePayload) : ViewModel() {
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
            val result = when (payload) {
                is SharePayload.Text -> {
                    val kind = shareKindOf(payload.text.trim())
                    repository.share(desktop.id, kind, if (kind == ShareKind.Link) payload.text.trim() else payload.text)
                }
                is SharePayload.Files -> repository.sendFiles(desktop.id, payload.uris).map {}
            }
            val outcome = result.exceptionOrNull()?.let { ShareOutcome.Failed(it.linkFailure()) }
                ?: ShareOutcome.Sent(desktop, payload is SharePayload.Files)
            outcomeChannel.send(outcome)
        }
    }

    fun cancel() {
        outcomeChannel.trySend(ShareOutcome.Cancelled)
    }
}
