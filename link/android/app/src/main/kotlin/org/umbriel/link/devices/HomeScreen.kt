package org.umbriel.link.devices

import android.Manifest
import android.content.ClipboardManager
import android.content.pm.PackageManager
import android.os.Build
import android.text.format.DateUtils
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Send
import androidx.compose.material.icons.filled.Notifications
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Warning
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.umbriel.link.R
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.clipboard.ClipboardHint
import org.umbriel.link.ui.components.ActionOrb
import org.umbriel.link.ui.components.BentoGrid
import org.umbriel.link.ui.components.BentoTile
import org.umbriel.link.ui.components.CenteredText
import org.umbriel.link.ui.components.ConnectionOrb
import org.umbriel.link.ui.components.Eyebrow
import org.umbriel.link.ui.components.HeroSurface
import org.umbriel.link.ui.components.IconChip
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.OrbAction
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.PullRefresh
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.components.SoftCard
import org.umbriel.link.ui.components.SwitchRow
import org.umbriel.link.ui.text
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Space

/** Where the app opens: the primary desktop as a hero, its actions as a bento grid, and the rest below. */
@Composable
fun HomeScreen(
    viewModel: HomeViewModel,
    setupNeeded: Boolean,
    onPair: () -> Unit,
    onDesktop: (Desktop) -> Unit,
    onMedia: () -> Unit,
    onNotifications: () -> Unit,
    onSetup: () -> Unit,
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val message by viewModel.message.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val resources = context.resources
    var orbOpen by remember { mutableStateOf(false) }
    val notificationPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) viewModel.notificationsAllowed()
    }
    val paired = state.desktops.isNotEmpty()
    // Asked once a desktop is paired, the first moment one can send this phone anything to notify about.
    LaunchedEffect(paired) {
        val needed = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        if (paired && needed) notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
    }
    val toast = when (val shown = message) {
        is HomeMessage.Paired -> resources.getString(R.string.paired_with, shown.name)
        is HomeMessage.Sent -> resources.getString(R.string.share_sent, shown.name)
        HomeMessage.ClipboardEmpty -> resources.getString(R.string.clipboard_empty)
        is HomeMessage.Failed -> shown.failure.text(resources)
        null -> null
    }
    Screen(
        title = stringResource(R.string.desktops_title),
        toast = toast,
        onToastShown = viewModel::messageShown,
        overlay = {
            if (paired) {
                ActionOrb(
                    expanded = orbOpen,
                    onToggle = { orbOpen = !orbOpen },
                    actions = listOf(
                        OrbAction(stringResource(R.string.pair_action), Icons.Filled.Add) { orbOpen = false; onPair() },
                        OrbAction(stringResource(R.string.setup_title), Icons.Filled.Settings) { orbOpen = false; onSetup() },
                    ),
                    modifier = Modifier.align(Alignment.BottomEnd).navigationBarsPadding().padding(Space.page),
                )
            }
        },
    ) {
        PullRefresh(state.refreshing, viewModel::refresh, Modifier.fillMaxSize()) {
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Space.page),
                verticalArrangement = Arrangement.spacedBy(Space.section),
            ) {
                val primary = state.primary
                if (primary == null) {
                    Empty(onPair)
                } else {
                    Hero(primary, onClick = { onDesktop(primary) })
                    if (setupNeeded) SetupCard(onSetup)
                    Actions(state, primary, viewModel, onMedia, onNotifications) {
                        context.getSystemService(ClipboardManager::class.java).primaryClip
                            ?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(context)?.toString()
                    }
                    SoftCard(Modifier.fillMaxWidth(), padding = Space.s16) {
                        SwitchRow(
                            title = stringResource(R.string.stay_connected),
                            subtitle = stringResource(R.string.stay_connected_hint),
                            checked = state.stayConnected,
                            onChange = viewModel::setStayConnected,
                        )
                        ClipboardHint()
                    }
                    if (state.others.isNotEmpty()) {
                        Column {
                            Eyebrow(stringResource(R.string.other_desktops))
                            state.others.forEach { desktop -> OtherDesktop(desktop) { onDesktop(desktop) } }
                        }
                    }
                }
                Spacer(Modifier.height(96.dp))
            }
        }
    }
}

