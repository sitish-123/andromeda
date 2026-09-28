package org.umbriel.link.core.data

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.provider.MediaStore
import android.provider.OpenableColumns
import java.io.File
import java.security.MessageDigest

/**
 * Received files, which the core verified in app storage, are published to MediaStore.Downloads: the entry stays
 * pending (invisible to other apps) until its copy hashes to the value the core verified, then the private file goes.
 */
internal class Downloads(private val context: Context) {

    /** The published entry, or null when the copy failed or did not match [sha256]. */
    fun publish(file: File, sha256: String): Uri? {
        val resolver = context.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, file.name)
            put(MediaStore.Downloads.IS_PENDING, 1)
        }
        val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values) ?: return null
        val digest = MessageDigest.getInstance("SHA-256")
        val copied = runCatching {
            resolver.openOutputStream(uri)!!.use { out ->
                file.inputStream().use { input ->
                    val buffer = ByteArray(BUFFER)
                    while (true) {
                        val read = input.read(buffer)
                        if (read < 0) break
                        digest.update(buffer, 0, read)
                        out.write(buffer, 0, read)
                    }
                }
            }
        }.isSuccess
        val hex = digest.digest().joinToString("") { "%02x".format(it) }
        if (!copied || hex != sha256) {
            resolver.delete(uri, null, null)
            return null
        }
        resolver.update(uri, ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }, null, null)
        file.delete()
        return uri
    }

    /**
     * The display name, size, and type of a shared content URI, and a descriptor the core takes over. Throws
     * [IllegalArgumentException] with the reason when the URI is not a readable regular file.
     */
    fun open(uri: Uri): Opened {
        val resolver = context.contentResolver
        val name = resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        } ?: uri.lastPathSegment ?: "file"
        val descriptor = requireNotNull(resolver.openFileDescriptor(uri, "r")) { "$name cannot be opened" }
        val size = descriptor.statSize
        if (size < 0) {
            descriptor.close()
            throw IllegalArgumentException("$name is not a regular file")
        }
        return Opened(descriptor.detachFd(), name, size, resolver.getType(uri) ?: OCTET_STREAM)
    }

    data class Opened(val fd: Int, val name: String, val size: Long, val mime: String)

    private companion object {
        const val BUFFER = 256 * 1024
        const val OCTET_STREAM = "application/octet-stream"
    }
}
