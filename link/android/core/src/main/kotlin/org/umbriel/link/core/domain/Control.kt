package org.umbriel.link.core.domain

/** A command line the desktop lets this phone run there. */
data class DesktopCommand(val id: String, val name: String)

data class IncomingPing(val desktopName: String, val message: String?)

enum class PointerButton { Left, Right, Middle }

/** In the order of the core's InputKey, which the repository maps by position. */
enum class RemoteKey { Enter, Backspace, Delete, Escape, Tab, Space, Left, Right, Up, Down, Home, End, PageUp, PageDown, F5 }

/** The phone as the desktop's touchpad and keyboard; moves and scrolls are in the desktop's logical pixels. */
sealed interface RemoteInput {
    data class Move(val dx: Int, val dy: Int) : RemoteInput
    data class Scroll(val dx: Int, val dy: Int) : RemoteInput
    data class Click(val button: PointerButton) : RemoteInput
    data class Press(val button: PointerButton) : RemoteInput
    data class Release(val button: PointerButton) : RemoteInput
    data class Key(val key: RemoteKey) : RemoteInput
    data class Text(val text: String) : RemoteInput
}
