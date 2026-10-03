package org.umbriel.link.fixture

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.io.File
import kotlin.concurrent.thread

/** Copies a PNG from another app with a temporary URI grant, served through a pipe rather than a regular file. */
class ImageClipboardActivity : Activity() {
    private var copied = false

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus || copied) return
        copied = true
        val bitmap = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888)
        for (y in 0 until 64) for (x in 0 until 64) bitmap.setPixel(x, y, Color.rgb(x * 4, y * 4, (x + y) * 2))
        File(filesDir, "clipboard.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        bitmap.recycle()
        val uri = Uri.parse("content://org.umbriel.link.fixture.clip/clipboard.png")
        getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newUri(contentResolver, "Screenshot", uri))
        finish()
    }
}

class ImageClipboardProvider : ContentProvider() {
    override fun onCreate(): Boolean = true
    override fun getType(uri: Uri): String = "image/png"

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        require(mode == "r" && uri.path == "/clipboard.png")
        val input = File(requireNotNull(context).filesDir, "clipboard.png").inputStream()
        val (read, write) = ParcelFileDescriptor.createPipe()
        thread {
            input.use { source ->
                ParcelFileDescriptor.AutoCloseOutputStream(write).use { source.copyTo(it) }
            }
        }
        return read
    }

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, args: Array<out String>?, sort: String?): Cursor? = null
    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    override fun delete(uri: Uri, selection: String?, args: Array<out String>?): Int = 0
    override fun update(uri: Uri, values: ContentValues?, selection: String?, args: Array<out String>?): Int = 0
}
