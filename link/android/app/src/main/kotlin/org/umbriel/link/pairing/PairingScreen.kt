package org.umbriel.link.pairing

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.umbriel.link.R
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.ui.text

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PairingScreen(viewModel: PairingViewModel, onBack: () -> Unit, onPaired: (Desktop) -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    LaunchedEffect(viewModel) { viewModel.paired.collect(onPaired) }
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.pair_title)) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.back))
                    }
                },
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier.padding(padding).fillMaxSize().padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            if (state.link != null) LinkConfirmation(state, viewModel) else CodeEntry(state, viewModel)
            state.failure?.let {
                Text(it.text(LocalContext.current.resources), color = MaterialTheme.colorScheme.error)
            }
            if (state.busy) CircularProgressIndicator()
        }
    }
}

@Composable
private fun LinkConfirmation(state: PairingState, viewModel: PairingViewModel) {
    Text(stringResource(R.string.pair_link_title), style = MaterialTheme.typography.headlineSmall)
    Text(stringResource(R.string.pair_link_body), style = MaterialTheme.typography.bodyLarge)
    Button(onClick = viewModel::pairWithLink, enabled = !state.busy, modifier = Modifier.fillMaxWidth()) {
        Text(stringResource(R.string.pair_button))
    }
}

@Composable
private fun CodeEntry(state: PairingState, viewModel: PairingViewModel) {
    Text(stringResource(R.string.pair_instructions), style = MaterialTheme.typography.bodyLarge)
    OutlinedTextField(
        value = state.code,
        onValueChange = viewModel::updateCode,
        label = { Text(stringResource(R.string.pair_code_label)) },
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword),
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )
    Button(onClick = viewModel::pairWithCode, enabled = state.canPairCode, modifier = Modifier.fillMaxWidth()) {
        Text(stringResource(R.string.pair_button))
    }
}
