package org.umbriel.link.clipboard

import android.content.ClipData
import android.content.ClipDescription
import android.content.ClipboardManager
import android.content.Context
import java.security.MessageDigest
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.core.domain.IncomingClip

/**
 * Desktop to phone: text is set at once, other types as a `content://` URI [ClipProvider] pulls when an app reads it.
 * A clip set from a desktop is cleared after two minutes unless the clipboard changed since. The hash of the last one
 * set here keeps it from being offered back.
 */
class ClipboardSync(
    private val context: Context,
    private val repository: LinkRepository,
    private val scope: CoroutineScope,
) {
    private val clipboard = context.getSystemService(ClipboardManager::class.java)
    private var clear: Job? = null

    /** SHA-256 of the last text set from a desktop. */
    @Volatile
    var lastApplied: String? = null
        private set

    fun start() {
        scope.launch { repository.clips.collect(::apply) }
    }

    /** Whether [text] is what a desktop just set, so sending it would echo. */
    fun isEcho(text: String): Boolean = hash(text) == lastApplied

    /** Sends the original content, not a URI coerced to text; our lazy desktop URIs must not be echoed back. */
    suspend fun offer(clip: ClipData): Result<Unit> {
        val item = clip.getItemAt(0)
        item.uri?.let { uri ->
            if (ClipProvider.owns(uri)) return Result.success(Unit)
            val mime = (0 until clip.description.mimeTypeCount)
                .map { clip.description.getMimeType(it) }.firstOrNull { '*' !in it }
            return repository.offerClipUri(uri, mime)
        }
        val text = item.coerceToText(context)?.toString()
        if (text.isNullOrEmpty() || isEcho(text)) return Result.success(Unit)
        return repository.offerClipText(text)
    }

    /** The clipboard changed here; a desktop clip is no longer there to clear. */
    fun changedLocally() {
        clear?.cancel()
        clear = null
    }

    private fun apply(clip: IncomingClip) {
        val data = clip.text?.let { text ->
            lastApplied = hash(text)
            ClipData.newPlainText(LABEL, text)
        } ?: run {
            val mime = clip.mimes.first()
            ClipData(ClipDescription(LABEL, arrayOf(mime)), ClipData.Item(ClipProvider.uri(clip.desktopId, clip.clipId, mime)))
        }
        clipboard.setPrimaryClip(data)
        clear?.cancel()
        clear = scope.launch {
            delay(CLEAR_AFTER_MS)
            clipboard.clearPrimaryClip()
        }
    }

    companion object {
        const val LABEL = "Umbriel Link"
        private const val CLEAR_AFTER_MS = 120_000L

        fun hash(text: String): String =
            MessageDigest.getInstance("SHA-256").digest(text.toByteArray()).joinToString("") { "%02x".format(it) }
    }
}
