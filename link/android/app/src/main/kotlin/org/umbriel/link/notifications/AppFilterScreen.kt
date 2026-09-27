package org.umbriel.link.notifications

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.umbriel.link.R
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.components.SoftCard
import org.umbriel.link.ui.components.SwitchRow
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Space

/** Every launcher app with a switch; all are mirrored until the user turns one off. */
@Composable
fun AppFilterScreen(viewModel: AppFilterViewModel, onBack: () -> Unit) {
    val apps by viewModel.state.collectAsStateWithLifecycle()
    Screen(title = stringResource(R.string.mirror_apps_title), onBack = onBack) {
        LazyColumn(
            Modifier.fillMaxSize(),
            contentPadding = PaddingValues(horizontal = Space.page, vertical = Space.s8),
            verticalArrangement = Arrangement.spacedBy(Space.s4),
        ) {
            item {
                Label(stringResource(R.string.mirror_apps_hint), LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
            }
            item {
                SoftCard(Modifier.fillMaxWidth(), padding = Space.s12) {
                    apps.forEach { app ->
                        SwitchRow(app.label, app.mirrored, { viewModel.setMirrored(app.packageName, it) })
                    }
                }
            }
        }
    }
}
