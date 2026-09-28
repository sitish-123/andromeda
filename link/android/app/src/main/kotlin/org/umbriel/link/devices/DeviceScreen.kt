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
import androidx.compose.material.icons.filled.Face
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.Notifications
import androidx.compose.material.icons.filled.Phone
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Search
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import android.content.Intent
import android.provider.Settings
import org.umbriel.link.screen.ScreenInput
import androidx.compose.ui.res.stringResource
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.umbriel.link.R
import org.umbriel.link.core.domain.Feature
import org.umbriel.link.ui.components.Eyebrow
import org.umbriel.link.ui.components.Glyph
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.components.SoftCard
import org.umbriel.link.ui.components.SwitchRow
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
    val resources = LocalContext.current.resources
    LaunchedEffect(unpaired) { if (unpaired) onBack() }
    val desktop = state.desktop ?: return Screen(stringResource(R.string.desktops_title), onBack) {}
    Screen(
        title = desktop.name,
        onBack = onBack,
        toast = state.failure?.text(resources),
        onToastShown = viewModel::failureShown,
    ) {
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Space.page),
            verticalArrangement = Arrangement.spacedBy(Space.section),
        ) {
            Status(desktop)
            if (!desktop.connected) {
                PillButton(stringResource(R.string.connect), viewModel::connect, Modifier.fillMaxWidth(), enabled = !state.busy)
            }
            SoftCard(Modifier.fillMaxWidth(), padding = Space.s16) {
                Eyebrow(stringResource(R.string.sharing_title))
                Label(
                    stringResource(R.string.device_hint),
                    LinkTheme.type.bodyMedium,
                    LinkTheme.colors.textSecondary,
                    Modifier.padding(bottom = Space.s4),
                )
                val sharing = desktop.sharing
                listOf(
                    FeatureRow(Feature.Clipboard, R.string.feature_clipboard, R.string.feature_clipboard_hint, Icons.Filled.Edit),
                    FeatureRow(Feature.Files, R.string.feature_files, R.string.feature_files_hint, Icons.Filled.Share),
                    FeatureRow(Feature.Notifications, R.string.feature_notifications, R.string.feature_notifications_hint, Icons.Filled.Notifications),
                    FeatureRow(Feature.Media, R.string.feature_media, R.string.feature_media_hint, Icons.Filled.PlayArrow),
                    FeatureRow(Feature.Ring, R.string.feature_ring, R.string.feature_ring_hint, Icons.Filled.Phone),
                    FeatureRow(Feature.Calls, R.string.feature_calls, R.string.feature_calls_hint, Icons.Filled.Call),
                    FeatureRow(Feature.Browse, R.string.feature_browse, R.string.feature_browse_hint, Icons.Filled.Search),
                    FeatureRow(Feature.Screen, R.string.feature_screen, R.string.feature_screen_hint, Icons.Filled.Face),
                ).forEach { row ->
                    SwitchRow(
                        title = stringResource(row.label),
                        subtitle = stringResource(row.hint),
                        checked = sharing.allows(row.feature),
                        onChange = { viewModel.setSharing(row.feature, it) },
                        icon = row.icon,
                    )
                    if (row.feature == Feature.Screen && sharing.allows(Feature.Screen) && !ScreenInput.enabled) {
                        val context = LocalContext.current
                        Row(
                            Modifier.fillMaxWidth().clip(Radius.list)
                                .clickable { context.startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)) }
                                .padding(start = Size.iconContainer + Space.s12, top = Space.s4, bottom = Space.s8),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Label(stringResource(R.string.screen_control_off), LinkTheme.type.titleSmall, LinkTheme.colors.accentText, Modifier.weight(1f))
                            Glyph(Icons.AutoMirrored.Filled.KeyboardArrowRight, LinkTheme.colors.accentText)
                        }
                    }
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
            PillButton(
                stringResource(R.string.unpair),
                viewModel::unpair,
                kind = PillKind.Destructive,
                enabled = !state.busy,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(Space.s32))
        }
    }
}

private data class FeatureRow(val feature: Feature, @param:StringRes val label: Int, @param:StringRes val hint: Int, val icon: ImageVector)
