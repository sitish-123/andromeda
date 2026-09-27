package org.umbriel.link.media

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.core.domain.LinkFailure
import org.umbriel.link.core.domain.MediaCommandKind
import org.umbriel.link.core.domain.MediaPlayer
import org.umbriel.link.core.domain.linkFailure

data class ShownPlayer(val desktopId: String, val desktopName: String, val player: MediaPlayer)

/** The players of every connected desktop, and commands to them. */
class MediaViewModel(private val repository: LinkRepository) : ViewModel() {
    private val failures = Channel<LinkFailure>(Channel.BUFFERED)

    val players: StateFlow<List<ShownPlayer>> = combine(repository.desktopPlayers, repository.desktops) { players, desktops ->
        players.map { shown ->
            ShownPlayer(shown.desktopId, desktops.firstOrNull { it.id == shown.desktopId }?.name.orEmpty(), shown.player)
        }
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(STOP_TIMEOUT_MS), emptyList())

    val failed: Flow<LinkFailure> = failures.receiveAsFlow()

    fun command(shown: ShownPlayer, command: MediaCommandKind, value: Long? = null) {
        viewModelScope.launch {
            repository.mediaCommand(shown.desktopId, shown.player.player, command, value)
                .onFailure { failures.send(it.linkFailure()) }
        }
    }

    private companion object {
        const val STOP_TIMEOUT_MS = 5_000L
    }
}
