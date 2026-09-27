package org.umbriel.link.media

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowLeft
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import android.os.SystemClock
import kotlinx.coroutines.delay
import org.umbriel.link.R
import org.umbriel.link.core.domain.MediaCommandKind
import org.umbriel.link.core.domain.PlaybackState
import org.umbriel.link.ui.components.CenteredText
import org.umbriel.link.ui.components.ConnectionOrb
import org.umbriel.link.ui.components.Eyebrow
import org.umbriel.link.ui.components.Glyph
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.components.SoftCard
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Size
import org.umbriel.link.ui.theme.Space

/** Every connected desktop's players as soft cards: artwork, track, progress, and transport pills. */
@Composable
fun MediaScreen(viewModel: MediaViewModel, onBack: () -> Unit) {
    val players by viewModel.players.collectAsStateWithLifecycle()
    Screen(title = stringResource(R.string.media_title), onBack = onBack) {
        if (players.isEmpty()) {
            Column(
                Modifier.fillMaxSize().padding(Space.page),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(Space.s24, Alignment.CenterVertically),
            ) {
                ConnectionOrb(active = false, size = 120.dp)
                CenteredText(stringResource(R.string.media_empty_title), stringResource(R.string.media_empty))
            }
            return@Screen
        }
        LazyColumn(
            Modifier.fillMaxSize(),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = Space.page, vertical = Space.s8),
            verticalArrangement = Arrangement.spacedBy(Space.section),
        ) {
            items(players, key = { it.desktopId + it.player.player }) { shown -> PlayerCard(shown, viewModel) }
        }
    }
}

@Composable
private fun PlayerCard(shown: ShownPlayer, viewModel: MediaViewModel) {
    val player = shown.player
    val colors = LinkTheme.colors
    val playing = player.state == PlaybackState.Playing
    SoftCard(Modifier.fillMaxWidth()) {
        Row(horizontalArrangement = Arrangement.spacedBy(Space.s16), verticalAlignment = Alignment.CenterVertically) {
            Artwork(player.artwork)
            Column(Modifier.weight(1f)) {
                Eyebrow("${player.name} · ${shown.desktopName}")
                Label(player.title.ifEmpty { player.name }, LinkTheme.type.headlineMedium, maxLines = 2)
                if (player.artist.isNotEmpty()) Label(player.artist, LinkTheme.type.bodyMedium, colors.textSecondary, maxLines = 1)
            }
        }
        Progress(player.positionMs, player.lengthMs, player.receivedAt, playing)
        Row(
            Modifier.fillMaxWidth().padding(top = Space.s16),
            horizontalArrangement = Arrangement.spacedBy(Space.s12, Alignment.CenterHorizontally),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val can = player.can
            RoundControl(Icons.AutoMirrored.Filled.KeyboardArrowLeft, stringResource(R.string.media_previous), MediaCommandKind.Previous in can) {
                viewModel.command(shown, MediaCommandKind.Previous)
            }
            PillButton(
                stringResource(if (playing) R.string.media_pause else R.string.media_play),
                onClick = { viewModel.command(shown, MediaCommandKind.PlayPause) },
                icon = if (playing) null else Icons.Filled.PlayArrow,
                enabled = MediaCommandKind.PlayPause in can,
            )
            RoundControl(Icons.AutoMirrored.Filled.KeyboardArrowRight, stringResource(R.string.media_next), MediaCommandKind.Next in can) {
                viewModel.command(shown, MediaCommandKind.Next)
            }
        }
    }
}

@Composable
private fun Artwork(bytes: ByteArray?) {
    val colors = LinkTheme.colors
    val image = remember(bytes) { bytes?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }?.asImageBitmap() }
    Box(
        Modifier.size(72.dp).clip(Radius.hero).background(colors.accent.copy(alpha = 0.16f)),
        contentAlignment = Alignment.Center,
    ) {
        if (image != null) {
            Image(image, contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
        } else {
            Glyph(Icons.Filled.PlayArrow, colors.accentText, Size.iconLarge)
        }
    }
}

/** The position advances locally while playing, from when the desktop last reported it. */
@Composable
private fun Progress(positionMs: Long, lengthMs: Long?, receivedAt: Long, playing: Boolean) {
    val length = lengthMs ?: return
    val colors = LinkTheme.colors
    var now by remember { mutableLongStateOf(SystemClock.elapsedRealtime()) }
    LaunchedEffect(playing, receivedAt) {
        // Playback has no change event on this side; the bar moves once a second while it plays.
        while (playing) {
            now = SystemClock.elapsedRealtime()
            delay(1_000)
        }
    }
    val position = if (playing) positionMs + (now - receivedAt) else positionMs
    val fraction = (position.toFloat() / length).coerceIn(0f, 1f)
    Box(Modifier.fillMaxWidth().padding(top = Space.s16).height(6.dp).clip(Radius.pill).background(colors.surfaceTertiary)) {
        Box(Modifier.fillMaxWidth(fraction).height(6.dp).clip(Radius.pill).background(colors.accent))
    }
}

@Composable
private fun RoundControl(icon: ImageVector, label: String, enabled: Boolean, onClick: () -> Unit) {
    val colors = LinkTheme.colors
    Box(
        Modifier.size(Size.touch).clip(Radius.pill).background(colors.surfaceTertiary)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .semantics { contentDescription = label },
        contentAlignment = Alignment.Center,
    ) { Glyph(icon, if (enabled) colors.textPrimary else colors.textDisabled) }
}
