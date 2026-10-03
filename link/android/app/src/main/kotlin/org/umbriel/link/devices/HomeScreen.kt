package org.umbriel.link.devices

import android.Manifest
import android.content.ClipboardManager
import android.content.pm.PackageManager
import android.os.Build
import android.text.format.DateUtils
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.text.BasicText
import androidx.compose.foundation.text.TextAutoSize
import androidx.compose.ui.unit.sp
import org.umbriel.link.ui.components.Node
import org.umbriel.link.ui.components.Readout
import org.umbriel.link.ui.components.TetherRow
import org.umbriel.link.ui.components.Wire
import org.umbriel.link.ui.components.WireMotion
import org.umbriel.link.ui.components.rememberWireMotion
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
import androidx.compose.material.icons.filled.Notifications
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.foundation.selection.selectable
import androidx.compose.ui.semantics.Role
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
import org.umbriel.link.ui.components.Glyph
import org.umbriel.link.ui.components.IconChip
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.LinkIcons
import org.umbriel.link.ui.components.NavRow
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.PullRefresh
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.components.DashboardCard
import org.umbriel.link.ui.components.DashboardSection
import org.umbriel.link.ui.components.StatusBadge
import org.umbriel.link.ui.text
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Size
import org.umbriel.link.ui.theme.Space

/** Home is the link itself: the desktop at the top of a line, this phone at the bottom, and every action a stop between. */
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
    onSetup: () -> Unit,
    onClipboardSetup: () -> Unit,
    onActivity: () -> Unit,
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
        title = stringResource(R.string.app_name),
        toast = toast,
        onToastShown = viewModel::messageShown,
        overlay = { if (primary != null) PickerSheet(picker, pick, primary.name, send) },
    ) {
        // Under the open picker, Home's photos and buttons would duplicate the sheet's for TalkBack.
        val hidden = if (pick.open) Modifier.clearAndSetSemantics {} else Modifier
        PullRefresh(state.refreshing, viewModel::refresh, Modifier.fillMaxSize().then(hidden)) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(top = Space.s16)) {
                if (primary == null) {
                    EmptyLine(onPair)
                    return@Column
                }
                val wire = primary.wire(state.refreshing)
                val motion = rememberWireMotion(state.activeTransfers > 0)
                val lit = wire == Wire.WiFi || wire == Wire.Bluetooth
                TetherRow(wire, motion, Node.Desktop, first = true, nodeAt = 30.dp) {
                    DesktopHeader(primary, state.refreshing, { viewModel.connect(primary) }) { onDesktop(primary) }
                    if (state.desktops.size > 1) DesktopSwitcher(state.desktops, primary, viewModel::selectDesktop)
                }
                TetherRow(wire, motion, Node.Dot, nodeAt = 40.dp) {
                    val ringing = primary.id in state.ringing
                    Row(Modifier.padding(top = Space.s8), horizontalArrangement = Arrangement.spacedBy(Space.s8)) {
                        QuickTile(LinkIcons.Clipboard, stringResource(R.string.action_clipboard), primary.sharing.clipboard, false, Modifier.weight(1f)) {
                            if (!primary.sharing.clipboard) onDesktop(primary)
                            else viewModel.sendClipboard(primary, context.getSystemService(ClipboardManager::class.java).primaryClip)
                        }
                        QuickTile(
                            LinkIcons.Ring, stringResource(if (ringing) R.string.ring_desktop_stop else R.string.action_ring),
                            primary.sharing.ring, ringing, Modifier.weight(1f),
                        ) { if (primary.sharing.ring) viewModel.ring(primary, !ringing) else onDesktop(primary) }
                        QuickTile(Icons.Filled.PlayArrow, stringResource(R.string.action_media), primary.sharing.media, false, Modifier.weight(1f)) {
                            if (primary.sharing.media) onMedia() else onDesktop(primary)
                        }
                    }
                }
                TetherRow(wire, motion, Node.Ring, nodeAt = 20.dp) {
                    if (primary.sharing.files) SendTray(primary, pick, picker, send)
                    else Label(stringResource(R.string.home_files_disabled), LinkTheme.type.headlineMedium)
                }
                TetherRow(wire, motion, Node.Phone, last = true, nodeAt = 30.dp, onClick = onActivity) {
                    Readout(
                        if (state.activeTransfers > 0) stringResource(R.string.readout_moving, state.activeTransfers)
                        else stringResource(R.string.readout_idle),
                        if (state.activeTransfers > 0 && lit) LinkTheme.colors.accentText else LinkTheme.colors.textTertiary,
                    )
                    Label(stringResource(R.string.home_this_phone), LinkTheme.type.headlineLarge, modifier = Modifier.padding(top = Space.s4))
                    Label(stringResource(R.string.home_activity_hint), LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
                    if (setupNeeded) SetupLine(stringResource(R.string.setup_banner), onSetup)
                    if (primary.sharing.clipboard && !autoClipboard) SetupLine(stringResource(R.string.home_clipboard_setup), onClipboardSetup)
                }
                Spacer(Modifier.height(Space.s32))
            }
        }
    }
}

