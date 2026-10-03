package org.umbriel.link.ui.components

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.selection.selectable
import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size as Box2
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Fill
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.translate
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.rememberVectorPainter
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.res.stringResource
import org.umbriel.link.R
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Motion
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Space

/** How the line between phone and desktop is drawn: the transport, read at a glance. */
enum class Wire { WiFi, Bluetooth, Searching, Down }

enum class Node { None, Dot, Ring, Desktop, Phone, Alert, Empty }

/** The line's moving parts: dash phase for a link still forming, and a pulse travelling up while files move. */
class WireMotion(val phase: State<Float>, val pulse: State<Float>, val moving: Boolean)

@Composable
fun rememberWireMotion(moving: Boolean): WireMotion {
    val transition = rememberInfiniteTransition(label = "wire")
    val phase = transition.animateFloat(0f, 1f, infiniteRepeatable(tween(Motion.SLOW * 3, easing = LinearEasing)), label = "phase")
    val pulse = transition.animateFloat(1f, 0f, infiniteRepeatable(tween(Motion.SLOW * 4, easing = Motion.standard)), label = "pulse")
    return WireMotion(phase, pulse, moving)
}

/**
 * One stop on the vertical line: a 48 dp gutter carrying the wire through the row's full height, with [node] at the
 * row's first line of text, and the content to its right. [first] and [last] cut the wire at the node.
 */
