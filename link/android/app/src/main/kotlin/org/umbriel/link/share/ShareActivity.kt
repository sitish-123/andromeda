package org.umbriel.link.share

import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import org.umbriel.link.LinkApplication
import org.umbriel.link.R
import org.umbriel.link.ui.LinkTheme
import org.umbriel.link.ui.text

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
    when (val current = state) {
        ShareState.Loading -> Progress(stringResource(R.string.share_preparing))
        is ShareState.Sending -> Progress(stringResource(R.string.share_sending, current.desktop.name))
        is ShareState.Choose -> AlertDialog(
            onDismissRequest = viewModel::cancel,
            title = { Text(stringResource(R.string.share_choose)) },
            text = {
                Column {
                    current.desktops.forEach { desktop ->
                        TextButton(onClick = { viewModel.send(desktop) }) { Text(desktop.name) }
                    }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = viewModel::cancel) { Text(stringResource(R.string.cancel)) } },
        )
    }
}

@Composable
private fun Progress(label: String) {
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally) {
        Surface(shape = MaterialTheme.shapes.large, tonalElevation = 6.dp) {
            Row(
                modifier = Modifier.padding(24.dp),
                horizontalArrangement = Arrangement.spacedBy(16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                CircularProgressIndicator()
                Text(label)
            }
        }
    }
}
