package org.umbriel.link.clipboard

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.umbriel.link.R

/** One line with the adb command that turns on automatic phone-to-desktop clipboard, shown until it is granted. */
@Composable
fun ClipboardHint() {
    val context = LocalContext.current
    val granted = remember { ClipboardWatcher.granted(context) }
    if (granted) return
    Text(
        stringResource(R.string.clipboard_hint, context.packageName),
        style = MaterialTheme.typography.bodySmall,
        modifier = Modifier.fillMaxWidth().padding(horizontal = 4.dp),
    )
}
