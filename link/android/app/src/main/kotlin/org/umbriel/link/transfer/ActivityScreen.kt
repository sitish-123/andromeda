package org.umbriel.link.transfer

import android.app.DownloadManager
import android.content.ActivityNotFoundException
import android.content.Intent
import android.text.format.DateUtils
import android.text.format.Formatter
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.umbriel.link.R
import org.umbriel.link.core.domain.TransferRecord
import org.umbriel.link.ui.components.DashboardCard
import org.umbriel.link.ui.components.Footnote
import org.umbriel.link.ui.components.Glyph
import org.umbriel.link.ui.components.IconChip
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.LinkIcons
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.Readout
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.components.SlidingPillControl
import org.umbriel.link.ui.text
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Size
import org.umbriel.link.ui.theme.Space

private enum class Filter { All, Received, Sent }

/** The transfer history by day: each transfer's files, openable from here in either direction. */
@Composable
fun ActivityScreen(viewModel: ActivityViewModel) {
    val transfers by viewModel.transfers.collectAsStateWithLifecycle()
    val busy by viewModel.busy.collectAsStateWithLifecycle()
    val message by viewModel.message.collectAsStateWithLifecycle()
    val context = LocalContext.current
    var filter by rememberSaveable { mutableStateOf(Filter.All) }
    var openFailure by remember { mutableStateOf<String?>(null) }
    val open: (() -> Intent?) -> Unit = { intent ->
        try {
            intent()?.let(context::startActivity) ?: run { openFailure = context.getString(R.string.activity_file_gone) }
        } catch (_: ActivityNotFoundException) {
            openFailure = context.getString(R.string.activity_no_file_app)
        } catch (error: SecurityException) {
            openFailure = context.getString(R.string.failure_other, error.message.orEmpty())
        }
    }
    val toast = openFailure ?: when (val shown = message) {
        is ActivityMessage.Failed -> shown.failure.text(context.resources)
        ActivityMessage.Expired -> stringResource(R.string.activity_expired)
        null -> null
    }
    val shown = transfers.filter {
        when (filter) {
            Filter.All -> true
            Filter.Received -> it.incoming == true
            Filter.Sent -> it.incoming == false
        }
    }
    val downloads = stringResource(R.string.activity_downloads)
    Screen(
        stringResource(R.string.activity_title),
        toast = toast,
        onToastShown = { openFailure = null; viewModel.messageShown() },
        actions = {
            Box(
                Modifier.padding(end = Space.s8).size(Size.touch).clip(Radius.pill).background(LinkTheme.colors.surfaceTertiary)
                    .clickable(role = Role.Button) { open { Intent(DownloadManager.ACTION_VIEW_DOWNLOADS) } }
                    .semantics { contentDescription = downloads },
                contentAlignment = Alignment.Center,
            ) { Glyph(LinkIcons.Folder, LinkTheme.colors.textSecondary, Size.icon - 4.dp) }
        },
    ) {
        LazyColumn(
            Modifier.fillMaxSize(),
            contentPadding = PaddingValues(horizontal = Space.page, vertical = Space.s8),
            verticalArrangement = Arrangement.spacedBy(Space.s12),
        ) {
            item {
                SlidingPillControl(
                    listOf(
                        stringResource(R.string.activity_filter_all),
                        stringResource(R.string.activity_filter_received),
                        stringResource(R.string.activity_filter_sent),
                    ),
                    filter.ordinal,
                    { filter = Filter.entries[it] },
                )
            }
            if (shown.isEmpty()) {
                item {
                    Column(Modifier.padding(top = Space.s32), verticalArrangement = Arrangement.spacedBy(Space.s8)) {
                        Readout(stringResource(R.string.activity_empty_readout), LinkTheme.colors.textTertiary)
                        Label(stringResource(R.string.activity_empty_title), LinkTheme.type.headlineLarge)
                        Label(stringResource(R.string.activity_empty_hint), LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
                    }
                }
            }
            shown.groupBy { day(it.time) }.forEach { (day, records) ->
                item(key = "day-$day") { Readout(dayLabel(day), modifier = Modifier.padding(start = Space.s4, top = Space.s12)) }
                items(records, key = { it.transferId }) { transfer ->
                    TransferCard(
                        transfer, transfer.transferId in busy,
                        { viewModel.accept(transfer.transferId) },
                        { viewModel.decline(transfer.transferId) },
                        { viewModel.cancel(transfer.transferId) },
                    ) { uri -> open { viewIntent(context, uri) } }
                }
            }
            item { Footnote(stringResource(R.string.activity_kept_hint)) }
            item { Spacer(Modifier.height(Space.s16)) }
        }
    }
}

@Composable
private fun TransferCard(
    transfer: TransferRecord,
    busy: Boolean,
    onAccept: () -> Unit,
    onDecline: () -> Unit,
    onCancel: () -> Unit,
    onOpen: (String) -> Unit,
) {
    val colors = LinkTheme.colors
    val context = LocalContext.current
    val incoming = transfer.incoming == true
    val desktop = transfer.desktopName.ifEmpty { stringResource(R.string.app_name) }
    var expanded by rememberSaveable(transfer.transferId) { mutableStateOf(false) }
    // A received file is openable once it is published; a sent one from where it was read.
    val rows = if (incoming && transfer.saved.isNotEmpty()) {
        transfer.saved.map { saved -> FileRow(saved.name, transfer.files.firstOrNull { it.name == saved.name }?.size, saved.uri) }
    } else {
        transfer.files.map { FileRow(it.name, it.size, if (incoming) null else it.uri) }
    }
    DashboardCard(Modifier.fillMaxWidth()) {
        Row(Modifier.padding(top = Space.s8), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Space.s12)) {
            IconChip(if (incoming) LinkIcons.Download else LinkIcons.Upload)
            Column(Modifier.weight(1f)) {
                Label(
                    stringResource(if (incoming) R.string.activity_from else R.string.activity_sent_to, desktop),
                    LinkTheme.type.titleMedium, maxLines = 1,
                )
                Readout(
                    listOfNotNull(
                        DateUtils.formatDateTime(context, transfer.time, DateUtils.FORMAT_SHOW_TIME),
                        context.resources.getQuantityString(R.plurals.activity_files, rows.size, rows.size),
                        transfer.total.takeIf { it > 0 }?.let { Formatter.formatShortFileSize(context, it) },
                    ).joinToString(" · "),
                    colors.textTertiary,
                )
            }
            StatusTag(transfer.status)
        }
        when (transfer.status) {
            "transferring" -> Progress(transfer, busy, onCancel)
            "offered" -> Row(Modifier.padding(top = Space.s4), horizontalArrangement = Arrangement.spacedBy(Space.s8)) {
                PillButton(stringResource(R.string.transfer_accept), onAccept, Modifier.weight(1f), enabled = !busy)
                PillButton(stringResource(R.string.transfer_decline), onDecline, Modifier.weight(1f), PillKind.Tonal, enabled = !busy)
            }
        }
        Column(Modifier.padding(top = Space.s4, bottom = Space.s4), verticalArrangement = Arrangement.spacedBy(Space.s4)) {
            rows.take(if (expanded) rows.size else COLLAPSED).forEach { row -> FileLine(row, onOpen) }
            if (rows.size > COLLAPSED && !expanded) {
                PillButton(stringResource(R.string.activity_more, rows.size - COLLAPSED), { expanded = true }, kind = PillKind.Quiet)
            }
        }
    }
}

