package org.umbriel.link.core.domain

data class Desktop(
    val id: String,
    val name: String,
    val connected: Boolean,
    /** Unix seconds. */
    val lastSeen: Long,
)
