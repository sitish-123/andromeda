package org.umbriel.link.transfer

import android.content.Context
import android.content.Intent
import android.media.ThumbnailUtils
import android.net.Uri
import android.provider.MediaStore
import android.util.Size
import android.webkit.MimeTypeMap
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import java.io.File
import java.io.IOException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * An intent that opens [uri] in another app, or null when the file is gone. A `file:` URI, which the Files tab sends
 * from, cannot leave this app, so it is swapped for the file's MediaStore entry.
 */
fun viewIntent(context: Context, uri: String): Intent? {
    val parsed = Uri.parse(uri)
    val content = if (parsed.scheme == "file") mediaStoreUri(context, File(requireNotNull(parsed.path))) else parsed
    content ?: return null
    val mime = context.contentResolver.getType(content) ?: mimeOf(content.lastPathSegment.orEmpty())
    return Intent(Intent.ACTION_VIEW).setDataAndType(content, mime).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
}

private fun mediaStoreUri(context: Context, file: File): Uri? {
    if (!file.exists()) return null
    val files = MediaStore.Files.getContentUri("external")
    return context.contentResolver.query(
        files, arrayOf(MediaStore.MediaColumns._ID), "${MediaStore.MediaColumns.DATA} = ?", arrayOf(file.path), null,
    )?.use { cursor -> if (cursor.moveToFirst()) Uri.withAppendedPath(files, cursor.getLong(0).toString()) else null }
}

private fun mimeOf(name: String): String =
    MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substringAfterLast('.', "").lowercase()) ?: "*/*"

/** A small preview of an image or video at [uri], or null for other files and ones that cannot be read. */
@Composable
fun rememberThumbnail(uri: String, name: String): ImageBitmap? {
    val context = LocalContext.current
    val px = with(LocalDensity.current) { 44.dp.roundToPx() }
    val thumbnail by produceState<ImageBitmap?>(null, uri) {
        val mime = mimeOf(name)
        if (!mime.startsWith("image/") && !mime.startsWith("video/")) return@produceState
        value = withContext(Dispatchers.IO) {
            try {
                val parsed = Uri.parse(uri)
                val size = Size(px, px)
                val bitmap = when {
                    parsed.scheme != "file" -> context.contentResolver.loadThumbnail(parsed, size, null)
                    mime.startsWith("image/") -> ThumbnailUtils.createImageThumbnail(File(requireNotNull(parsed.path)), size, null)
                    else -> ThumbnailUtils.createVideoThumbnail(File(requireNotNull(parsed.path)), size, null)
                }
                bitmap.asImageBitmap()
            } catch (_: IOException) {
                null
            } catch (_: SecurityException) {
                null
            }
        }
    }
    return thumbnail
}
