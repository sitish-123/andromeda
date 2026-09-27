package org.umbriel.link.core.domain

/** A desktop's clipboard: [text] is inline and set at once; other types are pulled when an app reads them. */
data class IncomingClip(val desktopId: String, val clipId: Long, val mimes: List<String>, val text: String?)
