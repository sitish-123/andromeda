package org.umbriel.link.devices

import android.os.Build
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Notifications
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.umbriel.link.R
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.ui.components.DashboardCard
import org.umbriel.link.ui.components.Footnote
import org.umbriel.link.ui.components.LinkIcons
import org.umbriel.link.ui.components.Readout
import org.umbriel.link.ui.components.DashboardSection
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.NavRow
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.components.SlidingPillControl
import org.umbriel.link.ui.components.SwitchRow
import org.umbriel.link.ui.theme.AppTheme
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Space
import org.umbriel.link.ui.theme.rememberAppTheme
import org.umbriel.link.ui.theme.setAppTheme

@Composable
fun SettingsScreen(
    viewModel: HomeViewModel,
    autoClipboard: Boolean,
    onDesktop: (Desktop) -> Unit,
    onPair: () -> Unit,
    onNotifications: () -> Unit,
    onSetup: () -> Unit,
    onClipboardSetup: () -> Unit,
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val theme by rememberAppTheme()
    val context = LocalContext.current
    val version = remember(context) {
        context.packageManager.getPackageInfo(context.packageName, 0).versionName.orEmpty()
    }
    Screen(stringResource(R.string.settings_title)) {
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Space.page),
            verticalArrangement = Arrangement.spacedBy(Space.s12),
        ) {
            DashboardSection(stringResource(R.string.settings_devices))
            DashboardCard(Modifier.fillMaxWidth()) {
                state.desktops.forEach { desktop ->
                    NavRow(
                        desktop.name,
                        stringResource(if (desktop.connected) R.string.connected else R.string.not_connected),
                        LinkIcons.Computer, { onDesktop(desktop) },
                    )
                }
                NavRow(stringResource(R.string.pair_action), null, Icons.Filled.Add, onPair)
            }
            Footnote(stringResource(R.string.settings_devices_hint))
            DashboardSection(stringResource(R.string.settings_connection))
            DashboardCard(Modifier.fillMaxWidth()) {
                SwitchRow(
                    title = stringResource(R.string.stay_connected),
                    subtitle = stringResource(R.string.stay_connected_hint),
                    checked = state.stayConnected,
                    onChange = viewModel::setStayConnected,
                    icon = Icons.Filled.Refresh,
                )
            }
            Footnote(stringResource(R.string.settings_connection_hint))
            DashboardSection(stringResource(R.string.settings_sharing))
            DashboardCard(Modifier.fillMaxWidth()) {
                NavRow(
                    stringResource(R.string.clipboard_auto_row),
                    stringResource(if (autoClipboard) R.string.settings_clipboard_configured else R.string.clipboard_auto_row_hint),
                    Icons.Filled.Edit, onClipboardSetup,
                )
                NavRow(
                    stringResource(R.string.notifications_row),
                    stringResource(if (state.mirrorGranted) R.string.notifications_row_on else R.string.notifications_row_off),
                    Icons.Filled.Notifications, onNotifications,
                )
                NavRow(stringResource(R.string.setup_title), stringResource(R.string.setup_row_hint), Icons.Filled.Lock, onSetup)
            }
            DashboardSection(stringResource(R.string.settings_appearance))
            SlidingPillControl(
                listOf(stringResource(R.string.theme_system), stringResource(R.string.theme_light), stringResource(R.string.theme_dark)),
                theme.ordinal,
                { setAppTheme(context, AppTheme.entries[it]) },
            )
            Readout(
                "${stringResource(R.string.app_name)} $version · ${Build.MODEL}",
                LinkTheme.colors.textTertiary,
                Modifier.fillMaxWidth().padding(top = Space.s32),
            )
            Footnote(stringResource(R.string.home_private_hint))
            Spacer(Modifier.height(Space.s16))
        }
    }
}
