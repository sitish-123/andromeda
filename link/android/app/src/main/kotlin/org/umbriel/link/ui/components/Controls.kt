package org.umbriel.link.ui.components

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.drawscope.rotate as turned
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Motion
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Size
import org.umbriel.link.ui.theme.Space

enum class PillKind { Filled, Tonal, Quiet, Destructive }

/** The 100 dp pill button, 48 dp tall: accent-filled for the screen's one call to action, tonal otherwise. */
@Composable
fun PillButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    kind: PillKind = PillKind.Filled,
    icon: ImageVector? = null,
    enabled: Boolean = true,
) {
    val colors = LinkTheme.colors
    val source = remember { MutableInteractionSource() }
    val pressed by source.collectIsPressedAsState()
    val scale by animateFloatAsState(if (pressed) 0.97f else 1f, tween(Motion.BUTTON_PRESS), label = "pill")
    val (fill, content) = when (kind) {
        PillKind.Filled -> colors.accent to colors.onAccent
        PillKind.Tonal -> colors.surfaceTertiary to colors.textPrimary
        PillKind.Quiet -> Color.Transparent to colors.accentText
        PillKind.Destructive -> colors.error.copy(alpha = 0.12f) to colors.error
    }
    Row(
        modifier = modifier
            .graphicsLayer { scaleX = scale; scaleY = scale }
            .heightIn(min = Size.pill)
            .alpha(if (enabled) 1f else 0.4f)
            .clip(Radius.pill)
            .background(fill)
            .clickable(source, LocalIndication.current, enabled = enabled, role = Role.Button, onClick = onClick)
            .padding(horizontal = Space.s20, vertical = Space.s12),
        horizontalArrangement = Arrangement.spacedBy(Space.s8, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) Glyph(icon, content, Size.iconSmall + 2.dp)
        Label(text, LinkTheme.type.titleMedium, content, maxLines = 1)
    }
}

/**
 * The sliding-pill segmented control: a 48 dp track with an inner pill, inset 4 dp, that slides to the selection.
 */
@Composable
fun SlidingPillControl(options: List<String>, selected: Int, onSelect: (Int) -> Unit, modifier: Modifier = Modifier) {
    val colors = LinkTheme.colors
    BoxWithConstraints(
        modifier.fillMaxWidth().height(Size.pill).clip(Radius.pill).border(1.dp, colors.borderPrimary, Radius.pill).padding(Space.s4),
    ) {
        val segment = maxWidth / options.size
        val x by animateDpAsState(segment * selected, tween(Motion.MORPH, easing = Motion.smoothEnter), label = "segment")
        Box(
            Modifier.offset(x = x).width(segment).fillMaxHeight()
                .clip(Radius.pill).background(colors.surfacePrimary).border(1.dp, colors.borderPrimary, Radius.pill),
        )
        Row(Modifier.fillMaxWidth().fillMaxHeight()) {
            options.forEachIndexed { index, option ->
                Box(
                    Modifier.weight(1f).fillMaxHeight().clip(Radius.pill)
                        .selectable(index == selected, role = Role.Tab) { onSelect(index) },
                    contentAlignment = Alignment.Center,
                ) {
                    val color = if (index == selected) colors.textPrimary else colors.textSecondary
                    Label(option, LinkTheme.type.titleMedium, color, maxLines = 1)
                }
            }
        }
    }
}

/** A pill-shaped switch drawn from primitives; the row that holds it is the toggle. */
@Composable
fun PillSwitch(checked: Boolean, modifier: Modifier = Modifier) {
    val colors = LinkTheme.colors
    val track by animateColorAsState(if (checked) colors.accent else colors.surfacePrimary, tween(Motion.FAST), label = "track")
    val knob by animateDpAsState(if (checked) 20.dp else 0.dp, tween(Motion.MEDIUM_FAST, easing = Motion.smoothEnter), label = "knob")
    Box(
        modifier.width(48.dp).height(28.dp).clip(Radius.pill).background(track)
            .border(1.dp, if (checked) colors.accent else colors.borderPrimary, Radius.pill).padding(Space.s4),
    ) {
        Box(
            Modifier.offset(x = knob).size(20.dp)
                .clip(Radius.pill).background(if (checked) colors.onAccent else colors.textTertiary),
        )
    }
}

