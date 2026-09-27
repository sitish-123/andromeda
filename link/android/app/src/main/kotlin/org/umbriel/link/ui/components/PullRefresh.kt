package org.umbriel.link.ui.components

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.NestedScrollSource
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.Velocity
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import org.umbriel.link.ui.theme.Motion
import org.umbriel.link.ui.theme.Space
import kotlin.math.roundToInt

/**
 * Pull to refresh over scrollable [content]: pulling past the top draws the connection orb down; letting go past the
 * threshold refreshes, and the orb works until [refreshing] is false.
 */
@Composable
fun PullRefresh(refreshing: Boolean, onRefresh: () -> Unit, modifier: Modifier = Modifier, content: @Composable BoxScope.() -> Unit) {
    val threshold = with(LocalDensity.current) { 72.dp.toPx() }
    val pull = remember { Animatable(0f) }
    val scope = rememberCoroutineScope()
    val connection = remember(threshold) {
        object : NestedScrollConnection {
            override fun onPreScroll(available: Offset, source: NestedScrollSource): Offset {
                if (available.y >= 0 || pull.value <= 0f) return Offset.Zero
                val used = maxOf(available.y, -pull.value)
                scope.launch { pull.snapTo(pull.value + used) }
                return Offset(0f, used)
            }

            override fun onPostScroll(consumed: Offset, available: Offset, source: NestedScrollSource): Offset {
                if (available.y <= 0 || source != NestedScrollSource.UserInput) return Offset.Zero
                scope.launch { pull.snapTo((pull.value + available.y * RESISTANCE).coerceAtMost(threshold * 1.5f)) }
                return Offset(0f, available.y)
            }

            override suspend fun onPreFling(available: Velocity): Velocity {
                if (pull.value >= threshold) onRefresh()
                if (pull.value > 0f) pull.animateTo(0f, tween(Motion.NORMAL, easing = Motion.smoothEnter))
                return Velocity.Zero
            }
        }
    }
    LaunchedEffect(refreshing) { if (!refreshing) pull.animateTo(0f, tween(Motion.NORMAL)) }
    Box(modifier.nestedScroll(connection)) {
        content()
        val shown = if (refreshing) threshold else pull.value
        Box(
            Modifier.fillMaxWidth().padding(top = Space.s8)
                .offset { IntOffset(0, (shown - threshold).roundToInt()) }
                .alpha((shown / threshold).coerceIn(0f, 1f)),
            contentAlignment = Alignment.TopCenter,
        ) { ConnectionOrb(active = true, working = refreshing, size = 44.dp) }
    }
}

/** How much of a drag past the top becomes pull, so the pull feels weighted. */
private const val RESISTANCE = 0.5f
