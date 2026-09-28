package org.umbriel.link.core.domain

enum class CallState { Ringing, Active, Idle }

enum class CallAction { Mute, Decline }

object CallLimits {
    const val NUMBER = 64
    const val NAME = 128
}
