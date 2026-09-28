package org.umbriel.link.files

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.text.format.Formatter
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.umbriel.link.R
import org.umbriel.link.ui.components.CenteredText
import org.umbriel.link.ui.components.Glyph
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.SlidingPillControl
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Motion
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Size
import org.umbriel.link.ui.theme.Space

/** The in-app picker: Photos, Videos, and Files over one selection, and a send bar once anything is picked. */
@Composable
fun BoxScope.PickerSheet(viewModel: PickerViewModel, state: PickerState, desktopName: String, onSend: () -> Unit) {
    BackHandler(state.open) { if (state.tab != PickerTab.Files || !viewModel.up()) viewModel.close() }
    val colors = LinkTheme.colors
    AnimatedVisibility(state.open, enter = fadeIn(tween(Motion.FAST)), exit = fadeOut(tween(Motion.FADE_OUT))) {
        Box(
            Modifier.fillMaxSize().background(colors.surfaceOverlay)
                .clickable(remember { MutableInteractionSource() }, indication = null, onClick = viewModel::close),
        )
    }
    AnimatedVisibility(
        state.open,
        modifier = Modifier.align(Alignment.BottomCenter),
        enter = slideInVertically(tween(Motion.NORMAL, easing = Motion.smoothEnter)) { it },
        exit = slideOutVertically(tween(Motion.FAST, easing = Motion.exit)) { it },
    ) {
        Column(
            Modifier.fillMaxWidth().fillMaxHeight(SHEET_HEIGHT)
                .clip(RoundedCornerShape(topStart = 28.dp, topEnd = 28.dp))
                .background(if (colors.dark) colors.surfaceSecondary else colors.surfacePrimary)
                .clickable(remember { MutableInteractionSource() }, indication = null) {}
                .navigationBarsPadding(),
        ) {
            Box(
                Modifier.padding(top = Space.s8).align(Alignment.CenterHorizontally).size(36.dp, 4.dp)
                    .clip(Radius.pill).background(colors.borderPrimary),
            )
            Row(Modifier.padding(start = Space.page, end = Space.s8, top = Space.s8), verticalAlignment = Alignment.CenterVertically) {
                Label(stringResource(R.string.picker_title, desktopName), LinkTheme.type.headlineMedium, modifier = Modifier.weight(1f), maxLines = 1)
                IconButton(Icons.Filled.Close, viewModel::close)
            }
            SlidingPillControl(
                listOf(stringResource(R.string.picker_photos), stringResource(R.string.picker_videos), stringResource(R.string.picker_files)),
                selected = state.tab.ordinal,
                onSelect = { viewModel.select(PickerTab.entries[it]) },
                modifier = Modifier.padding(horizontal = Space.page, vertical = Space.s12).fillMaxWidth(),
            )
            Box(Modifier.weight(1f).fillMaxWidth()) {
                when (state.tab) {
                    PickerTab.Photos -> MediaGrid(viewModel, state, state.photos)
                    PickerTab.Videos -> MediaGrid(viewModel, state, state.videos)
                    PickerTab.Files -> FileList(viewModel, state)
                }
            }
            if (state.selected.isNotEmpty()) SendBar(state, onSend)
        }
    }
}

