package org.umbriel.link.ui.components

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.unit.dp

object LinkIcons {
    val Computer = glyph("Desktop", "M3 3H21V17H13V19H17V21H7V19H11V17H3ZM5 5V15H19V5Z")
    val Folder = glyph("Folder", "M10 4H4c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2h-8l-2-2z")
    val Transfers = glyph("Transfers", "M7 2L2 7H6V19H8V7H12ZM17 22L22 17H18V5H16V17H12Z")
    val Download = glyph("Receive", "M11 3H13V13L17 9L18.5 10.5L12 17L5.5 10.5L7 9L11 13ZM4 19H20V21H4Z")
    val Clipboard = glyph("Clipboard", "M9 2H15V4H18C19.1 4 20 4.9 20 6V20C20 21.1 19.1 22 18 22H6C4.9 22 4 21.1 4 20V6C4 4.9 4.9 4 6 4H9ZM6 6V20H18V6H16V8H8V6Z")
    val Ring = glyph("Ring", "M3 9V15H7L12 20V4L7 9ZM16.5 12A4.5 4.5 0 0 0 14 8V16A4.5 4.5 0 0 0 16.5 12ZM14 3.2V5.3A7 7 0 0 1 14 18.7V20.8A9 9 0 0 0 14 3.2Z")
    val Upload = glyph("Send", "M11 17H13V7L17 11L18.5 9.5L12 3L5.5 9.5L7 11L11 7ZM4 19H20V21H4Z")
}

private fun glyph(name: String, path: String) = ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f)
    .addPath(PathParser().parsePathString(path).toNodes(), fill = SolidColor(Color.Black))
    .build()
