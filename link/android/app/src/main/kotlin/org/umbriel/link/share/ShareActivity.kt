package org.umbriel.link.share

import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import org.umbriel.link.LinkApplication
import org.umbriel.link.R
import org.umbriel.link.ui.components.ConnectionOrb
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.SoftCard
import org.umbriel.link.ui.text
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Space

/** The share target for text and links from any app. It shows only a chooser or progress, then a toast. */
class ShareActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val text = intent.takeIf { it.action == Intent.ACTION_SEND }?.getStringExtra(Intent.EXTRA_TEXT)
        if (text.isNullOrEmpty()) {
            finish()
            return
        }
        val repository = (application as LinkApplication).container.repository
        setContent {
            LinkTheme {
                val viewModel = viewModel { ShareViewModel(repository, text) }
                ShareScreen(viewModel, onDone = { message ->
                    message?.let { Toast.makeText(this, it, Toast.LENGTH_LONG).show() }
                    finish()
                })
            }
        }
    }
}

@Composable
private fun ShareScreen(viewModel: ShareViewModel, onDone: (String?) -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val resources = LocalContext.current.resources
    LaunchedEffect(viewModel) {
        viewModel.outcome.collect { outcome ->
            onDone(
                when (outcome) {
                    is ShareOutcome.Sent -> resources.getString(R.string.share_sent, outcome.desktop.name)
                    ShareOutcome.NoDesktop -> resources.getString(R.string.share_no_desktop)
                    ShareOutcome.Cancelled -> null
                    is ShareOutcome.Failed -> resources.getString(R.string.share_failed, outcome.failure.text(resources))
                },
            )
        }
    }
    val colors = LinkTheme.colors
    Box(
        Modifier.fillMaxSize().background(colors.surfaceOverlay)
            .clickable(remember { MutableInteractionSource() }, indication = null, onClick = viewModel::cancel),
        contentAlignment = Alignment.BottomCenter,
    ) {
        // The card takes its own taps, so only the scrim around it cancels.
        val card = remember { MutableInteractionSource() }
        SoftCard(Modifier.fillMaxWidth().navigationBarsPadding().padding(Space.s12).clickable(card, indication = null) {}) {
            when (val current = state) {
                ShareState.Loading -> Progress(stringResource(R.string.share_preparing))
                is ShareState.Sending -> Progress(stringResource(R.string.share_sending, current.desktop.name))
                is ShareState.Choose -> {
                    Label(stringResource(R.string.share_choose), LinkTheme.type.headlineMedium)
                    Column(Modifier.padding(top = Space.s16), verticalArrangement = Arrangement.spacedBy(Space.s8)) {
                        current.desktops.forEach { desktop ->
                            PillButton(desktop.name, { viewModel.send(desktop) }, Modifier.fillMaxWidth(), PillKind.Tonal)
                        }
                        PillButton(stringResource(R.string.cancel), viewModel::cancel, Modifier.fillMaxWidth(), PillKind.Quiet)
                    }
                }
            }
        }
    }
}

@Composable
private fun Progress(label: String) {
    Row(horizontalArrangement = Arrangement.spacedBy(Space.s16), verticalAlignment = Alignment.CenterVertically) {
        ConnectionOrb(active = true, working = true, size = 48.dp)
        Label(label, LinkTheme.type.titleLarge)
    }
}
