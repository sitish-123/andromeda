package org.umbriel.link.core.domain

data class OfferedFile(val name: String, val size: Long)

/** A received file, published in Downloads. */
data class SavedFile(val name: String, val uri: String)

/** File transfers in both directions, as the core reports them. */
sealed interface TransferEvent {
    val transferId: String

    /** A desktop offers files; answer with accept or decline within 120 s. */
    data class Offered(
        override val transferId: String,
        val desktopName: String,
        val files: List<OfferedFile>,
    ) : TransferEvent

    data class Progress(override val transferId: String, val bytes: Long, val total: Long) : TransferEvent

    /** [status] is done, failed, declined, no-space, too-large, busy, or cancelled. */
    data class Finished(
        override val transferId: String,
        val desktopName: String,
        val incoming: Boolean,
        val status: String,
        val saved: List<SavedFile>,
    ) : TransferEvent
}
