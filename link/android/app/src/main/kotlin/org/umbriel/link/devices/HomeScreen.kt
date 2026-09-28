package org.umbriel.link.devices

import android.Manifest
import android.content.ClipboardManager
import android.content.pm.PackageManager
import android.os.Build
import android.text.format.DateUtils
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Notifications
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Warning
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.umbriel.link.R
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.files.MediaAccess
import org.umbriel.link.files.MediaTile
import org.umbriel.link.files.PhoneFiles
import org.umbriel.link.files.PickerSheet
import org.umbriel.link.files.PickerState
import org.umbriel.link.files.PickerTab
import org.umbriel.link.files.PickerViewModel
import org.umbriel.link.files.sendLabel
import org.umbriel.link.ui.components.CenteredText
import org.umbriel.link.ui.components.ConnectionOrb
import org.umbriel.link.ui.components.Eyebrow
import org.umbriel.link.ui.components.Glyph
import org.umbriel.link.ui.components.IconChip
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.NavRow
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.PullRefresh
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.components.SoftCard
import org.umbriel.link.ui.components.SwitchRow
import org.umbriel.link.ui.text
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Size
import org.umbriel.link.ui.theme.Space

/** Where the app opens: the primary desktop's status, sending to it, three quick actions, then settings. */
@Composable
fun HomeScreen(
    viewModel: HomeViewModel,
    picker: PickerViewModel,
    setupNeeded: Boolean,
    /** Both grants automatic clipboard needs are given, so Home stops offering to set it up. */
    autoClipboard: Boolean,
    onPair: () -> Unit,
    onDesktop: (Desktop) -> Unit,
    onMedia: () -> Unit,
    onNotifications: () -> Unit,
    onSetup: () -> Unit,
    onClipboardSetup: () -> Unit,
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val message by viewModel.message.collectAsStateWithLifecycle()
    val pick by picker.state.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val resources = context.resources
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
    LifecycleResumeEffect(Unit) {
        picker.refresh()
        onPauseOrDispose {}
    }
    val toast = when (val shown = message) {
        is HomeMessage.Paired -> resources.getString(R.string.paired_with, shown.name)
        is HomeMessage.Sent -> resources.getString(R.string.share_sent, shown.name)
        is HomeMessage.FilesSending -> resources.getString(R.string.share_files_sending, shown.name)
        HomeMessage.ClipboardEmpty -> resources.getString(R.string.clipboard_empty)
        is HomeMessage.ClipboardSent -> resources.getString(R.string.clipboard_sent, shown.name)
        is HomeMessage.Failed -> shown.failure.text(resources)
        null -> null
    }
    val primary = state.primary
    val send = {
        primary?.let { viewModel.sendFiles(it, pick.selected.keys.toList()) }
        picker.clear()
    }
    Screen(
        title = primary?.name ?: stringResource(R.string.desktops_title),
        toast = toast,
        onToastShown = viewModel::messageShown,
        actions = {
            if (primary != null) {
                val label = stringResource(R.string.desktop_settings)
                Box(
                    Modifier.size(Size.touch).clip(Radius.pill).clickable { onDesktop(primary) }
                        .semantics { contentDescription = label },
                    contentAlignment = Alignment.Center,
                ) { Glyph(Icons.Filled.Settings, LinkTheme.colors.textSecondary) }
            }
        },
        overlay = { if (primary != null) PickerSheet(picker, pick, primary.name, send) },
    ) {
        // Under the open picker, Home's photos and buttons would duplicate the sheet's for TalkBack.
        val hidden = if (pick.open) Modifier.clearAndSetSemantics {} else Modifier
        PullRefresh(state.refreshing, viewModel::refresh, Modifier.fillMaxSize().then(hidden)) {
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Space.page),
                verticalArrangement = Arrangement.spacedBy(Space.section),
            ) {
                if (primary == null) {
                    Empty(onPair)
                } else {
                    Status(primary)
                    if (setupNeeded) SetupBanner(onSetup)
                    SendCard(primary, pick, picker, send)
                    QuickActions(primary.id in state.ringing, onRing = { viewModel.ring(primary, primary.id !in state.ringing) }, onMedia) {
                        val clip = context.getSystemService(ClipboardManager::class.java).primaryClip
                            ?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(context)?.toString()
                        viewModel.sendClipboard(primary, clip)
                    }
                    SettingsCard(state, autoClipboard, viewModel, onNotifications, onSetup, onClipboardSetup, onPair)
                    if (state.others.isNotEmpty()) {
                        Column {
                            Eyebrow(stringResource(R.string.other_desktops), Modifier.padding(bottom = Space.s8))
                            state.others.forEach { desktop -> OtherDesktop(desktop) { onDesktop(desktop) } }
                        }
                    }
                }
                Spacer(Modifier.height(Space.s32))
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

/** One line under the title: a dot and whether the desktop is reachable now. */
@Composable
internal fun Status(desktop: Desktop) {
    val colors = LinkTheme.colors
    Row(Modifier.padding(start = Space.s4), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Space.s8)) {
        Box(Modifier.size(8.dp).clip(Radius.pill).background(if (desktop.connected) colors.success else colors.textTertiary))
        val status = when {
            desktop.connected && desktop.bluetooth -> stringResource(R.string.connected_bluetooth)
            desktop.connected -> stringResource(R.string.connected)
            else -> stringResource(R.string.last_seen, lastSeen(desktop.lastSeen))
        }
        Label(status, LinkTheme.type.titleMedium, colors.textSecondary)
    }
}

@Composable
private fun SetupBanner(onSetup: () -> Unit) {
    val colors = LinkTheme.colors
    Row(
        Modifier.fillMaxWidth().clip(Radius.list).background(colors.warning.copy(alpha = 0.12f)).clickable(onClick = onSetup)
            .padding(horizontal = Space.s16, vertical = Space.s12),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(Space.s12),
    ) {
        Glyph(Icons.Filled.Warning, colors.warning, Size.icon - 4.dp)
        Label(stringResource(R.string.setup_banner), LinkTheme.type.titleMedium, modifier = Modifier.weight(1f))
        Label(stringResource(R.string.setup_banner_action), LinkTheme.type.titleSmall, colors.accentText)
    }
}

/** The screen's lead: recent photos to tap, the picker's three tabs, and Send once anything is picked. */
@Composable
private fun SendCard(desktop: Desktop, pick: PickerState, picker: PickerViewModel, onSend: () -> Unit) {
    val colors = LinkTheme.colors
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { picker.refresh() }
    SoftCard(Modifier.fillMaxWidth(), padding = Space.s20) {
        Label(stringResource(R.string.picker_title, desktop.name), LinkTheme.type.headlineMedium, maxLines = 1)
        Label(stringResource(R.string.send_files_hint), LinkTheme.type.bodyMedium, colors.textSecondary)
        if (pick.access == MediaAccess.None) {
            Row(
                Modifier.padding(top = Space.s16).fillMaxWidth().clip(Radius.list).background(colors.surfaceTertiary)
                    .clickable { ask.launch(PhoneFiles.mediaPermissions) }.padding(Space.s16),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Label(stringResource(R.string.recent_allow), LinkTheme.type.bodyMedium, colors.textSecondary, Modifier.weight(1f))
                Label(stringResource(R.string.picker_media_allow), LinkTheme.type.titleSmall, colors.accentText)
            }
        } else if (pick.photos.isNotEmpty()) {
            LazyRow(Modifier.padding(top = Space.s16), horizontalArrangement = Arrangement.spacedBy(Space.s8)) {
                items(pick.photos.take(RECENT), key = { it.key }) { item ->
                    MediaTile(item, pick.selected.keys.indexOf(item.key), picker::thumbnail, Modifier.size(84.dp)) { picker.toggle(item) }
                }
            }
        }
        Row(Modifier.padding(top = Space.s16), horizontalArrangement = Arrangement.spacedBy(Space.s8)) {
            listOf(
                PickerTab.Photos to R.string.picker_photos,
                PickerTab.Videos to R.string.picker_videos,
                PickerTab.Files to R.string.picker_files,
            ).forEach { (tab, label) ->
                PillButton(stringResource(label), { picker.open(tab) }, Modifier.weight(1f), PillKind.Tonal)
            }
        }
        if (pick.selected.isNotEmpty()) {
            PillButton(sendLabel(pick.selected), onSend, Modifier.padding(top = Space.s12).fillMaxWidth())
        }
    }
}

@Composable
private fun QuickActions(ringing: Boolean, onRing: () -> Unit, onMedia: () -> Unit, onClipboard: () -> Unit) {
    Row(horizontalArrangement = Arrangement.spacedBy(Space.s12)) {
        ActionTile(stringResource(R.string.action_clipboard), Icons.Filled.Edit, onClipboard, Modifier.weight(1f))
        ActionTile(
            stringResource(if (ringing) R.string.ring_desktop_stop else R.string.action_ring),
            Icons.Filled.Notifications,
            onRing,
            Modifier.weight(1f),
            highlighted = ringing,
        )
        ActionTile(stringResource(R.string.action_media), Icons.Filled.PlayArrow, onMedia, Modifier.weight(1f))
    }
}

@Composable
private fun ActionTile(title: String, icon: ImageVector, onClick: () -> Unit, modifier: Modifier, highlighted: Boolean = false) {
    val colors = LinkTheme.colors
    SoftCard(modifier, onClick = onClick, padding = Space.s16) {
        Box(Modifier.align(Alignment.CenterHorizontally)) {
            IconChip(
                icon,
                tint = if (highlighted) colors.onAccent else colors.accentText,
                fill = if (highlighted) colors.accent else colors.accent.copy(alpha = 0.14f),
            )
        }
        Label(title, LinkTheme.type.titleMedium, modifier = Modifier.align(Alignment.CenterHorizontally).padding(top = Space.s10), maxLines = 1)
    }
}

@Composable
private fun SettingsCard(
    state: HomeState,
    autoClipboard: Boolean,
    viewModel: HomeViewModel,
    onNotifications: () -> Unit,
    onSetup: () -> Unit,
    onClipboardSetup: () -> Unit,
    onPair: () -> Unit,
) {
    SoftCard(Modifier.fillMaxWidth(), padding = Space.s16) {
        SwitchRow(
            title = stringResource(R.string.stay_connected),
            subtitle = stringResource(R.string.stay_connected_short),
            checked = state.stayConnected,
            onChange = viewModel::setStayConnected,
            icon = Icons.Filled.Refresh,
        )
        if (!autoClipboard) {
            NavRow(stringResource(R.string.clipboard_auto_row), stringResource(R.string.clipboard_auto_row_hint), Icons.Filled.Edit, onClipboardSetup)
        }
        NavRow(
            stringResource(R.string.notifications_row),
            stringResource(if (state.mirrorGranted) R.string.notifications_row_on else R.string.notifications_row_off),
            Icons.Filled.Notifications,
            onNotifications,
        )
        NavRow(stringResource(R.string.setup_title), stringResource(R.string.setup_row_hint), Icons.Filled.Lock, onSetup)
        NavRow(stringResource(R.string.pair_action), null, Icons.Filled.Add, onPair)
    }
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

private const val RECENT = 12