private fun Desktop.wire(reaching: Boolean): Wire = when {
    connected && bluetooth -> Wire.Bluetooth
    connected -> Wire.WiFi
    reaching -> Wire.Searching
    else -> Wire.Down
}

/** Nothing paired: the line hangs from the phone toward an empty, dashed desktop, and scanning closes it. */
@Composable
private fun EmptyLine(onPair: () -> Unit) {
    val motion = rememberWireMotion(false)
    TetherRow(Wire.Searching, motion, Node.Empty, first = true, nodeAt = 30.dp) {
        Readout(stringResource(R.string.empty_desktop), LinkTheme.colors.textTertiary)
        Label(stringResource(R.string.empty_scan_title), LinkTheme.type.hero, modifier = Modifier.padding(top = Space.s8))
        Label(
            stringResource(R.string.empty_scan_body), LinkTheme.type.bodyLarge, LinkTheme.colors.textSecondary,
            Modifier.padding(top = Space.s12),
        )
        PillButton(stringResource(R.string.scan_action), onPair, Modifier.padding(top = Space.s24).fillMaxWidth())
        Spacer(Modifier.height(Space.s48))
    }
    TetherRow(Wire.Searching, motion, Node.Phone, last = true, nodeAt = 30.dp) {
        Readout(stringResource(R.string.readout_searching), LinkTheme.colors.textTertiary)
        Label(stringResource(R.string.home_this_phone), LinkTheme.type.headlineLarge, modifier = Modifier.padding(top = Space.s4))
        Label(stringResource(R.string.home_private_hint), LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
    }
}

/** The desktop as the line's top end: its transport as a readout, its name set large, and the one way forward. */
@Composable
internal fun DesktopHeader(desktop: Desktop, busy: Boolean = false, onConnect: () -> Unit, onOpen: (() -> Unit)?) {
    val colors = LinkTheme.colors
    val readout = when {
        desktop.connected && desktop.bluetooth -> stringResource(R.string.readout_bluetooth)
        desktop.connected -> stringResource(R.string.readout_wifi)
        busy -> stringResource(R.string.readout_searching)
        else -> stringResource(R.string.readout_offline, lastSeen(desktop.lastSeen))
    }
    Row(verticalAlignment = Alignment.Top) {
        Column(Modifier.weight(1f)) {
            Readout(readout, if (desktop.connected) colors.accentText else colors.textTertiary)
            Label(desktop.name, LinkTheme.type.hero, modifier = Modifier.padding(top = Space.s8), maxLines = 2)
        }
        if (onOpen != null) {
            val label = stringResource(R.string.desktop_open)
            Box(
                Modifier.size(Size.touch).clip(Radius.pill).border(1.dp, colors.borderPrimary, Radius.pill)
                    .clickable(role = Role.Button, onClick = onOpen).semantics { contentDescription = label },
                contentAlignment = Alignment.Center,
            ) { Glyph(Icons.Filled.Settings, colors.textSecondary, Size.icon - 4.dp) }
        }
    }
    if (!desktop.connected) {
        PillButton(stringResource(R.string.connect), onConnect, Modifier.padding(top = Space.s16), enabled = !busy)
    }
}

@Composable
private fun DesktopSwitcher(desktops: List<Desktop>, primary: Desktop, onSelect: (Desktop) -> Unit) {
    LazyRow(Modifier.padding(top = Space.s16), horizontalArrangement = Arrangement.spacedBy(Space.s8)) {
        items(desktops, key = { it.id }) { desktop ->
            val active = desktop.id == primary.id
            Box(
                Modifier.clip(Radius.pill)
                    .border(1.dp, if (active) LinkTheme.colors.accent else LinkTheme.colors.borderPrimary, Radius.pill)
                    .selectable(active, role = Role.Tab) { onSelect(desktop) }
                    .padding(horizontal = Space.s16, vertical = Space.s10),
            ) { Label(desktop.name, LinkTheme.type.titleSmall, maxLines = 1) }
        }
    }
}

/** Something left to set up, at the phone's end of the line: a warning dot, what it is, and a way there. */
@Composable
private fun SetupLine(text: String, onClick: () -> Unit) {
    Row(
        Modifier.padding(top = Space.s8).clip(Radius.pill).clickable(role = Role.Button, onClick = onClick)
            .padding(vertical = Space.s8),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(Space.s8),
    ) {
        Box(Modifier.size(8.dp).clip(Radius.pill).background(LinkTheme.colors.warning))
        Label(text, LinkTheme.type.titleMedium)
        Glyph(Icons.AutoMirrored.Filled.KeyboardArrowRight, LinkTheme.colors.textTertiary)
    }
}

/** One of the three things you do to the desktop in a tap; lit while it is happening, dimmed when not shared. */
@Composable
private fun QuickTile(icon: ImageVector, label: String, shared: Boolean, active: Boolean, modifier: Modifier, onClick: () -> Unit) {
    val colors = LinkTheme.colors
    Column(
        modifier.clip(Radius.list)
            .background(if (active) colors.accent else colors.surfaceTertiary)
            .clickable(role = Role.Button, onClick = onClick)
            .alpha(if (shared) 1f else 0.45f)
            .padding(Space.s14),
        verticalArrangement = Arrangement.spacedBy(Space.s20),
    ) {
        Glyph(icon, if (active) colors.onAccent else colors.textPrimary, Size.icon - 2.dp)
        // Large font scales would clip the label in a third of the width, so it shrinks to fit instead.
        BasicText(
            label, style = LinkTheme.type.titleMedium.copy(color = if (active) colors.onAccent else colors.textPrimary),
            maxLines = 1, autoSize = TextAutoSize.StepBased(minFontSize = 10.sp, maxFontSize = LinkTheme.type.titleMedium.fontSize),
        )
    }
}

/** Where things leave the phone: recent photos to tap, the picker's tabs, and Send once anything is picked. */
@Composable
private fun SendTray(desktop: Desktop, pick: PickerState, picker: PickerViewModel, onSend: () -> Unit) {
    val colors = LinkTheme.colors
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { picker.refresh() }
    Label(stringResource(R.string.home_send_title), LinkTheme.type.headlineMedium)
    Label(stringResource(R.string.home_send_target, desktop.name), LinkTheme.type.bodyMedium, colors.textSecondary)
    if (pick.access == MediaAccess.None) {
        Row(
            Modifier.padding(top = Space.s12).fillMaxWidth().clip(Radius.list).background(colors.surfaceTertiary)
                .clickable { ask.launch(PhoneFiles.mediaPermissions) }.padding(Space.s16),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Label(stringResource(R.string.recent_allow), LinkTheme.type.bodyMedium, colors.textSecondary, Modifier.weight(1f))
            Label(stringResource(R.string.picker_media_allow), LinkTheme.type.titleSmall, colors.accentText)
        }
    } else if (pick.photos.isNotEmpty()) {
        LazyRow(Modifier.padding(top = Space.s12), horizontalArrangement = Arrangement.spacedBy(Space.s6)) {
            items(pick.photos.take(RECENT), key = { it.key }) { item ->
                MediaTile(item, pick.selected.keys.indexOf(item.key), picker::thumbnail, Modifier.size(76.dp)) { picker.toggle(item) }
            }
        }
    }
    Row(Modifier.padding(top = Space.s12), horizontalArrangement = Arrangement.spacedBy(Space.s8)) {
        listOf(
            PickerTab.Photos to R.string.picker_photos,
            PickerTab.Videos to R.string.picker_videos,
            PickerTab.Files to R.string.picker_files,
        ).forEach { (tab, label) ->
            PillButton(stringResource(label), { picker.open(tab) }, Modifier.weight(1f), PillKind.Tonal)
        }
    }
    if (pick.selected.isNotEmpty()) {
        PillButton(sendLabel(pick.selected), onSend, Modifier.padding(top = Space.s12).fillMaxWidth(), icon = LinkIcons.Upload)
    }
}

@Composable
private fun lastSeen(unixSeconds: Long): String =
    if (unixSeconds <= 0) stringResource(R.string.last_seen_never) else DateUtils.getRelativeTimeSpanString(unixSeconds * 1000).toString()

private const val RECENT = 12
