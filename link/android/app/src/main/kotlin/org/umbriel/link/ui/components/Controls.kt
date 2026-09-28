package org.umbriel.link.ui.components

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
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
import androidx.compose.material.icons.filled.Add
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.drawscope.rotate as turned
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.umbriel.link.ui.theme.Elevation
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

/** A 32 dp pill that is on or off: tertiary at rest, accent when selected. */
@Composable
fun SelectablePill(label: String, selected: Boolean, onClick: () -> Unit, modifier: Modifier = Modifier, icon: ImageVector? = null) {
    val colors = LinkTheme.colors
    val fill by animateColorAsState(if (selected) colors.accent else colors.surfaceTertiary, tween(Motion.FAST), label = "chip")
    val content = if (selected) colors.onAccent else colors.textPrimary
    Row(
        modifier = modifier
            .heightIn(min = Size.touch)
            .selectable(selected, role = Role.Checkbox, onClick = onClick)
            .padding(vertical = Space.s8),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(
            Modifier.height(Size.chip).clip(Radius.pill).background(fill).padding(horizontal = Space.s12),
            horizontalArrangement = Arrangement.spacedBy(Space.s6),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (icon != null) Glyph(icon, content, Size.iconSmall)
            Label(label, LinkTheme.type.labelLarge, content, maxLines = 1)
        }
    }
}

/**
 * The sliding-pill segmented control: a 48 dp track with an inner pill, inset 4 dp, that slides to the selection.
 */
