package org.umbriel.link.files

import android.Manifest
import android.content.ContentResolver
import android.content.ContentUris
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.MediaStore
import android.util.LruCache
import android.util.Size
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.core.content.ContextCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.umbriel.link.core.data.mimeTypeOf
import java.io.File

enum class MediaKind { Photos, Videos }

/** A file on this phone; [uri] is what the send path opens, a MediaStore or a `file:` URI. */
data class PhoneItem(
    val uri: Uri,
    val name: String,
    val size: Long,
    val mime: String,
    val modified: Long,
    val folder: File? = null,
) {
    val key: String get() = uri.toString()
    val visual: Boolean get() = mime.startsWith("image/") || mime.startsWith("video/")
}

enum class MediaAccess { None, Partial, Full }

/** The phone's shared storage: MediaStore for photos and videos, the file tree under All files access. */
class PhoneFiles(private val context: Context) {
    private val thumbnails = LruCache<String, ImageBitmap>(THUMBNAIL_CACHE)

    val root: File get() = Environment.getExternalStorageDirectory()

    fun mediaAccess(): MediaAccess = when {
        allFiles() || granted(if (Build.VERSION.SDK_INT >= 33) Manifest.permission.READ_MEDIA_IMAGES else Manifest.permission.READ_EXTERNAL_STORAGE) ->
            MediaAccess.Full
        Build.VERSION.SDK_INT >= 34 && granted(Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED) -> MediaAccess.Partial
        else -> MediaAccess.None
    }

    /** Whether the whole shared storage can be listed: All files access, or on Android 10 the legacy read grant. */
    fun allFiles(): Boolean =
        if (Build.VERSION.SDK_INT >= 30) Environment.isExternalStorageManager() else granted(Manifest.permission.READ_EXTERNAL_STORAGE)

    suspend fun media(kind: MediaKind, limit: Int = MEDIA_LIMIT): List<PhoneItem> = withContext(Dispatchers.IO) {
        val collection = when (kind) {
            MediaKind.Photos -> MediaStore.Images.Media.EXTERNAL_CONTENT_URI
            MediaKind.Videos -> MediaStore.Video.Media.EXTERNAL_CONTENT_URI
        }
        val projection = arrayOf(
            MediaStore.MediaColumns._ID,
            MediaStore.MediaColumns.DISPLAY_NAME,
            MediaStore.MediaColumns.SIZE,
            MediaStore.MediaColumns.MIME_TYPE,
            MediaStore.MediaColumns.DATE_ADDED,
        )
        val query = Bundle().apply {
            putStringArray(ContentResolver.QUERY_ARG_SORT_COLUMNS, arrayOf(MediaStore.MediaColumns.DATE_ADDED))
            putInt(ContentResolver.QUERY_ARG_SORT_DIRECTION, ContentResolver.QUERY_SORT_DIRECTION_DESCENDING)
            putInt(ContentResolver.QUERY_ARG_LIMIT, limit)
        }
        context.contentResolver.query(collection, projection, query, null)?.use { cursor ->
            buildList {
                while (cursor.moveToNext()) {
                    add(
                        PhoneItem(
                            uri = ContentUris.withAppendedId(collection, cursor.getLong(0)),
                            name = cursor.getString(1) ?: "file",
                            size = cursor.getLong(2),
                            mime = cursor.getString(3) ?: mimeTypeOf(cursor.getString(1).orEmpty()),
                            modified = cursor.getLong(4),
                        ),
                    )
                }
            }
        }.orEmpty()
    }

    /** Folders first, then files, each by name; hidden entries are left out. Null when [dir] cannot be read. */
    suspend fun list(dir: File): List<PhoneItem>? = withContext(Dispatchers.IO) {
        dir.listFiles()?.filterNot { it.name.startsWith(".") }
            ?.sortedWith(compareBy<File> { !it.isDirectory }.thenBy { it.name.lowercase() })
            ?.map { file ->
                PhoneItem(
                    uri = Uri.fromFile(file),
                    name = file.name,
                    size = if (file.isDirectory) 0 else file.length(),
                    mime = if (file.isDirectory) FOLDER else mimeTypeOf(file.name),
                    modified = file.lastModified() / 1000,
                    folder = file.takeIf { it.isDirectory },
                )
            }
    }

    /** A square thumbnail at most [px] wide, cached; null when the item has none. */
    suspend fun thumbnail(item: PhoneItem, px: Int): ImageBitmap? {
        val key = "${item.key}@$px"
        thumbnails.get(key)?.let { return it }
        return withContext(Dispatchers.IO) {
            runCatching {
                val bitmap = if (item.uri.scheme == "file") {
                    val file = File(requireNotNull(item.uri.path))
                    if (item.mime.startsWith("video/")) {
                        android.media.ThumbnailUtils.createVideoThumbnail(file, Size(px, px), null)
                    } else {
                        android.media.ThumbnailUtils.createImageThumbnail(file, Size(px, px), null)
                    }
                } else {
                    context.contentResolver.loadThumbnail(item.uri, Size(px, px), null)
                }
                bitmap.asImageBitmap()
            }.getOrNull()?.also { thumbnails.put(key, it) }
        }
    }

    private fun granted(permission: String) =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

    companion object {
        const val FOLDER = "inode/directory"
        private const val MEDIA_LIMIT = 600
        private const val THUMBNAIL_CACHE = 240

        /** What to ask Android for so Photos and Videos can list the library. */
        val mediaPermissions: Array<String> = when {
            Build.VERSION.SDK_INT >= 34 -> arrayOf(
                Manifest.permission.READ_MEDIA_IMAGES,
                Manifest.permission.READ_MEDIA_VIDEO,
                Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED,
            )
            Build.VERSION.SDK_INT >= 33 -> arrayOf(Manifest.permission.READ_MEDIA_IMAGES, Manifest.permission.READ_MEDIA_VIDEO)
            else -> arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE)
        }
    }
}