/** A title, an optional line under it, and a switch; the whole row toggles. */
@Composable
fun SwitchRow(title: String, checked: Boolean, onChange: (Boolean) -> Unit, modifier: Modifier = Modifier, subtitle: String? = null, icon: ImageVector? = null, enabled: Boolean = true) {
    Row(
        modifier = modifier.fillMaxWidth().heightIn(min = Size.touchLarge).clip(Radius.list)
            .toggleable(checked, enabled = enabled, role = Role.Switch, onValueChange = onChange)
            .padding(vertical = Space.s8),
        horizontalArrangement = Arrangement.spacedBy(Space.s12),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) IconChip(icon)
        Column(Modifier.weight(1f)) {
            Label(title, LinkTheme.type.titleMedium)
            if (subtitle != null) Label(subtitle, LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
        }
        PillSwitch(checked)
    }
}

/** A title, an optional line under it, and a chevron; the whole row opens something. */
@Composable
fun NavRow(title: String, subtitle: String?, icon: ImageVector, onClick: () -> Unit, modifier: Modifier = Modifier) {
    Row(
        modifier = modifier.fillMaxWidth().heightIn(min = Size.touchLarge).clip(Radius.list)
            .clickable(role = Role.Button, onClick = onClick)
            .padding(vertical = Space.s8),
        horizontalArrangement = Arrangement.spacedBy(Space.s12),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        IconChip(icon)
        Column(Modifier.weight(1f)) {
            Label(title, LinkTheme.type.titleMedium)
            if (subtitle != null) Label(subtitle, LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
        }
        Glyph(Icons.AutoMirrored.Filled.KeyboardArrowRight, LinkTheme.colors.textTertiary)
    }
}

/** A 36 dp disc holding a glyph, lifted off its group by the page color. */
@Composable
fun IconChip(icon: ImageVector, tint: Color = LinkTheme.colors.textPrimary, fill: Color = LinkTheme.colors.surfacePrimary) {
    Box(Modifier.size(36.dp).clip(Radius.pill).background(fill), contentAlignment = Alignment.Center) {
        Glyph(icon, tint, Size.icon - 4.dp)
    }
}

/** The connection node: a dot in a ring that sends out a ripple while connected or working, still and grey otherwise. */
@Composable
fun ConnectionOrb(active: Boolean, modifier: Modifier = Modifier, size: Dp = 64.dp, working: Boolean = false) {
    val colors = LinkTheme.colors
    val pulse = rememberInfiniteTransition(label = "orb")
    val period = if (working) Motion.SLOW * 2 else Motion.SLOW * 4
    val phase by pulse.animateFloat(0f, 1f, infiniteRepeatable(tween(period, easing = Motion.standard), RepeatMode.Restart), label = "phase")
    val spin by pulse.animateFloat(0f, 360f, infiniteRepeatable(tween(period * 2, easing = androidx.compose.animation.core.LinearEasing)), label = "spin")
    val lit = active || working
    val core = if (lit) colors.accent else colors.textDisabled
    Canvas(modifier.size(size)) {
        val radius = this.size.minDimension / 2f
        if (lit) {
            drawCircle(core.copy(alpha = 0.35f * (1f - phase)), radius * (0.62f + 0.38f * phase))
        }
        drawCircle(core, radius * 0.22f)
        drawCircle(core, radius * 0.5f, style = androidx.compose.ui.graphics.drawscope.Stroke(radius * 0.04f))
        if (working) {
            turned(spin) {
                drawArc(
                    colors.accent,
                    startAngle = 0f,
                    sweepAngle = 90f,
                    useCenter = false,
                    topLeft = Offset(center.x - radius * 0.8f, center.y - radius * 0.8f),
                    size = androidx.compose.ui.geometry.Size(radius * 1.6f, radius * 1.6f),
                    style = androidx.compose.ui.graphics.drawscope.Stroke(width = radius * 0.08f, cap = androidx.compose.ui.graphics.StrokeCap.Round),
                )
            }
        }
    }
}

/** A small uppercase eyebrow over a section. */
@Composable
fun Eyebrow(text: String, modifier: Modifier = Modifier) {
    Label(text.uppercase(), LinkTheme.type.mono, LinkTheme.colors.textTertiary, modifier.padding(bottom = Space.s8), maxLines = 1)
}

/** Centered text blocks for empty states and explanations. */
@Composable
fun CenteredText(title: String, body: String, modifier: Modifier = Modifier) {
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(Space.s8)) {
        Label(title, LinkTheme.type.headlineLarge, modifier = Modifier.fillMaxWidth(), textAlign = TextAlign.Center)
        Label(body, LinkTheme.type.bodyLarge, LinkTheme.colors.textSecondary, Modifier.fillMaxWidth(), textAlign = TextAlign.Center)
    }
}
