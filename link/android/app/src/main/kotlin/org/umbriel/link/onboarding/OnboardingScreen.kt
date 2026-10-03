package org.umbriel.link.onboarding

import android.content.Intent
import org.umbriel.link.ui.theme.Size
import androidx.compose.material.icons.filled.Edit
import androidx.compose.ui.text.font.FontFamily
import android.content.ClipboardManager
import android.content.ClipData
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import android.net.Uri
import android.provider.Settings
import androidx.lifecycle.compose.LifecycleResumeEffect
import org.umbriel.link.ui.components.DashboardCard
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import kotlinx.coroutines.delay
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.Notifications
import androidx.compose.material.icons.filled.Warning
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch
import org.umbriel.link.R
import org.umbriel.link.ui.components.Glyph
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.Readout
import org.umbriel.link.ui.components.StopTrack
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Space

private data class Page(val grant: Grant, val title: Int, val body: Int, val icon: ImageVector)

private val PAGES = listOf(
    Page(Grant.NotificationAccess, R.string.onboarding_notifications_title, R.string.onboarding_notifications_body, Icons.Filled.Notifications),
    Page(Grant.DoNotDisturb, R.string.onboarding_dnd_title, R.string.onboarding_dnd_body, Icons.Filled.Warning),
    Page(Grant.PostNotifications, R.string.onboarding_post_title, R.string.onboarding_post_body, Icons.Filled.Notifications),
    Page(Grant.Battery, R.string.onboarding_battery_title, R.string.onboarding_battery_body, Icons.Filled.Favorite),
    Page(Grant.Calls, R.string.onboarding_calls_title, R.string.onboarding_calls_body, Icons.Filled.Call),
    Page(Grant.Bluetooth, R.string.onboarding_bluetooth_title, R.string.onboarding_bluetooth_body, Icons.Filled.Share),
    Page(Grant.AutoClipboard, R.string.onboarding_clipboard_title, R.string.onboarding_clipboard_body, Icons.Filled.Edit),
)

/** A short paged flow through the grants, each explained before Android's own screen asks. */
@Composable
fun OnboardingScreen(viewModel: OnboardingViewModel, mirrorSettings: Intent, start: Grant?, onDone: () -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val pager = rememberPagerState(PAGES.indexOfFirst { it.grant == start }.coerceAtLeast(0)) { PAGES.size }
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event -> if (event == Lifecycle.Event.ON_RESUME) viewModel.refresh() }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
    val request = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { viewModel.refresh() }
    val last = pager.currentPage == PAGES.lastIndex
    // A grant given on its page moves on by itself, once the user has seen it turn granted.
    val currentGranted = PAGES[pager.currentPage].grant in state.granted
    var seenUngranted by remember(pager.currentPage) { mutableStateOf(!currentGranted) }
    LaunchedEffect(pager.currentPage, currentGranted) {
        if (!currentGranted) seenUngranted = true
        else if (seenUngranted && !last) {
            delay(ADVANCE_MS)
            pager.animateScrollToPage(pager.currentPage + 1)
        }
    }
    Screen(title = stringResource(R.string.setup_title), onBack = onDone) {
        StopTrack(
            PAGES.size, pager.currentPage, { PAGES[it].grant in state.granted },
            { scope.launch { pager.animateScrollToPage(it) } },
            Modifier.padding(horizontal = Space.s8),
        )
        HorizontalPager(pager, Modifier.weight(1f).fillMaxWidth()) { index ->
            val page = PAGES[index]
            PageContent(page, index, granted = page.grant in state.granted, extra = {
                if (page.grant == Grant.AutoClipboard && page.grant !in state.granted) {
                    ClipboardSteps(viewModel) { viewModel.settings(Grant.AutoClipboard, mirrorSettings)?.let(context::startActivity) }
                }
            }) {
                val permissions = viewModel.permissions(page.grant)
                if (permissions.isNotEmpty()) request.launch(permissions) else viewModel.settings(page.grant, mirrorSettings)?.let(context::startActivity)
            }
        }
        Row(
            Modifier.fillMaxWidth().navigationBarsPadding().padding(Space.page),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Readout(
                stringResource(R.string.onboarding_count, state.granted.count { grant -> PAGES.any { it.grant == grant } }, PAGES.size),
                modifier = Modifier.weight(1f),
            )
            PillButton(
                stringResource(if (last) R.string.onboarding_done else R.string.onboarding_next),
                onClick = { if (last) onDone() else scope.launch { pager.animateScrollToPage(pager.currentPage + 1) } },
                kind = PillKind.Tonal,
            )
        }
    }
}

