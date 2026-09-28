package org.umbriel.link.core.domain

data class Desktop(
    val id: String,
    val name: String,
    val connected: Boolean,
    /** Connected over Bluetooth, since no Wi-Fi path answered. */
    val bluetooth: Boolean = false,
    /** Unix seconds. */
    val lastSeen: Long,
    val sharing: Sharing = Sharing(),
)

/** This phone's switches for one desktop; all but browsing on after pairing. */
data class Sharing(
    val clipboard: Boolean = true,
    val files: Boolean = true,
    val notifications: Boolean = true,
    val media: Boolean = true,
    val ring: Boolean = true,
    val calls: Boolean = true,
    val browse: Boolean = false,
) {
    fun allows(feature: Feature): Boolean = when (feature) {
        Feature.Clipboard -> clipboard
        Feature.Files -> files
        Feature.Notifications -> notifications
        Feature.Media -> media
        Feature.Ring -> ring
        Feature.Calls -> calls
        Feature.Browse -> browse
    }
}

enum class Feature { Clipboard, Files, Notifications, Media, Ring, Calls, Browse }