@Composable
fun SlidingPillControl(options: List<String>, selected: Int, onSelect: (Int) -> Unit, modifier: Modifier = Modifier) {
    val colors = LinkTheme.colors
    BoxWithConstraints(
        modifier.fillMaxWidth().height(Size.pill).clip(Radius.pill).background(colors.surfaceTertiary).padding(Space.s4),
    ) {
        val segment = maxWidth / options.size
        val x by animateDpAsState(segment * selected, tween(Motion.MORPH, easing = Motion.smoothEnter), label = "segment")
        Box(
            Modifier.offset(x = x).width(segment).fillMaxHeight()
                .softShadow(Radius.pill, Elevation.Small, colors.shadow, colors.dark)
                .clip(Radius.pill).background(if (colors.dark) colors.surfaceElevated else colors.surfacePrimary),
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
    val track by animateColorAsState(if (checked) colors.accent else colors.surfaceTertiary, tween(Motion.FAST), label = "track")
    val knob by animateDpAsState(if (checked) 20.dp else 0.dp, tween(Motion.MEDIUM_FAST, easing = Motion.smoothEnter), label = "knob")
    Box(modifier.width(48.dp).height(28.dp).clip(Radius.pill).background(track).padding(Space.s4)) {
        Box(
            Modifier.offset(x = knob).size(20.dp)
                .softShadow(Radius.pill, Elevation.Small, colors.shadow, colors.dark)
                .clip(Radius.pill).background(if (checked) colors.onAccent else colors.surfacePrimary),
        )
    }
}

/** A title, an optional line under it, and a switch; the whole row toggles. */
@Composable
fun SwitchRow(title: String, checked: Boolean, onChange: (Boolean) -> Unit, modifier: Modifier = Modifier, subtitle: String? = null, icon: ImageVector? = null) {
    Row(
        modifier = modifier.fillMaxWidth().heightIn(min = Size.touchLarge).clip(Radius.list)
            .toggleable(checked, role = Role.Switch, onValueChange = onChange)
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

/** A 40 dp rounded square holding a glyph, the design system's icon container. */
@Composable
fun IconChip(icon: ImageVector, tint: Color = LinkTheme.colors.accentText, fill: Color = LinkTheme.colors.accent.copy(alpha = 0.14f)) {
    Box(Modifier.size(Size.iconContainer).clip(RoundedSquare).background(fill), contentAlignment = Alignment.Center) {
        Glyph(icon, tint, Size.icon - 2.dp)
    }
}

private val RoundedSquare = androidx.compose.foundation.shape.RoundedCornerShape(10.dp)

/**
 * The connection orb: an accent sphere that breathes while connected or working, a still grey one otherwise.
 */
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
        drawCircle(
            Brush.radialGradient(
                listOf(Color.White.copy(alpha = if (lit) 0.85f else 0.4f), core, core.copy(alpha = 0.85f)),
                center = Offset(center.x - radius * 0.18f, center.y - radius * 0.22f),
                radius = radius * 0.62f,
            ),
            radius * 0.5f,
        )
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

data class OrbAction(val label: String, val icon: ImageVector, val onClick: () -> Unit)

/** The expanding action orb: a 56 dp accent orb that opens a column of quick actions above it. */
@Composable
fun ActionOrb(expanded: Boolean, onToggle: () -> Unit, actions: List<OrbAction>, modifier: Modifier = Modifier) {
    val colors = LinkTheme.colors
    val turn by animateFloatAsState(if (expanded) 45f else 0f, tween(Motion.MORPH, easing = Motion.smoothEnter), label = "turn")
    Column(modifier, horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(Space.s12)) {
        AnimatedVisibility(
            expanded,
            enter = fadeIn(tween(Motion.FAST)) + expandVertically(tween(Motion.MORPH, easing = Motion.smoothEnter), Alignment.Bottom),
            exit = fadeOut(tween(Motion.FADE_OUT)) + shrinkVertically(tween(Motion.MEDIUM_FAST, easing = Motion.exit), Alignment.Bottom),
        ) {
            Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(Space.s10)) {
                actions.forEach { action ->
                    Row(
                        Modifier.softShadow(Radius.pill, Elevation.SoftLift, colors.shadow, colors.dark)
                            .clip(Radius.pill)
                            .background(if (colors.dark) colors.surfaceElevated else colors.surfacePrimary)
                            .clickable(role = Role.Button, onClick = action.onClick)
                            .padding(horizontal = Space.s16, vertical = Space.s12),
                        horizontalArrangement = Arrangement.spacedBy(Space.s8),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Glyph(action.icon, colors.accentText, Size.iconSmall + 2.dp)
                        Label(action.label, LinkTheme.type.titleMedium, maxLines = 1)
                    }
                }
            }
        }
        Box(
            Modifier.size(Size.orb)
                .softShadow(Radius.pill, Elevation.SoftLiftElevated, colors.shadow, colors.dark)
                .clip(Radius.pill)
                .background(colors.accent)
                .clickable(role = Role.Button, onClick = onToggle),
            contentAlignment = Alignment.Center,
        ) {
            Glyph(Icons.Filled.Add, colors.onAccent, Size.icon, Modifier.rotate(turn))
        }
    }
}

/** A bento tile: an icon chip, a title, and a hint, in a soft card. */
@Composable
fun BentoTile(title: String, hint: String, icon: ImageVector, onClick: () -> Unit, modifier: Modifier = Modifier, highlighted: Boolean = false) {
    val colors = LinkTheme.colors
    SoftCard(modifier, onClick = onClick, padding = Space.s16) {
        IconChip(
            icon,
            tint = if (highlighted) colors.onAccent else colors.accentText,
            fill = if (highlighted) colors.accent else colors.accent.copy(alpha = 0.14f),
        )
        Label(title, LinkTheme.type.titleLarge, modifier = Modifier.padding(top = Space.s12), maxLines = 2)
        Label(hint, LinkTheme.type.bodySmall, colors.textSecondary, Modifier.padding(top = Space.s2), maxLines = 2)
    }
}

/** A two-column bento grid: [tiles] fill rows left to right; a tile marked wide takes a whole row. */
@Composable
fun BentoGrid(tiles: List<Pair<Boolean, @Composable (Modifier) -> Unit>>, modifier: Modifier = Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(Space.block)) {
        var row = mutableListOf<@Composable (Modifier) -> Unit>()
        val rows = mutableListOf<List<@Composable (Modifier) -> Unit>>()
        tiles.forEach { (wide, tile) ->
            if (wide) {
                if (row.isNotEmpty()) rows += row
                rows += listOf(tile)
                row = mutableListOf()
            } else {
                row += tile
                if (row.size == 2) {
                    rows += row
                    row = mutableListOf()
                }
            }
        }
        if (row.isNotEmpty()) rows += row
        rows.forEach { cells ->
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(Space.block)) {
                cells.forEach { cell -> cell(Modifier.weight(1f)) }
            }
        }
    }
}

/** A small uppercase eyebrow over a section. */
@Composable
fun Eyebrow(text: String, modifier: Modifier = Modifier) {
    Label(text.uppercase(), LinkTheme.type.labelMedium, LinkTheme.colors.textTertiary, modifier.padding(bottom = Space.s8), maxLines = 1)
}

/** Dots for a paged flow: the current page is a wider accent pill. */
@Composable
fun PageDots(count: Int, current: Int, modifier: Modifier = Modifier) {
    val colors = LinkTheme.colors
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(Space.s6), verticalAlignment = Alignment.CenterVertically) {
        repeat(count) { index ->
            val width by animateDpAsState(if (index == current) 24.dp else 8.dp, tween(Motion.NORMAL, easing = Motion.smoothEnter), label = "dot")
            Box(Modifier.height(8.dp).width(width).clip(Radius.pill).background(if (index == current) colors.accent else colors.surfaceTertiary))
        }
    }
}

/** Centered text blocks for empty states and explanations. */
@Composable
fun CenteredText(title: String, body: String, modifier: Modifier = Modifier) {
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(Space.s8)) {
        Label(title, LinkTheme.type.headlineLarge, modifier = Modifier.fillMaxWidth(), textAlign = TextAlign.Center)
        Label(body, LinkTheme.type.bodyLarge, LinkTheme.colors.textSecondary, Modifier.fillMaxWidth(), textAlign = TextAlign.Center)
    }
}
