package org.umbriel.link.clipboard

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import org.umbriel.link.LinkApplication

/**
 * Serves a desktop's clipboard lazily: `content://org.umbriel.link.clip/<desktop>/<clip>/<type>`. Opening it pulls the
 * type from the desktop into a pipe, so the bytes move only when an app pastes.
 */
class ClipProvider : ContentProvider() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onCreate(): Boolean = true

    override fun getType(uri: Uri): String? = uri.pathSegments.getOrNull(2)

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        val (desktop, clip, mime) = uri.pathSegments.takeIf { it.size == 3 } ?: throw IllegalArgumentException("$uri")
        val clipId = clip.toLongOrNull() ?: throw IllegalArgumentException("$uri")
        val repository = (context!!.applicationContext as LinkApplication).container.repository
        val (read, write) = ParcelFileDescriptor.createPipe()
        scope.launch { repository.pullClip(desktop, clipId, mime, write.detachFd()) }
        return read
    }

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, args: Array<out String>?, sort: String?): Cursor? = null

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    override fun delete(uri: Uri, selection: String?, args: Array<out String>?): Int = 0

    override fun update(uri: Uri, values: ContentValues?, selection: String?, args: Array<out String>?): Int = 0

    companion object {
        private const val AUTHORITY = "org.umbriel.link.clip"

        fun owns(uri: Uri): Boolean = uri.scheme == "content" && uri.authority == AUTHORITY

        fun uri(desktopId: String, clipId: Long, mime: String): Uri =
            Uri.Builder().scheme("content").authority(AUTHORITY).appendPath(desktopId).appendPath(clipId.toString())
                .appendPath(mime).build()
    }
}
