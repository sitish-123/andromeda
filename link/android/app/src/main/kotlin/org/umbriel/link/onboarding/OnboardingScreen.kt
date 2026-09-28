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
import org.umbriel.link.ui.components.ConnectionOrb
import org.umbriel.link.ui.components.Glyph
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.PageDots
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
    Screen(title = stringResource(R.string.setup_title), onBack = onDone) {
        HorizontalPager(pager, Modifier.weight(1f).fillMaxWidth()) { index ->
            val page = PAGES[index]
            PageContent(page, granted = page.grant in state.granted, extra = {
                if (page.grant == Grant.AutoClipboard && page.grant !in state.granted) AdbStep(viewModel)
            }) {
                val permissions = viewModel.permissions(page.grant)
                if (permissions.isNotEmpty()) request.launch(permissions) else viewModel.settings(page.grant, mirrorSettings)?.let(context::startActivity)
            }
        }
        Row(
            Modifier.fillMaxWidth().navigationBarsPadding().padding(Space.page),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            PageDots(PAGES.size, pager.currentPage, Modifier.weight(1f))
            PillButton(
                stringResource(if (last) R.string.onboarding_done else R.string.onboarding_next),
                onClick = { if (last) onDone() else scope.launch { pager.animateScrollToPage(pager.currentPage + 1) } },
                kind = PillKind.Tonal,
            )
        }
    }
}

@Composable
private fun PageContent(page: Page, granted: Boolean, extra: @Composable () -> Unit, onAllow: () -> Unit) {
    val colors = LinkTheme.colors
    // Centred when it fits; a page with more to say, like the clipboard one, scrolls instead of clipping its button.
    BoxWithConstraints(Modifier.fillMaxSize()) {
        Column(
            Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).heightIn(min = maxHeight)
                .padding(horizontal = Space.page, vertical = Space.s16),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(Space.s20, Alignment.CenterVertically),
        ) {
            Box(contentAlignment = Alignment.Center) {
                ConnectionOrb(active = granted, size = if (page.grant == Grant.AutoClipboard) 120.dp else 180.dp)
                Box(Modifier.size(56.dp).clip(Radius.pill).background(colors.surfacePrimary.copy(alpha = 0.85f)), contentAlignment = Alignment.Center) {
                    Glyph(if (granted) Icons.Filled.Check else page.icon, colors.accentText, 28.dp)
                }
            }
            Label(stringResource(page.title), LinkTheme.type.displaySmall, textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth())
            Label(
                stringResource(page.body),
                LinkTheme.type.bodyLarge,
                colors.textSecondary,
                Modifier.fillMaxWidth(),
                textAlign = TextAlign.Center,
            )
            if (granted) {
                PillButton(stringResource(R.string.onboarding_granted), onClick = {}, kind = PillKind.Tonal, icon = Icons.Filled.Check, enabled = false)
            } else if (page.grant != Grant.AutoClipboard) {
                PillButton(stringResource(R.string.onboarding_allow), onAllow)
            } else {
                extra()
                PillButton(stringResource(R.string.onboarding_clipboard_overlay), onAllow, kind = PillKind.Tonal)
            }
        }
    }
}

/** Where each half of automatic clipboard stands, and the adb command, copyable, for the half a computer gives. */
@Composable
private fun AdbStep(viewModel: OnboardingViewModel) {
    val colors = LinkTheme.colors
    val context = LocalContext.current
    val command = viewModel.adbCommand()
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(Space.s8)) {
        Step(stringResource(R.string.onboarding_clipboard_step_overlay), viewModel.overlayAllowed())
        Step(stringResource(R.string.onboarding_clipboard_step_logs), viewModel.logsAllowed())
        Label(
            command,
            LinkTheme.type.bodyMedium.copy(fontFamily = FontFamily.Monospace),
            colors.textPrimary,
            Modifier.fillMaxWidth().clip(Radius.list).background(colors.surfaceTertiary).padding(Space.s12),
        )
        PillButton(stringResource(R.string.onboarding_clipboard_copy), onClick = {
            context.getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newPlainText("adb", command))
        }, modifier = Modifier.fillMaxWidth(), kind = PillKind.Quiet)
    }
}

@Composable
private fun Step(label: String, done: Boolean) {
    val colors = LinkTheme.colors
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Space.s8)) {
        Glyph(if (done) Icons.Filled.Check else Icons.Filled.Warning, if (done) colors.success else colors.warning, Size.iconSmall + 2.dp)
        Label(label, LinkTheme.type.titleMedium)
    }
}
