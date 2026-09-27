package org.umbriel.link.ui

import androidx.activity.compose.BackHandler
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import kotlinx.coroutines.flow.StateFlow
import org.umbriel.link.AppContainer
import org.umbriel.link.devices.DevicesScreen
import org.umbriel.link.devices.DevicesViewModel
import org.umbriel.link.notifications.AppFilterScreen
import org.umbriel.link.notifications.AppFilterViewModel
import org.umbriel.link.pairing.PairingScreen
import org.umbriel.link.pairing.PairingViewModel

private enum class Screen { Devices, Pairing, MirrorApps }

/** A few screens, so navigation is one piece of state rather than a navigation library. */
@Composable
fun LinkApp(container: AppContainer, pairingLink: StateFlow<String?>, onLinkHandled: () -> Unit) {
    var screen by rememberSaveable { mutableStateOf(Screen.Devices) }
    val link by pairingLink.collectAsStateWithLifecycle()
    val devices = viewModel { DevicesViewModel(container.repository, container.presence, container.mirror) }
    val pairing = viewModel { PairingViewModel(container.repository) }

    LaunchedEffect(link) {
        link?.let {
            pairing.offerLink(it)
            onLinkHandled()
            screen = Screen.Pairing
        }
    }
    when (screen) {
        Screen.Devices -> DevicesScreen(devices, onPair = { screen = Screen.Pairing }, onMirrorApps = { screen = Screen.MirrorApps })
        Screen.MirrorApps -> {
            BackHandler { screen = Screen.Devices }
            val context = LocalContext.current
            val filter = viewModel { AppFilterViewModel(container.mirror, context.packageManager) }
            AppFilterScreen(filter, onBack = { screen = Screen.Devices })
        }
        Screen.Pairing -> {
            BackHandler { screen = Screen.Devices }
            PairingScreen(
                viewModel = pairing,
                onBack = { screen = Screen.Devices },
                onPaired = { desktop ->
                    container.presence.paired()
                    devices.announce(desktop)
                    screen = Screen.Devices
                },
            )
        }
    }
}
