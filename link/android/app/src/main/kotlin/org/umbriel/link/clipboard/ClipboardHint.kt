package org.umbriel.link.clipboard

import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import org.umbriel.link.R
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Space

/** One line with the grants that turn on automatic phone-to-desktop clipboard, shown until they are given. */
@Composable
fun ClipboardHint() {
    val context = LocalContext.current
    val granted = remember { ClipboardWatcher.granted(context) }
    if (granted) return
    Label(
        stringResource(R.string.clipboard_hint, context.packageName),
        LinkTheme.type.bodyMedium,
        LinkTheme.colors.textSecondary,
        modifier = Modifier.padding(top = Space.s12),
    )
}
