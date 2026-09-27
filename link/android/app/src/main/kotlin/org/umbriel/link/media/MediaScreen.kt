package org.umbriel.link.media

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.umbriel.link.R
import org.umbriel.link.core.domain.MediaCommandKind
import org.umbriel.link.core.domain.PlaybackState

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MediaScreen(viewModel: MediaViewModel, onBack: () -> Unit) {
    val players by viewModel.players.collectAsStateWithLifecycle()
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.media_title)) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.back))
                    }
                },
            )
        },
    ) { padding ->
        LazyColumn(Modifier.padding(padding).fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            if (players.isEmpty()) item { Text(stringResource(R.string.media_empty)) }
            items(players, key = { it.desktopId + it.player.player }) { shown ->
                val player = shown.player
                Column(Modifier.fillMaxWidth()) {
                    Text(player.title.ifEmpty { player.name }, style = MaterialTheme.typography.titleMedium)
                    Text("${player.artist} · ${player.name} · ${shown.desktopName}", style = MaterialTheme.typography.bodyMedium)
                    Row {
                        TextButton(onClick = { viewModel.command(shown, MediaCommandKind.Previous) }) {
                            Text(stringResource(R.string.media_previous))
                        }
                        val playing = player.state == PlaybackState.Playing
                        TextButton(onClick = { viewModel.command(shown, MediaCommandKind.PlayPause) }) {
                            Text(stringResource(if (playing) R.string.media_pause else R.string.media_play))
                        }
                        TextButton(onClick = { viewModel.command(shown, MediaCommandKind.Next) }) {
                            Text(stringResource(R.string.media_next))
                        }
                    }
                }
            }
        }
    }
}
