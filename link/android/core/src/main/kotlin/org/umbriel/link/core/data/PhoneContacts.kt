package org.umbriel.link.core.data

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.database.ContentObserver
import android.os.Handler
import android.os.Looper
import android.provider.ContactsContract.CommonDataKinds.Phone
import android.provider.ContactsContract.Contacts
import org.umbriel.link.ffi.PhoneContact

/**
 * The phone's contacts with a number, by name, for desktops whose messages switch is on. [contacts] is called off the
 * main thread; [changed] runs on it, at most once per [SETTLE_MS] of edits.
 */
class PhoneContacts(private val context: Context, private val changed: () -> Unit) {
    private val main = Handler(Looper.getMainLooper())
    private val notify = Runnable { changed() }
    private var watching = false

    fun contacts(offset: UInt, limit: UInt): List<PhoneContact>? {
        if (!granted()) return null
        val resolver = context.contentResolver
        val names = LinkedHashMap<Long, String>()
        resolver.query(
            Contacts.CONTENT_URI,
            arrayOf(Contacts._ID, Contacts.DISPLAY_NAME_PRIMARY),
            "${Contacts.HAS_PHONE_NUMBER}=1",
            null,
            "${Contacts.DISPLAY_NAME_PRIMARY} COLLATE NOCASE, ${Contacts._ID}",
        )?.use { cursor ->
            if (!cursor.moveToPosition(offset.toInt())) return@use
            do names[cursor.getLong(0)] = cursor.getString(1).orEmpty() while (names.size < limit.toInt() && cursor.moveToNext())
        }
        if (names.isEmpty()) return emptyList()
        val numbers = names.keys.associateWith { mutableListOf<String>() }
        resolver.query(
            Phone.CONTENT_URI,
            arrayOf(Phone.CONTACT_ID, Phone.NUMBER),
            "${Phone.CONTACT_ID} IN (${names.keys.joinToString(",")})",
            null,
            "${Phone.IS_SUPER_PRIMARY} DESC, ${Phone._ID}",
        )?.use { cursor ->
            while (cursor.moveToNext()) {
                val list = numbers[cursor.getLong(0)] ?: continue
                cursor.getString(1)?.takeIf { it !in list }?.let(list::add)
            }
        }
        return names.map { (id, name) -> PhoneContact(name, numbers.getValue(id)) }
    }

    /** Starts watching for edits once the permission is there, since the provider refuses observers before it. */
    fun watch() {
        if (watching || !granted()) return
        val observer = object : ContentObserver(main) {
            override fun onChange(selfChange: Boolean) {
                main.removeCallbacks(notify)
                main.postDelayed(notify, SETTLE_MS)
            }
        }
        context.contentResolver.registerContentObserver(Contacts.CONTENT_URI, true, observer)
        watching = true
        changed()
    }

    private fun granted() =
        context.checkSelfPermission(Manifest.permission.READ_CONTACTS) == PackageManager.PERMISSION_GRANTED

    private companion object {
        const val SETTLE_MS = 2_000L
    }
}