private data class FileRow(val name: String, val size: Long?, val uri: String?)

@Composable
private fun FileLine(row: FileRow, onOpen: (String) -> Unit) {
    val colors = LinkTheme.colors
    val context = LocalContext.current
    val uri = row.uri
    Row(
        Modifier.fillMaxWidth().clip(Radius.list)
            .then(if (uri != null) Modifier.clickable(role = Role.Button) { onOpen(uri) } else Modifier)
            .padding(vertical = Space.s6),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(Space.s12),
    ) {
        val thumbnail = uri?.let { rememberThumbnail(it, row.name) }
        Box(
            Modifier.size(44.dp).clip(Radius.button).background(colors.surfacePrimary),
            contentAlignment = Alignment.Center,
        ) {
            if (thumbnail != null) {
                Image(thumbnail, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
            } else {
                Readout(extension(row.name), colors.textTertiary)
            }
        }
        Column(Modifier.weight(1f)) {
            Label(row.name, LinkTheme.type.titleSmall, maxLines = 1)
            row.size?.let { Label(Formatter.formatShortFileSize(context, it), LinkTheme.type.bodySmall, colors.textTertiary) }
        }
        if (uri != null) Readout(stringResource(R.string.activity_open), colors.accentText)
    }
}

@Composable
private fun Progress(transfer: TransferRecord, busy: Boolean, onCancel: () -> Unit) {
    val colors = LinkTheme.colors
    val context = LocalContext.current
    val fraction = if (transfer.total <= 0) 0f else (transfer.bytes.toDouble() / transfer.total).toFloat().coerceIn(0f, 1f)
    Row(Modifier.padding(top = Space.s4), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Space.s12)) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(Space.s6)) {
            Box(
                Modifier.fillMaxWidth().height(Space.s4).clip(Radius.pill).background(colors.surfacePrimary)
                    .semantics { progressBarRangeInfo = ProgressBarRangeInfo(fraction, 0f..1f) },
            ) { Box(Modifier.fillMaxWidth(fraction).height(Space.s4).background(colors.accent)) }
            Readout(
                stringResource(
                    R.string.transfer_bytes,
                    Formatter.formatShortFileSize(context, transfer.bytes),
                    Formatter.formatShortFileSize(context, transfer.total),
                ),
                colors.textTertiary,
            )
        }
        PillButton(stringResource(R.string.cancel), onCancel, kind = PillKind.Quiet, enabled = !busy)
    }
}

