package org.umbriel.link.core.data

import android.content.Context
import android.graphics.Bitmap
import android.media.ThumbnailUtils
import android.provider.MediaStore
import android.util.Size
import java.io.ByteArrayOutputStream
import java.io.File
import org.umbriel.link.ffi.LibraryItem
import org.umbriel.link.ffi.PhoneLibrary

/**
 * The media store by album, for a desktop browsing `Photos`: images and videos, which the core reads by path (the
 * media permissions allow that), and PNG thumbnails of them. Called off the main thread.
 */
class MediaLibrary(private val context: Context) : PhoneLibrary {
    override fun albums(): List<String> = query(null).map { it.album }.distinct().sorted()

    override fun album(name: String): List<LibraryItem> = query(name).map { it.item }

    override fun thumbnail(path: String, px: UShort): ByteArray? = runCatching {
        val file = File(path)
        val size = Size(px.toInt(), px.toInt())
        val bitmap = if (mimeTypeOf(file.name).startsWith("video/")) {
            ThumbnailUtils.createVideoThumbnail(file, size, null)
        } else {
            ThumbnailUtils.createImageThumbnail(file, size, null)
        }
        ByteArrayOutputStream().also { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }.toByteArray()
            .also { bitmap.recycle() }
    }.getOrNull()

    private class Row(val album: String, val item: LibraryItem)

    private fun query(album: String?): List<Row> {
        val type = MediaStore.Files.FileColumns.MEDIA_TYPE
        val bucket = MediaStore.MediaColumns.BUCKET_DISPLAY_NAME
        val selection = "($type=? OR $type=?)" + if (album != null) " AND $bucket=?" else ""
        val args = listOfNotNull(
            MediaStore.Files.FileColumns.MEDIA_TYPE_IMAGE.toString(),
            MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO.toString(),
            album,
        ).toTypedArray()
        val projection = arrayOf(
            bucket,
            MediaStore.MediaColumns.DISPLAY_NAME,
            @Suppress("DEPRECATION") MediaStore.MediaColumns.DATA,
            MediaStore.MediaColumns.SIZE,
            MediaStore.MediaColumns.DATE_MODIFIED,
        )
        val collection = MediaStore.Files.getContentUri(MediaStore.VOLUME_EXTERNAL)
        return context.contentResolver.query(collection, projection, selection, args, null)?.use { cursor ->
            buildList {
                while (cursor.moveToNext()) {
                    val name = cursor.getString(1) ?: continue
                    val path = cursor.getString(2) ?: continue
                    val item = LibraryItem(name, path, cursor.getLong(3).toULong(), cursor.getLong(4).toULong())
                    add(Row(cursor.getString(0) ?: "Other", item))
                }
            }
        }.orEmpty()
    }
}
