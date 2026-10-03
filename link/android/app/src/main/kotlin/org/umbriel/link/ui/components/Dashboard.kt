package org.umbriel.link.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Space

@Composable
fun DashboardCard(
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(
        modifier.clip(Radius.softTech).background(LinkTheme.colors.surfaceTertiary)
            .then(if (onClick != null) Modifier.clickable(role = Role.Button, onClick = onClick) else Modifier)
            .padding(horizontal = Space.s16, vertical = Space.s8),
        verticalArrangement = Arrangement.spacedBy(Space.s8),
        content = content,
    )
}

@Composable
fun DashboardSection(title: String, subtitle: String? = null, action: (@Composable () -> Unit)? = null) {
    Row(Modifier.fillMaxWidth().padding(start = Space.s4, top = Space.s8), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(Space.s4)) {
            Readout(title, LinkTheme.colors.textTertiary)
            if (subtitle != null) Label(subtitle, LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
        }
        action?.invoke()
    }
}

/** Small print under a group, the way a setting explains itself without adding a row. */
@Composable
fun Footnote(text: String) {
    Label(text, LinkTheme.type.bodySmall, LinkTheme.colors.textTertiary, Modifier.padding(horizontal = Space.s4))
}

@Composable
fun StatusBadge(text: String, active: Boolean, modifier: Modifier = Modifier) {
    val colors = LinkTheme.colors
    Row(
        modifier.clip(Radius.pill)
            .border(1.dp, colors.borderPrimary, Radius.pill)
            .padding(horizontal = Space.s12, vertical = Space.s8),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(Space.s8),
    ) {
        Box(Modifier.size(6.dp).clip(Radius.pill).background(if (active) colors.accent else colors.textTertiary))
        Label(text.uppercase(), LinkTheme.type.mono, colors.textSecondary)
    }
}