@Composable
private fun StatusTag(status: String) {
    val colors = LinkTheme.colors
    val (label, color) = when (status) {
        "offered" -> R.string.activity_awaiting_short to colors.warning
        "transferring" -> R.string.activity_moving_short to colors.accentText
        "done" -> R.string.activity_done to colors.success
        "declined" -> R.string.activity_declined to colors.textTertiary
        "cancelled" -> R.string.activity_cancelled to colors.textTertiary
        "too-large" -> R.string.activity_too_large to colors.error
        "no-space" -> R.string.activity_no_space to colors.error
        "busy" -> R.string.activity_busy to colors.error
        else -> R.string.activity_failed to colors.error
    }
    Readout(stringResource(label), color)
}

@Composable
private fun dayLabel(day: Long): String = when (day) {
    day(System.currentTimeMillis()) -> stringResource(R.string.activity_today)
    day(System.currentTimeMillis() - DateUtils.DAY_IN_MILLIS) -> stringResource(R.string.activity_yesterday)
    else -> DateUtils.formatDateTime(LocalContext.current, day, DateUtils.FORMAT_SHOW_DATE or DateUtils.FORMAT_SHOW_WEEKDAY)
}

/** Local midnight of [time], so a day's transfers group together. */
private fun day(time: Long): Long = java.util.Calendar.getInstance().run {
    timeInMillis = time
    set(java.util.Calendar.HOUR_OF_DAY, 0)
    set(java.util.Calendar.MINUTE, 0)
    set(java.util.Calendar.SECOND, 0)
    set(java.util.Calendar.MILLISECOND, 0)
    timeInMillis
}

private fun extension(name: String): String = name.substringAfterLast('.', "").take(4).ifEmpty { "file" }

private const val COLLAPSED = 4
