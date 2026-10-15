package org.umbriel.link.core.data

import org.umbriel.link.ffi.PhoneMessages

/** Contacts and texts, for desktops whose messages switch is on. */
class Messaging(private val contacts: PhoneContacts, private val texts: PhoneTexts) : PhoneMessages {
    override fun contacts(offset: UInt, limit: UInt) = contacts.contacts(offset, limit)

    override fun texts(offset: UInt, limit: UInt) = texts.texts(offset, limit)

    override fun sendText(to: String, body: String) = texts.send(to, body)
}