@Composable
private fun MediaGrid(viewModel: PickerViewModel, state: PickerState, items: List<PhoneItem>) {
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { viewModel.refresh() }
    if (state.access == MediaAccess.None) {
        Grant(stringResource(R.string.picker_media_title), stringResource(R.string.picker_media_hint), stringResource(R.string.picker_media_allow)) {
            ask.launch(PhoneFiles.mediaPermissions)
        }
        return
    }
    Column {
        if (state.access == MediaAccess.Partial) {
            Row(
                Modifier.fillMaxWidth().clickable { ask.launch(PhoneFiles.mediaPermissions) }
                    .padding(horizontal = Space.page, vertical = Space.s8),
            ) {
                Label(stringResource(R.string.picker_partial), LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary, Modifier.weight(1f))
                Label(stringResource(R.string.picker_partial_more), LinkTheme.type.titleSmall, LinkTheme.colors.accentText)
            }
        }
        if (items.isEmpty()) {
            Hint(stringResource(R.string.picker_empty))
            return
        }
        LazyVerticalGrid(
            GridCells.Fixed(GRID_COLUMNS),
            contentPadding = PaddingValues(horizontal = Space.s12, vertical = Space.s4),
            horizontalArrangement = Arrangement.spacedBy(Space.s4),
            verticalArrangement = Arrangement.spacedBy(Space.s4),
        ) {
            items(items, key = { it.key }) { item ->
                MediaTile(item, state.selected.keys.indexOf(item.key), viewModel::thumbnail, Modifier.fillMaxWidth()) { viewModel.toggle(item) }
            }
        }
    }
}

@Composable
private fun FileList(viewModel: PickerViewModel, state: PickerState) {
    val context = LocalContext.current
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { viewModel.refresh() }
    if (!state.allFiles) {
        Grant(stringResource(R.string.picker_files_title), stringResource(R.string.picker_files_hint), stringResource(R.string.picker_files_allow)) {
            if (Build.VERSION.SDK_INT >= 30) {
                context.startActivity(
                    Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:${context.packageName}")),
                )
            } else {
                ask.launch(PhoneFiles.mediaPermissions)
            }
        }
        return
    }
    Column {
        Row(
            Modifier.fillMaxWidth().padding(start = Space.s8, end = Space.page),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(Icons.AutoMirrored.Filled.ArrowBack, { viewModel.up() })
            val path = (listOf(stringResource(R.string.picker_storage)) + state.path).joinToString(" / ")
            Label(path, LinkTheme.type.titleMedium, LinkTheme.colors.textSecondary, maxLines = 1)
        }
        val entries = state.entries
        if (entries.isNullOrEmpty()) {
            val text = if (entries == null) R.string.picker_folder_unreadable else R.string.picker_folder_empty
            Hint(stringResource(text))
            return
        }
        LazyColumn(contentPadding = PaddingValues(bottom = Space.s8)) {
            items(entries, key = { it.key }) { item ->
                val selected = item.key in state.selected
                Row(
                    Modifier.fillMaxWidth()
                        .clickable { item.folder?.let(viewModel::openFolder) ?: viewModel.toggle(item) }
                        .background(if (selected) LinkTheme.colors.accent.copy(alpha = 0.10f) else Color.Transparent)
                        .padding(horizontal = Space.page, vertical = Space.s10),
                    horizontalArrangement = Arrangement.spacedBy(Space.s12),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    FileIcon(item, viewModel::thumbnail)
                    Column(Modifier.weight(1f)) {
                        Label(item.name, LinkTheme.type.titleMedium, maxLines = 1)
                        if (item.folder == null) {
                            Label(Formatter.formatShortFileSize(context, item.size), LinkTheme.type.bodySmall, LinkTheme.colors.textSecondary)
                        }
                    }
                    if (selected) CheckBadge(null)
                }
            }
        }
    }
}

@Composable
private fun FileIcon(item: PhoneItem, thumbnail: suspend (PhoneItem, Int) -> ImageBitmap?) {
    val colors = LinkTheme.colors
    val box = Modifier.size(Size.iconContainer).clip(Radius.small)
    if (item.visual) {
        Thumbnail(item, Size.iconContainer, thumbnail, box.background(colors.surfaceTertiary))
    } else {
        Box(box.background(colors.accent.copy(alpha = 0.14f)), contentAlignment = Alignment.Center) {
            Glyph(if (item.folder != null) FolderGlyph else FileGlyph, colors.accentText, Size.icon - 2.dp)
        }
    }
}

