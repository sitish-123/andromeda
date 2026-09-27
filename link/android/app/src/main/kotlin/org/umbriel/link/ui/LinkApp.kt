package org.umbriel.link.ui

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
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
import org.umbriel.link.devices.DeviceScreen
import org.umbriel.link.devices.DeviceViewModel
import org.umbriel.link.devices.HomeScreen
import org.umbriel.link.devices.HomeViewModel
import org.umbriel.link.media.MediaScreen
import org.umbriel.link.media.MediaViewModel
import org.umbriel.link.notifications.AppFilterScreen
import org.umbriel.link.notifications.AppFilterViewModel
import org.umbriel.link.onboarding.Grant
import org.umbriel.link.onboarding.OnboardingScreen
import org.umbriel.link.onboarding.OnboardingViewModel
import org.umbriel.link.pairing.PairingScreen
import org.umbriel.link.pairing.PairingViewModel
import org.umbriel.link.ui.theme.Motion

private enum class Screen { Home, Device, Pairing, Setup, Media, MirrorApps }

/** A handful of screens, so navigation is one piece of state rather than a navigation library. */
@Composable
fun LinkApp(container: AppContainer, pairingLink: StateFlow<String?>, onLinkHandled: () -> Unit) {
    var screen by rememberSaveable { mutableStateOf(Screen.Home) }
    var desktopId by rememberSaveable { mutableStateOf<String?>(null) }
    val link by pairingLink.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val home = viewModel { HomeViewModel(container.repository, container.presence, container.mirror) }
    val pairing = viewModel { PairingViewModel(container.repository) }
    val onboarding = viewModel { OnboardingViewModel(container.application, container.mirror, container.ringer) }
    val setup by onboarding.state.collectAsStateWithLifecycle()

    LaunchedEffect(link) {
        link?.let {
            pairing.offerLink(it)
            onLinkHandled()
            screen = Screen.Pairing
        }
    }
    if (screen != Screen.Home) BackHandler { screen = if (screen == Screen.MirrorApps && desktopId != null) Screen.Device else Screen.Home }
    AnimatedContent(
        targetState = screen,
        transitionSpec = {
            val forward = targetState.ordinal > initialState.ordinal
            val enter = slideInHorizontally(tween(Motion.MORPH, easing = Motion.smoothEnter)) { if (forward) it / 6 else -it / 6 } +
                fadeIn(tween(Motion.FAST))
            enter togetherWith fadeOut(tween(Motion.FADE_OUT)) +
                slideOutHorizontally(tween(Motion.MORPH, easing = Motion.exit)) { if (forward) -it / 8 else it / 8 }
        },
        label = "screen",
    ) { current ->
        when (current) {
            Screen.Home -> HomeScreen(
                viewModel = home,
                setupNeeded = !setup.complete,
                onPair = { screen = Screen.Pairing },
                onDesktop = { desktopId = it.id; screen = Screen.Device },
                onMedia = { screen = Screen.Media },
                onNotifications = {
                    if (Grant.NotificationAccess in setup.granted) {
                        desktopId = null
                        screen = Screen.MirrorApps
                    } else {
                        screen = Screen.Setup
                    }
                },
                onSetup = { screen = Screen.Setup },
            )
            Screen.Device -> desktopId?.let { id ->
                DeviceScreen(
                    viewModel(key = "device-$id") { DeviceViewModel(container.repository, id) },
                    onBack = { screen = Screen.Home },
                    onMirrorApps = { screen = Screen.MirrorApps },
                )
            }
            Screen.Pairing -> PairingScreen(
                viewModel = pairing,
                onBack = { screen = Screen.Home },
                onPaired = { desktop ->
                    container.presence.paired()
                    home.announce(desktop)
                    screen = Screen.Home
                },
            )
            Screen.Setup -> OnboardingScreen(onboarding, container.mirror.accessSettings(), onDone = { screen = Screen.Home })
            Screen.Media -> MediaScreen(viewModel { MediaViewModel(container.repository) }, onBack = { screen = Screen.Home })
            Screen.MirrorApps -> AppFilterScreen(
                viewModel { AppFilterViewModel(container.mirror, context.packageManager) },
                onBack = { screen = if (desktopId != null) Screen.Device else Screen.Home },
            )
        }
    }
}
