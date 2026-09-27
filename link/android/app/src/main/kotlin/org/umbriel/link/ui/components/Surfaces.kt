package org.umbriel.link.ui.components

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.Image
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.text.BasicText
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Paint
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.addOutline
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.rememberVectorPainter
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import org.umbriel.link.ui.theme.Elevation
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Motion
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Size
import org.umbriel.link.ui.theme.Space

/** Text in a type role; the design system's only text primitive. */
@Composable
fun Label(
    text: String,
    style: TextStyle = LinkTheme.type.bodyLarge,
    color: Color = LinkTheme.colors.textPrimary,
    modifier: Modifier = Modifier,
    maxLines: Int = Int.MAX_VALUE,
    textAlign: TextAlign = TextAlign.Start,
) {
    BasicText(text, modifier, style.copy(color = color, textAlign = textAlign), maxLines = maxLines, overflow = TextOverflow.Ellipsis)
}

@Composable
fun Glyph(icon: ImageVector, tint: Color = LinkTheme.colors.textPrimary, size: Dp = Size.icon, modifier: Modifier = Modifier) {
    Image(rememberVectorPainter(icon), contentDescription = null, modifier = modifier.size(size), colorFilter = ColorFilter.tint(tint))
}

/**
 * A soft shadow drawn as the design system specifies it (blur and offset, one color), which elevation shadows cannot
 * match. Dark mode uses the dark offsets; many dark surfaces carry none and separate by color instead.
 */
fun Modifier.softShadow(shape: Shape, elevation: Elevation, color: Color, dark: Boolean): Modifier = drawBehind {
    val blur = (if (dark) elevation.darkBlur else elevation.blur).dp.toPx()
    val offset = (if (dark) elevation.darkOffset else elevation.lightOffset).dp.toPx()
    val path = Path().apply { addOutline(shape.createOutline(size, layoutDirection, this@drawBehind)) }
    drawIntoCanvas { canvas ->
        val paint = Paint()
        // A translucent shadow color keeps its own alpha on a transparent paint, so only the shadow shows.
        paint.asFrameworkPaint().apply {
            this.color = android.graphics.Color.TRANSPARENT
            setShadowLayer(blur, 0f, offset, color.toArgb())
        }
        canvas.drawPath(path, paint)
    }
}

/** A press shrinks the surface slightly, as the soft-tech card does. */
@Composable
private fun Modifier.pressScale(source: MutableInteractionSource, scale: Float = 0.99f): Modifier {
    val pressed by source.collectIsPressedAsState()
    val current by animateFloatAsState(if (pressed) scale else 1f, tween(Motion.BUTTON_PRESS, easing = Motion.enter), label = "press")
    return graphicsLayer { scaleX = current; scaleY = current }
}

/**
 * The canonical container: 28 dp corners, 20 dp padding; in light a 2 dp white rim over the page and a soft lift,
 * in dark one surface step up and no shadow.
 */
@Composable
fun SoftCard(
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null,
    padding: Dp = Space.s20,
    content: @Composable ColumnScope.() -> Unit,
) {
    val colors = LinkTheme.colors
    val source = remember { MutableInteractionSource() }
    val shape = Radius.softTech
    val surface = if (colors.dark) colors.surfaceSecondary else colors.surfacePrimary
    Column(
        modifier = modifier
            .then(if (onClick != null) Modifier.pressScale(source) else Modifier)
            .then(if (colors.dark) Modifier else Modifier.softShadow(shape, Elevation.SoftLift, colors.shadow, dark = false))
            .clip(shape)
            .background(surface)
            .border(Size.cardRim, colors.cardRim, shape)
            .then(if (onClick != null) Modifier.clickable(source, indication = LocalIndication.current, onClick = onClick) else Modifier)
            .padding(padding),
        content = content,
    )
}