@Composable
fun TetherRow(
    wire: Wire,
    motion: WireMotion,
    node: Node,
    modifier: Modifier = Modifier,
    first: Boolean = false,
    last: Boolean = false,
    nodeIcon: ImageVector? = null,
    nodeAt: Dp = 22.dp,
    onClick: (() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    val colors = LinkTheme.colors
    val lit = wire == Wire.WiFi || wire == Wire.Bluetooth
    val line = if (lit) colors.accent else colors.textTertiary
    val icon = nodeIcon?.let { rememberVectorPainter(it) }
    val ink = colors.textPrimary
    val page = colors.surfacePrimary
    val alert = colors.warning
    // Drawn behind the row, so the wire spans whatever height the content takes without measuring it twice.
    Row(
        modifier.fillMaxWidth()
            .then(if (onClick != null) Modifier.clickable(role = Role.Button, onClick = onClick) else Modifier)
            .drawBehind {
                val x = (Space.s8 + GUTTER / 2).toPx()
                val y = nodeAt.toPx()
                val top = if (first) y else 0f
                val bottom = if (last) y else size.height
                drawWire(wire, line, x, top, bottom, motion.phase.value)
                if (motion.moving && lit) {
                    val travel = top + (bottom - top) * motion.pulse.value
                    drawCircle(line, 3.dp.toPx(), Offset(x, travel))
                }
                when (node) {
                    Node.None -> Unit
                    Node.Dot -> drawCircle(line, 4.dp.toPx(), Offset(x, y))
                    Node.Ring -> {
                        drawCircle(page, 7.dp.toPx(), Offset(x, y))
                        drawCircle(line, 6.dp.toPx(), Offset(x, y), style = Stroke(1.5.dp.toPx()))
                    }
                    Node.Alert -> {
                        drawCircle(page, 8.dp.toPx(), Offset(x, y))
                        drawCircle(alert, 6.dp.toPx(), Offset(x, y))
                    }
                    Node.Desktop, Node.Phone, Node.Empty -> {
                        val w = (if (node == Node.Phone) 22 else 34).dp.toPx()
                        val h = (if (node == Node.Phone) 34 else 24).dp.toPx()
                        val tl = Offset(x - w / 2, y - h / 2)
                        drawRoundRect(page, tl, Box2(w, h), CornerRadius(6.dp.toPx()))
                        drawRoundRect(
                            if (node == Node.Empty) colors.textTertiary else ink, tl, Box2(w, h), CornerRadius(6.dp.toPx()),
                            style = Stroke(
                                1.5.dp.toPx(),
                                pathEffect = if (node == Node.Empty) PathEffect.dashPathEffect(floatArrayOf(4.dp.toPx(), 4.dp.toPx())) else null,
                            ),
                        )
                        if (node == Node.Desktop) drawCircle(line, 3.dp.toPx(), Offset(x, y))
                    }
                }
                if (icon != null && node == Node.None) {
                    val s = 20.dp.toPx()
                    drawCircle(page, s * 0.75f, Offset(x, y))
                    translate(x - s / 2, y - s / 2) { with(icon) { draw(Box2(s, s), colorFilter = ColorFilter.tint(line)) } }
                }
            },
    ) {
        Spacer(Modifier.width(Space.s8 + GUTTER))
        Column(Modifier.weight(1f).padding(top = Space.s8, bottom = Space.s20, end = Space.page), content = content)
    }
}

private fun DrawScope.drawWire(wire: Wire, color: Color, x: Float, top: Float, bottom: Float, phase: Float) {
    if (bottom <= top) return
    val width = (if (wire == Wire.WiFi) 2.5f else 2f).dp.toPx()
    val dash = when (wire) {
        Wire.WiFi -> null
        Wire.Bluetooth -> floatArrayOf(10.dp.toPx(), 6.dp.toPx())
        Wire.Searching, Wire.Down -> floatArrayOf(2.dp.toPx(), 6.dp.toPx())
    }
    // A forming link crawls upward; an idle one stays still.
    val offset = if (wire == Wire.Searching || wire == Wire.Bluetooth) phase * (dash?.sum() ?: 0f) else 0f
    drawLine(
        color.copy(alpha = if (wire == Wire.Down) 0.5f else 1f), Offset(x, top), Offset(x, bottom), width,
        StrokeCap.Round, dash?.let { PathEffect.dashPathEffect(it, offset) },
    )
}

/** The bottom bar: icon over label per tab, and one soft pill behind the selected tab that slides to the next. */
@Composable
fun PillNav(selected: Int, labels: List<String>, icons: List<ImageVector>, onSelect: (Int) -> Unit) {
    val colors = LinkTheme.colors
    val position by animateFloatAsState(
        selected.toFloat(), spring(dampingRatio = 0.72f, stiffness = Spring.StiffnessMediumLow), label = "pill",
    )
    val bubble = colors.accent.copy(alpha = if (colors.dark) 0.18f else 0.12f)
    val rule = colors.borderPrimary
    Box(
        Modifier.fillMaxWidth()
            .drawBehind { drawLine(rule, Offset.Zero, Offset(size.width, 0f), 1.dp.toPx()) }
            .navigationBarsPadding()
            .padding(horizontal = Space.s12, vertical = Space.s6),
    ) {
        Canvas(Modifier.matchParentSize()) {
            val step = size.width / labels.size
            val inset = 6.dp.toPx()
            drawRoundRect(
                bubble, Offset(position * step + inset, 0f), Box2(step - inset * 2, size.height),
                CornerRadius(size.height / 2),
            )
        }
        Row(Modifier.fillMaxWidth()) {
            labels.forEachIndexed { index, label ->
                val active = index == selected
                val tint by animateColorAsState(if (active) colors.accentText else colors.textTertiary, tween(Motion.FAST), label = "tab")
                val description = stringResource(R.string.navigation_tab, label)
                Column(
                    Modifier.weight(1f).height(56.dp).clip(Radius.pill)
                        .selectable(active, role = Role.Tab) { onSelect(index) }
                        .semantics { contentDescription = description },
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center,
                ) {
                    Glyph(icons[index], tint, 22.dp)
                    Label(label, LinkTheme.type.titleSmall, tint, Modifier.padding(top = Space.s2), maxLines = 1)
                }
            }
        }
    }
}

/**
 * A run of stops on a short wire, for a paged flow: stops already [done] are filled, the [current] one carries the
 * knob, and the wire is lit up to it. A stop is tappable.
 */
@Composable
fun StopTrack(count: Int, current: Int, done: (Int) -> Boolean, onSelect: (Int) -> Unit, modifier: Modifier = Modifier) {
    val colors = LinkTheme.colors
    val position by animateFloatAsState(
        current.toFloat(), spring(dampingRatio = 0.62f, stiffness = Spring.StiffnessMediumLow), label = "stop",
    )
    val track = colors.borderPrimary
    val lit = colors.accent
    val page = colors.surfacePrimary
    Box(modifier.fillMaxWidth().height(32.dp)) {
        Canvas(Modifier.matchParentSize()) {
            val step = size.width / count
            val y = size.height / 2
            val first = step / 2
            val knob = first + position * step
            drawLine(track, Offset(first, y), Offset(first + (count - 1) * step, y), 2.dp.toPx(), StrokeCap.Round)
            drawLine(lit, Offset(first, y), Offset(knob, y), 2.5.dp.toPx(), StrokeCap.Round)
            repeat(count) { index ->
                val x = first + index * step
                drawCircle(page, 7.dp.toPx(), Offset(x, y))
                if (done(index)) drawCircle(lit, 4.5.dp.toPx(), Offset(x, y))
                else drawCircle(track, 4.dp.toPx(), Offset(x, y), style = Stroke(1.5.dp.toPx()))
            }
            drawCircle(lit, 9.dp.toPx(), Offset(knob, y), style = Stroke(2.dp.toPx()))
        }
        Row(Modifier.matchParentSize()) {
            repeat(count) { index ->
                Box(Modifier.weight(1f).fillMaxSize().selectable(index == current, role = Role.Tab) { onSelect(index) })
            }
        }
    }
}

/** A hairline rule, the only divider. */
@Composable
fun Hairline(modifier: Modifier = Modifier) {
    val color = LinkTheme.colors.borderPrimary
    Canvas(modifier.fillMaxWidth().height(1.dp)) { drawLine(color, Offset.Zero, Offset(size.width, 0f), 1.dp.toPx()) }
}

/** A short uppercase mono readout: transport, counts, states. */
@Composable
fun Readout(text: String, color: Color = LinkTheme.colors.textSecondary, modifier: Modifier = Modifier) {
    Label(text.uppercase(), LinkTheme.type.mono, color, modifier, maxLines = 1)
}

private val GUTTER = 64.dp
