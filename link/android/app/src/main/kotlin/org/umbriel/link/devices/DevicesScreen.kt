package org.umbriel.link.devices

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.umbriel.link.R
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.ui.text

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DevicesScreen(viewModel: DevicesViewModel, onPair: () -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val snackbar = remember { SnackbarHostState() }
    val context = LocalContext.current
    val resources = context.resources
    val notificationPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {}
    val paired = state.desktops.isNotEmpty()
    // Asked once a desktop is paired, the first moment one can send this phone anything to notify about.
    LaunchedEffect(paired) {
        val needed = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        if (paired && needed) notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
    }
    LaunchedEffect(viewModel) {
        viewModel.messages.collect { message ->
            val text = when (message) {
                is DevicesMessage.Paired -> resources.getString(R.string.paired_with, message.name)
                is DevicesMessage.Unpaired -> resources.getString(R.string.unpaired_from, message.name)
                is DevicesMessage.Failed -> message.failure.text(resources)
            }
            snackbar.showSnackbar(text)
        }
    }
    Scaffold(
        topBar = { TopAppBar(title = { Text(stringResource(R.string.desktops_title)) }) },
        snackbarHost = { SnackbarHost(snackbar) },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                onClick = onPair,
                icon = { Icon(Icons.Filled.Add, contentDescription = null) },
                text = { Text(stringResource(R.string.pair_action)) },
            )
        },
    ) { padding ->
        if (state.desktops.isEmpty()) {
            EmptyDesktops(Modifier.padding(padding))
        } else {
            DesktopList(state, viewModel, PaddingValues(16.dp), Modifier.padding(padding))
        }
    }
}

@Composable
private fun EmptyDesktops(modifier: Modifier) {
    Column(
        modifier = modifier.fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(stringResource(R.string.desktops_empty), style = MaterialTheme.typography.titleLarge)
        Text(
            stringResource(R.string.desktops_empty_hint),
            style = MaterialTheme.typography.bodyMedium,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(top = 8.dp),
        )
    }
}

@Composable
private fun DesktopList(state: DevicesState, viewModel: DevicesViewModel, content: PaddingValues, modifier: Modifier) {
    LazyColumn(modifier = modifier.fillMaxSize(), contentPadding = content, verticalArrangement = Arrangement.spacedBy(12.dp)) {
        item(key = "stay-connected") { StayConnected(state.stayConnected, viewModel::setStayConnected) }
        items(state.desktops, key = { it.id }) { desktop ->
            DesktopCard(desktop, busy = desktop.id in state.busy, viewModel)
        }
    }
}

@Composable
private fun StayConnected(checked: Boolean, onChange: (Boolean) -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.toggleable(value = checked, role = Role.Switch, onValueChange = onChange).padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.stay_connected), style = MaterialTheme.typography.titleMedium)
                Text(stringResource(R.string.stay_connected_hint), style = MaterialTheme.typography.bodyMedium)
            }
            Switch(checked = checked, onCheckedChange = null)
        }
    }
}

@Composable
private fun DesktopCard(desktop: Desktop, busy: Boolean, viewModel: DevicesViewModel) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(desktop.name, style = MaterialTheme.typography.titleMedium)
            val status = if (desktop.connected) R.string.connected else R.string.not_connected
            Text(stringResource(status), style = MaterialTheme.typography.bodyMedium)
            Row(modifier = Modifier.padding(top = 12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (!desktop.connected) {
                    FilledTonalButton(onClick = { viewModel.connect(desktop) }, enabled = !busy) {
                        Text(stringResource(R.string.connect))
                    }
                }
                TextButton(onClick = { viewModel.unpair(desktop) }, enabled = !busy) {
                    Text(stringResource(R.string.unpair))
                }
            }
        }
    }
}