@Composable
private fun PageContent(page: Page, index: Int, granted: Boolean, extra: @Composable () -> Unit, onAllow: () -> Unit) {
    val colors = LinkTheme.colors
    // Set low on the page when it fits; a page with more to say, like the clipboard one, scrolls instead of clipping.
    BoxWithConstraints(Modifier.fillMaxSize()) {
        Column(
            Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).heightIn(min = maxHeight)
                .padding(horizontal = Space.page, vertical = Space.s16),
            verticalArrangement = Arrangement.spacedBy(Space.s16, Alignment.CenterVertically),
        ) {
            Box(
                Modifier.size(72.dp).clip(Radius.pill)
                    .background(if (granted) colors.accent else colors.surfacePrimary)
                    .border(1.5.dp, if (granted) colors.accent else colors.borderPrimary, Radius.pill),
                contentAlignment = Alignment.Center,
            ) { Glyph(if (granted) Icons.Filled.Check else page.icon, if (granted) colors.onAccent else colors.textPrimary, 30.dp) }
            Readout(
                stringResource(if (granted) R.string.onboarding_step_granted else R.string.onboarding_step, index + 1, PAGES.size),
                if (granted) colors.accentText else colors.textTertiary,
                Modifier.padding(top = Space.s8),
            )
            Label(stringResource(page.title), LinkTheme.type.displayLarge, modifier = Modifier.fillMaxWidth())
            Label(stringResource(page.body), LinkTheme.type.bodyLarge, colors.textSecondary, Modifier.fillMaxWidth())
            if (granted) return@Column
            if (page.grant != Grant.AutoClipboard) {
                PillButton(stringResource(R.string.onboarding_allow), onAllow, Modifier.padding(top = Space.s8).fillMaxWidth())
            } else {
                extra()
            }
        }
    }
}

private const val ADVANCE_MS = 700L

/**
 * Automatic clipboard's two grants as two steps, each with its own way to give it: the overlay in Settings, and log
 * access from a computer. Rechecked on every return to the app, since both are given outside it.
 */
@Composable
private fun ClipboardSteps(viewModel: OnboardingViewModel, onOverlay: () -> Unit) {
    val colors = LinkTheme.colors
    val context = LocalContext.current
    val command = viewModel.adbCommand()
    var overlay by remember { mutableStateOf(viewModel.overlayAllowed()) }
    var logs by remember { mutableStateOf(viewModel.logsAllowed()) }
    LifecycleResumeEffect(Unit) {
        overlay = viewModel.overlayAllowed()
        logs = viewModel.logsAllowed()
        onPauseOrDispose {}
    }
    DashboardCard(Modifier.fillMaxWidth()) {
        Step(stringResource(R.string.onboarding_clipboard_step_overlay), overlay) {
            PillButton(stringResource(R.string.onboarding_clipboard_open), onOverlay)
        }
        if (!overlay) {
            Label(stringResource(R.string.onboarding_clipboard_restricted), LinkTheme.type.bodySmall, colors.textSecondary)
            PillButton(stringResource(R.string.onboarding_clipboard_app_info), {
                context.startActivity(
                    Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:${context.packageName}"))
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                )
            }, kind = PillKind.Quiet)
        }
        Step(stringResource(R.string.onboarding_clipboard_step_logs), logs) {}
        if (!logs) {
            Label(
                command,
                LinkTheme.type.bodyMedium.copy(fontFamily = FontFamily.Monospace),
                colors.textPrimary,
                Modifier.fillMaxWidth().clip(Radius.list).background(colors.surfacePrimary).padding(Space.s12),
            )
            PillButton(stringResource(R.string.onboarding_clipboard_copy), onClick = {
                context.getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newPlainText("adb", command))
            }, kind = PillKind.Quiet)
        }
    }
}

/** One grant: done or not, and while not, the [action] that gives it. */
@Composable
private fun Step(label: String, done: Boolean, action: @Composable () -> Unit) {
    val colors = LinkTheme.colors
    Row(
        Modifier.fillMaxWidth().padding(vertical = Space.s8),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(Space.s12),
    ) {
        Glyph(if (done) Icons.Filled.Check else Icons.Filled.Warning, if (done) colors.success else colors.warning, Size.icon - 4.dp)
        Label(label, LinkTheme.type.titleMedium, modifier = Modifier.weight(1f))
        if (!done) action()
    }
}