/** The 20 dp hero surface: the screen's one lead element, washed with the accent. */
@Composable
fun HeroSurface(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    val colors = LinkTheme.colors
    val shape = Radius.hero
    val wash = Brush.linearGradient(
        listOf(colors.accent.copy(alpha = if (colors.dark) 0.28f else 0.22f), colors.surfaceTertiary.copy(alpha = 0.4f)),
    )
    Column(
        modifier = modifier
            .softShadow(shape, Elevation.SoftLift, colors.shadow, colors.dark)
            .clip(shape)
            .background(if (colors.dark) colors.surfaceSecondary else colors.surfacePrimary)
            .background(wash)
            .padding(Space.s24),
        content = content,
    )
}

/**
 * A screen: an optional back glyph and a title over content with the 20 dp page gutter, and a toast host for the
 * screen's one-line messages.
 */
@Composable
fun Screen(
    title: String,
    onBack: (() -> Unit)? = null,
    toast: String? = null,
    onToastShown: () -> Unit = {},
    actions: @Composable () -> Unit = {},
    overlay: @Composable BoxScope.() -> Unit = {},
    content: @Composable ColumnScope.() -> Unit,
) {
    val colors = LinkTheme.colors
    Box(Modifier.fillMaxSize().background(colors.surfacePrimary)) {
        Column(Modifier.fillMaxSize().statusBarsPadding()) {
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = Space.s8, vertical = Space.s8),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (onBack != null) {
                    Box(
                        modifier = Modifier.size(Size.touch).clip(Radius.pill).clickable(role = Role.Button, onClick = onBack)
                            .semantics { contentDescription = "Back" },
                        contentAlignment = Alignment.Center,
                    ) { Glyph(Icons.AutoMirrored.Filled.ArrowBack) }
                } else {
                    Box(Modifier.size(Space.s12))
                }
                Label(title, LinkTheme.type.headlineLarge, modifier = Modifier.weight(1f).padding(horizontal = Space.s4))
                actions()
            }
            Column(Modifier.weight(1f).fillMaxWidth(), content = content)
        }
        overlay()
        ToastHost(toast, onToastShown, Modifier.align(Alignment.BottomCenter))
    }
}

/** One line at the bottom in a pill, for a few seconds. */
@Composable
private fun ToastHost(message: String?, onShown: () -> Unit, modifier: Modifier) {
    LaunchedEffect(message) {
        if (message != null) {
            delay(TOAST_MS)
            onShown()
        }
    }
    AnimatedVisibility(
        visible = message != null,
        modifier = modifier.navigationBarsPadding().padding(Space.s20),
        enter = slideInVertically(tween(Motion.NORMAL, easing = Motion.smoothEnter)) { it } + fadeIn(tween(Motion.FAST)),
        exit = slideOutVertically(tween(Motion.FAST, easing = Motion.exit)) { it } + fadeOut(tween(Motion.FADE_OUT)),
    ) {
        val colors = LinkTheme.colors
        Box(
            Modifier.softShadow(Radius.pill, Elevation.Medium, colors.shadow, colors.dark)
                .clip(Radius.pill)
                .background(if (colors.dark) colors.surfaceElevated else colors.textPrimary)
                .padding(horizontal = Space.s20, vertical = Space.s14),
        ) {
            Label(message.orEmpty(), LinkTheme.type.titleMedium, if (colors.dark) colors.textPrimary else colors.textInverse)
        }
    }
}

private const val TOAST_MS = 4_000L

/** A section header: title, and a quieter note after it. */
@Composable
fun SectionHeader(title: String, note: String? = null, modifier: Modifier = Modifier) {
    Row(modifier.padding(bottom = Space.block), horizontalArrangement = Arrangement.spacedBy(Space.s8), verticalAlignment = Alignment.Bottom) {
        Label(title, LinkTheme.type.titleLarge)
        if (note != null) Label(note, LinkTheme.type.bodySmall, LinkTheme.colors.textSecondary)
    }
}
