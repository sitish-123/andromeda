package org.umbriel.link.core.domain

data class NotificationButton(val id: String, val label: String, val reply: Boolean)

/** A phone notification to mirror; the Rust core refuses one over the limits in `link/ARCHITECTURE.md`. */
class PhoneNotification(
    val id: String,
    val app: String,
    val title: String,
    val text: String,
    /** PNG. */
    val icon: ByteArray?,
    val actions: List<NotificationButton>,
)

/** What a desktop asks of a mirrored notification. */
sealed interface NotificationCommand {
    val desktopId: String
    val id: String

    data class Action(override val desktopId: String, override val id: String, val action: String, val replyText: String?) :
        NotificationCommand

    data class Dismiss(override val desktopId: String, override val id: String) : NotificationCommand
}

/** Limits from `link/ARCHITECTURE.md`, in UTF-8 bytes; the sender truncates to them. */
object NotificationLimits {
    const val APP = 128
    const val TITLE = 512
    const val TEXT = 4096
    const val ACTION = 64
    const val ACTIONS = 3
    const val ICON = 16 * 1024
}

/** The longest prefix of at most [maxBytes] UTF-8 bytes that ends on a character boundary. */
fun String.truncateUtf8(maxBytes: Int): String {
    if (toByteArray().size <= maxBytes) return this
    var bytes = 0
    var end = 0
    while (end < length) {
        val next = if (Character.isHighSurrogate(this[end]) && end + 1 < length) end + 2 else end + 1
        val size = substring(end, next).toByteArray().size
        if (bytes + size > maxBytes) break
        bytes += size
        end = next
    }
    return substring(0, end)
}