/** A square photo or video with its pick order when selected; shared by Home's recent row. */
@Composable
fun MediaTile(
    item: PhoneItem,
    order: Int,
    thumbnail: suspend (PhoneItem, Int) -> ImageBitmap?,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    val colors = LinkTheme.colors
    val selected = order >= 0
    Box(
        modifier.aspectRatio(1f).clip(Radius.small).background(colors.surfaceTertiary).clickable(onClick = onClick)
            .then(if (selected) Modifier.border(3.dp, colors.accent, Radius.small) else Modifier),
    ) {
        Thumbnail(item, THUMB_DP, thumbnail, Modifier.fillMaxSize())
        if (item.mime.startsWith("video/")) {
            Glyph(Icons.Filled.PlayArrow, Color.White, Size.iconSmall + 4.dp, Modifier.align(Alignment.BottomStart).padding(Space.s6))
        }
        if (selected) CheckBadge(order + 1, Modifier.align(Alignment.TopEnd).padding(Space.s6))
    }
}

@Composable
private fun Thumbnail(item: PhoneItem, size: Dp, thumbnail: suspend (PhoneItem, Int) -> ImageBitmap?, modifier: Modifier) {
    val px = with(LocalDensity.current) { size.roundToPx() }
    val bitmap by produceState<ImageBitmap?>(null, item.key, px) { value = thumbnail(item, px) }
    Box(modifier) {
        bitmap?.let { Image(it, contentDescription = item.name, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize()) }
    }
}

@Composable
private fun CheckBadge(number: Int?, modifier: Modifier = Modifier) {
    val colors = LinkTheme.colors
    Box(modifier.size(24.dp).clip(Radius.pill).background(colors.accent), contentAlignment = Alignment.Center) {
        if (number == null) Glyph(Icons.Filled.Check, colors.onAccent, Size.iconSmall)
        else Label(number.toString(), LinkTheme.type.labelLarge, colors.onAccent)
    }
}

@Composable
private fun SendBar(state: PickerState, onSend: () -> Unit) {
    PillButton(sendLabel(state.selected), onSend, Modifier.fillMaxWidth().padding(horizontal = Space.page, vertical = Space.s12))
}

/** "Send 3 items · 12 MB". */
@Composable
fun sendLabel(selected: Map<String, PhoneItem>): String {
    val context = LocalContext.current
    val bytes = Formatter.formatShortFileSize(context, selected.values.sumOf { it.size })
    return context.resources.getQuantityString(R.plurals.picker_send, selected.size, selected.size, bytes)
}

@Composable
private fun Hint(text: String) {
    Label(
        text,
        LinkTheme.type.bodyLarge,
        LinkTheme.colors.textSecondary,
        Modifier.fillMaxWidth().padding(top = Space.s48),
        textAlign = TextAlign.Center,
    )
}

@Composable
private fun Grant(title: String, body: String, action: String, onAllow: () -> Unit) {
    Column(
        Modifier.fillMaxWidth().padding(horizontal = Space.page, vertical = Space.s32),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(Space.s20),
    ) {
        CenteredText(title, body)
        PillButton(action, onAllow, kind = PillKind.Filled)
    }
}

@Composable
private fun IconButton(icon: ImageVector, onClick: () -> Unit) {
    Box(Modifier.size(Size.touch).clip(Radius.pill).clickable(onClick = onClick), contentAlignment = Alignment.Center) {
        Glyph(icon, LinkTheme.colors.textSecondary)
    }
}

private fun glyph(name: String, path: String) = ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f)
    .addPath(PathParser().parsePathString(path).toNodes(), fill = SolidColor(Color.Black))
    .build()

private val FolderGlyph = glyph("Folder", "M10 4H4c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2h-8l-2-2z")
private val FileGlyph = glyph("File", "M14 2H6c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V8l-6-6zm-1 7V3.5L18.5 9H13z")

private const val SHEET_HEIGHT = 0.88f
private const val GRID_COLUMNS = 3
private val THUMB_DP = 128.dp
