package org.umbriel.link.pairing

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.core.domain.LinkFailure
import org.umbriel.link.core.domain.linkFailure

data class PairingState(
    val code: String = "",
    /** A pairing URI from the desktop's QR code, shown for confirmation before anything is sent. */
    val link: String? = null,
    val busy: Boolean = false,
    val failure: LinkFailure? = null,
) {
    val canPairCode: Boolean get() = !busy && code.length == CODE_LENGTH
}

const val CODE_LENGTH = 6

class PairingViewModel(private val repository: LinkRepository) : ViewModel() {
    private val _state = MutableStateFlow(PairingState())
    private val pairedChannel = Channel<Desktop>(Channel.BUFFERED)

    val state: StateFlow<PairingState> = _state.asStateFlow()
    val paired: Flow<Desktop> = pairedChannel.receiveAsFlow()

    fun offerLink(uri: String) = _state.update { PairingState(link = uri) }

    /** A code scanned inside the app was chosen by the user, so it pairs without the confirmation an outside link gets. */
    fun pairScanned(uri: String) {
        _state.update { PairingState() }
        pair { repository.pairUri(uri) }
    }

    fun dismissLink() = _state.update { it.copy(link = null, failure = null) }

    fun updateCode(input: String) = _state.update {
        it.copy(code = input.filter(Char::isDigit).take(CODE_LENGTH), failure = null)
    }

    fun pairWithCode() = pair { repository.pairCode(_state.value.code) }

    fun pairWithLink() {
        val link = _state.value.link ?: return
        pair { repository.pairUri(link) }
    }

    private fun pair(attempt: suspend () -> Result<Desktop>) {
        viewModelScope.launch {
            _state.update { it.copy(busy = true, failure = null) }
            attempt()
                .onSuccess { desktop ->
                    _state.value = PairingState()
                    pairedChannel.send(desktop)
                }
                .onFailure { error -> _state.update { it.copy(busy = false, failure = error.linkFailure()) } }
        }
    }
}