@Composable
private fun Empty(onPair: () -> Unit) {
    Column(
        Modifier.fillMaxWidth().padding(top = Space.s48),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(Space.s24),
    ) {
        ConnectionOrb(active = false, size = 120.dp)
        CenteredText(stringResource(R.string.desktops_empty), stringResource(R.string.desktops_empty_hint))
        PillButton(stringResource(R.string.pair_action), onPair, icon = Icons.Filled.Add)
    }
}

@Composable
private fun Hero(desktop: Desktop, onClick: () -> Unit) {
    val colors = LinkTheme.colors
    HeroSurface(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Space.s16)) {
            ConnectionOrb(active = desktop.connected, size = 72.dp)
            Column(Modifier.weight(1f)) {
                Eyebrow(stringResource(R.string.primary_desktop))
                Label(desktop.name, LinkTheme.type.displaySmall, maxLines = 2)
                val status = if (desktop.connected) {
                    stringResource(R.string.connected)
                } else {
                    stringResource(R.string.last_seen, lastSeen(desktop.lastSeen))
                }
                Label(status, LinkTheme.type.titleMedium, if (desktop.connected) colors.accentText else colors.textSecondary)
            }
        }
        PillButton(
            stringResource(R.string.desktop_open),
            onClick,
            kind = PillKind.Tonal,
            modifier = Modifier.padding(top = Space.s20),
        )
    }
}

@Composable
private fun SetupCard(onSetup: () -> Unit) {
    SoftCard(Modifier.fillMaxWidth(), onClick = onSetup, padding = Space.s16) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Space.s12)) {
            IconChip(Icons.Filled.Warning, LinkTheme.colors.warning, LinkTheme.colors.warning.copy(alpha = 0.14f))
            Column(Modifier.weight(1f)) {
                Label(stringResource(R.string.setup_title), LinkTheme.type.titleLarge)
                Label(stringResource(R.string.setup_hint), LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
            }
        }
    }
}

@Composable
private fun Actions(
    state: HomeState,
    desktop: Desktop,
    viewModel: HomeViewModel,
    onMedia: () -> Unit,
    onNotifications: () -> Unit,
    clipboard: () -> String?,
) {
    val ringing = desktop.id in state.ringing
    BentoGrid(
        listOf(
            false to { modifier ->
                BentoTile(
                    stringResource(if (ringing) R.string.ring_desktop_stop else R.string.ring_desktop),
                    stringResource(R.string.ring_desktop_hint),
                    Icons.Filled.Notifications,
                    onClick = { viewModel.ring(desktop, !ringing) },
                    modifier = modifier,
                    highlighted = ringing,
                )
            },
            false to { modifier ->
                BentoTile(
                    stringResource(R.string.send_clipboard),
                    stringResource(R.string.send_clipboard_hint),
                    Icons.Filled.Send,
                    onClick = { viewModel.sendClipboard(desktop, clipboard()) },
                    modifier = modifier,
                )
            },
            true to { modifier ->
                BentoTile(
                    stringResource(R.string.media_title),
                    stringResource(R.string.media_hint),
                    Icons.Filled.PlayArrow,
                    onClick = onMedia,
                    modifier = modifier,
                )
            },
            true to { modifier ->
                BentoTile(
                    stringResource(R.string.mirror_title),
                    stringResource(if (state.mirrorGranted) R.string.mirror_on_hint else R.string.mirror_off_hint),
                    Icons.Filled.Notifications,
                    onClick = onNotifications,
                    modifier = modifier,
                )
            },
        ),
    )
}

@Composable
private fun OtherDesktop(desktop: Desktop, onClick: () -> Unit) {
    SoftCard(Modifier.fillMaxWidth().padding(bottom = Space.block), onClick = onClick, padding = Space.s16) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Space.s12)) {
            ConnectionOrb(active = desktop.connected, size = 36.dp)
            Column(Modifier.weight(1f)) {
                Label(desktop.name, LinkTheme.type.titleLarge, maxLines = 1)
                val status = if (desktop.connected) stringResource(R.string.connected) else stringResource(R.string.not_connected)
                Label(status, LinkTheme.type.bodySmall, LinkTheme.colors.textSecondary)
            }
        }
    }
}

private fun lastSeen(unixSeconds: Long): String =
    if (unixSeconds <= 0) "never" else DateUtils.getRelativeTimeSpanString(unixSeconds * 1000).toString()
