package org.umbriel.link.onboarding

import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
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
import androidx.compose.ui.platform.LocalLifecycleOwner
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
)

/** A short paged flow through the grants, each explained before Android's own screen asks. */
@Composable
fun OnboardingScreen(viewModel: OnboardingViewModel, mirrorSettings: Intent, onDone: () -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val pager = rememberPagerState { PAGES.size }
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
            PageContent(page, granted = page.grant in state.granted) {
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
private fun PageContent(page: Page, granted: Boolean, onAllow: () -> Unit) {
    val colors = LinkTheme.colors
    Column(
        Modifier.fillMaxSize().padding(horizontal = Space.page),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(Space.s20, Alignment.CenterVertically),
    ) {
        Box(contentAlignment = Alignment.Center) {
            ConnectionOrb(active = granted, size = 180.dp)
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
        } else {
            PillButton(stringResource(R.string.onboarding_allow), onAllow)
        }
    }
}
