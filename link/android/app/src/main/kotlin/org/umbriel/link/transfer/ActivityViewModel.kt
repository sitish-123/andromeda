package org.umbriel.link.transfer

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.core.domain.LinkFailure
import org.umbriel.link.core.domain.linkFailure

sealed interface ActivityMessage {
    data class Failed(val failure: LinkFailure) : ActivityMessage
    data object Expired : ActivityMessage
}

class ActivityViewModel(private val repository: LinkRepository) : ViewModel() {
    val transfers = repository.transferActivity
    private val _busy = MutableStateFlow<Set<String>>(emptySet())
    private val _message = MutableStateFlow<ActivityMessage?>(null)
    val busy: StateFlow<Set<String>> = _busy.asStateFlow()
    val message: StateFlow<ActivityMessage?> = _message.asStateFlow()

    fun accept(id: String) = act(id) { repository.acceptTransfer(id) }
    fun decline(id: String) = act(id) { repository.declineTransfer(id) }
    fun cancel(id: String) = act(id) { repository.cancelTransfer(id) }
    fun messageShown() { _message.value = null }

    private fun act(id: String, block: suspend () -> Result<Boolean>) {
        if (id in _busy.value) return
        _busy.update { it + id }
        viewModelScope.launch {
            try {
                val result = block()
                _message.value = result.exceptionOrNull()?.let { ActivityMessage.Failed(it.linkFailure()) }
                    ?: if (result.getOrNull() == false) ActivityMessage.Expired else null
            } finally {
                _busy.update { it - id }
            }
        }
    }
}
