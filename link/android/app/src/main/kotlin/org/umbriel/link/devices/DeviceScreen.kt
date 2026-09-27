package org.umbriel.link.devices

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
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
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.Notifications
import androidx.compose.material.icons.filled.Phone
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.umbriel.link.R
import org.umbriel.link.core.domain.Feature
import org.umbriel.link.ui.components.ConnectionOrb
import org.umbriel.link.ui.components.Eyebrow
import org.umbriel.link.ui.components.Glyph
import org.umbriel.link.ui.components.HeroSurface
import org.umbriel.link.ui.components.IconChip
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.components.SelectablePill
import org.umbriel.link.ui.components.SoftCard
import org.umbriel.link.ui.text
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Space

/** One desktop: what this phone shares with it, as selectable pills, the per-app filter, and unpairing. */
@OptIn(ExperimentalLayoutApi::class)
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
            HeroSurface(Modifier.fillMaxWidth()) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Space.s16)) {
                    ConnectionOrb(active = desktop.connected, working = state.busy, size = 56.dp)
                    Column(Modifier.weight(1f)) {
                        val status = if (desktop.connected) R.string.connected else R.string.not_connected
                        Label(stringResource(status), LinkTheme.type.headlineMedium)
                        Label(stringResource(R.string.device_hint), LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
                    }
                }
                if (!desktop.connected) {
                    PillButton(
                        stringResource(R.string.connect),
                        viewModel::connect,
                        enabled = !state.busy,
                        modifier = Modifier.padding(top = Space.s16),
                    )
                }
            }
            SoftCard(Modifier.fillMaxWidth()) {
                Eyebrow(stringResource(R.string.sharing_title))
                Label(stringResource(R.string.sharing_hint), LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
                FlowRow(
                    Modifier.padding(top = Space.s8),
                    horizontalArrangement = Arrangement.spacedBy(Space.s8),
                ) {
                    val sharing = desktop.sharing
                    listOf(
                        Triple(Feature.Notifications, R.string.feature_notifications, Icons.Filled.Notifications),
                        Triple(Feature.Media, R.string.feature_media, Icons.Filled.PlayArrow),
                        Triple(Feature.Ring, R.string.feature_ring, Icons.Filled.Phone),
                        Triple(Feature.Calls, R.string.feature_calls, Icons.Filled.Call),
                    ).forEach { (feature, label, icon) ->
                        val on = sharing.allows(feature)
                        SelectablePill(
                            stringResource(label),
                            selected = on,
                            onClick = { viewModel.setSharing(feature, !on) },
                            icon = if (on) Icons.Filled.Check else icon,
                        )
                    }
                }
            }
            SoftCard(Modifier.fillMaxWidth(), onClick = onMirrorApps, padding = Space.s16) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Space.s12)) {
                    IconChip(Icons.AutoMirrored.Filled.List)
                    Column(Modifier.weight(1f)) {
                        Label(stringResource(R.string.mirror_apps_title), LinkTheme.type.titleLarge)
                        Label(stringResource(R.string.mirror_apps_hint), LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
                    }
                    Glyph(Icons.AutoMirrored.Filled.KeyboardArrowRight, LinkTheme.colors.textTertiary)
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
