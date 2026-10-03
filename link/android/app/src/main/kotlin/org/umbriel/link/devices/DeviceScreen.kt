package org.umbriel.link.devices

import androidx.annotation.StringRes
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.Notifications
import androidx.compose.material.icons.filled.Phone
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Lock
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import android.content.Intent
import android.provider.Settings
import org.umbriel.link.screen.ScreenInput
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.window.Dialog
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.compose.LifecycleResumeEffect
import org.umbriel.link.R
import org.umbriel.link.core.domain.Feature
import org.umbriel.link.ui.components.Glyph
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.LinkIcons
import org.umbriel.link.ui.components.NavRow
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.components.SwitchRow
import org.umbriel.link.ui.components.DashboardCard
import org.umbriel.link.ui.components.DashboardSection
import org.umbriel.link.ui.text
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Size
import org.umbriel.link.ui.theme.Space

/** One desktop: what this phone shares with it, the per-app notification filter, and unpairing. */
@Composable
fun DeviceScreen(viewModel: DeviceViewModel, onBack: () -> Unit, onMirrorApps: () -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val unpaired by viewModel.unpaired.collectAsStateWithLifecycle()
    var confirmUnpair by rememberSaveable { mutableStateOf(false) }
    var controlEnabled by rememberSaveable { mutableStateOf(ScreenInput.enabled) }
    LifecycleResumeEffect(Unit) {
        controlEnabled = ScreenInput.enabled
        onPauseOrDispose {}
    }
    val resources = LocalContext.current.resources
    LaunchedEffect(unpaired) { if (unpaired) onBack() }
    val desktop = state.desktop ?: return Screen(stringResource(R.string.desktops_title), onBack) {}
    Screen(
        title = stringResource(R.string.desktop_settings),
        onBack = onBack,
        toast = state.failure?.text(resources),
        onToastShown = viewModel::failureShown,
    ) {
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Space.page),
            verticalArrangement = Arrangement.spacedBy(Space.s24),
        ) {
            DesktopHeader(desktop, state.busy, viewModel::connect, onOpen = null)
            DashboardSection(stringResource(R.string.sharing_title), stringResource(R.string.device_hint))
            DashboardCard(Modifier.fillMaxWidth()) {
                val sharing = desktop.sharing
                listOf(
                    FeatureRow(Feature.Clipboard, R.string.feature_clipboard, R.string.feature_clipboard_hint, Icons.Filled.Edit),
                    FeatureRow(Feature.Files, R.string.feature_files, R.string.feature_files_hint, Icons.Filled.Share),
                    FeatureRow(Feature.Notifications, R.string.feature_notifications, R.string.feature_notifications_hint, Icons.Filled.Notifications),
                    FeatureRow(Feature.Media, R.string.feature_media, R.string.feature_media_hint, Icons.Filled.PlayArrow),
                    FeatureRow(Feature.Ring, R.string.feature_ring, R.string.feature_ring_hint, Icons.Filled.Phone),
                    FeatureRow(Feature.Calls, R.string.feature_calls, R.string.feature_calls_hint, Icons.Filled.Call),
                ).forEach { row ->
                    SwitchRow(
                        title = stringResource(row.label),
                        subtitle = stringResource(row.hint),
                        checked = sharing.allows(row.feature),
                        onChange = { viewModel.setSharing(row.feature, it) },
                        icon = row.icon,
                        enabled = !state.busy,
                    )
                    if (row.feature == Feature.Notifications && sharing.allows(Feature.Notifications)) {
                        Row(
                            Modifier.fillMaxWidth().clip(Radius.list).clickable(onClick = onMirrorApps)
                                .padding(start = Size.iconContainer + Space.s12, top = Space.s4, bottom = Space.s8),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Label(stringResource(R.string.mirror_apps_choose), LinkTheme.type.titleSmall, LinkTheme.colors.accentText, Modifier.weight(1f))
                            Glyph(Icons.AutoMirrored.Filled.KeyboardArrowRight, LinkTheme.colors.accentText)
                        }
                    }
                }
            }
            DashboardSection(stringResource(R.string.settings_advanced), stringResource(R.string.settings_advanced_hint))
            DashboardCard(Modifier.fillMaxWidth()) {
                SwitchRow(
                    stringResource(R.string.feature_browse), desktop.sharing.browse,
                    { viewModel.setSharing(Feature.Browse, it) },
                    subtitle = stringResource(R.string.feature_browse_hint), icon = LinkIcons.Folder, enabled = !state.busy,
                )
                SwitchRow(
                    stringResource(R.string.feature_screen), desktop.sharing.screen,
                    { viewModel.setSharing(Feature.Screen, it) },
                    subtitle = stringResource(R.string.feature_screen_hint), icon = LinkIcons.Computer, enabled = !state.busy,
                )
                if (desktop.sharing.screen && !controlEnabled) {
                    val context = LocalContext.current
                    NavRow(
                        stringResource(R.string.screen_control_off), null, Icons.Filled.Lock,
                        { context.startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)) },
                    )
                }
            }
            DashboardCard(Modifier.fillMaxWidth()) {
                Label(stringResource(R.string.device_remove_title), LinkTheme.type.titleLarge)
                Label(stringResource(R.string.device_remove_hint), LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
                PillButton(
                    stringResource(R.string.unpair), { confirmUnpair = true },
                    kind = PillKind.Destructive, enabled = !state.busy, modifier = Modifier.fillMaxWidth(),
                )
            }
            Spacer(Modifier.height(Space.s32))
        }
        if (confirmUnpair) {
            Dialog(onDismissRequest = { if (!state.busy) confirmUnpair = false }) {
                DashboardCard(Modifier.fillMaxWidth()) {
                    Label(stringResource(R.string.device_unpair_title), LinkTheme.type.headlineLarge)
                    Label(stringResource(R.string.device_unpair_body, desktop.name), LinkTheme.type.bodyLarge, LinkTheme.colors.textSecondary)
                    PillButton(
                        stringResource(R.string.unpair), viewModel::unpair, Modifier.fillMaxWidth(),
                        PillKind.Destructive, enabled = !state.busy,
                    )
                    PillButton(
                        stringResource(R.string.cancel), { confirmUnpair = false }, Modifier.fillMaxWidth(),
                        PillKind.Quiet, enabled = !state.busy,
                    )
                }
            }
        }
    }
}

private data class FeatureRow(val feature: Feature, @param:StringRes val label: Int, @param:StringRes val hint: Int, val icon: ImageVector)
