package org.umbriel.link.core.domain

/** [uri] is where a sent file was read from, so the history can open it again; null for received offers. */
data class OfferedFile(val name: String, val size: Long, val uri: String? = null)

/** A received file, published in Downloads. */
data class SavedFile(val name: String, val uri: String)

/** One transfer in the history; [time] is when it was first seen, in Unix milliseconds. */
data class TransferRecord(
    val transferId: String,
    val desktopName: String = "",
    val incoming: Boolean? = null,
    val files: List<OfferedFile> = emptyList(),
    val bytes: Long = 0,
    val total: Long = 0,
    val status: String = "transferring",
    val saved: List<SavedFile> = emptyList(),
    val time: Long = System.currentTimeMillis(),
)

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
