package org.umbriel.link.ui.theme

import androidx.compose.runtime.Immutable
import androidx.compose.ui.graphics.Color

/** Semantic colors: the only ones screens use, each with a light and a dark value. */
@Immutable
data class LinkColors(
    val dark: Boolean,
    val surfacePrimary: Color,
    val surfaceSecondary: Color,
    val surfaceTertiary: Color,
    val surfaceElevated: Color,
    val surfaceInput: Color,
    val surfaceOverlay: Color,
    val surfaceHighlight: Color,
    val textPrimary: Color,
    val textSecondary: Color,
    val textTertiary: Color,
    val textInverse: Color,
    val textDisabled: Color,
    val accent: Color,
    /** The accent as text, readable on the surface. */
    val accentText: Color,
    /** Text and glyphs on an accent fill. */
    val onAccent: Color,
    val borderPrimary: Color,
    val borderSecondary: Color,
    val divider: Color,
    val success: Color,
    val warning: Color,
    val error: Color,
    val info: Color,
)

private object Palette {
    val signal = Color(0xFF00D7FF)
    val signalOnPaper = Color(0xFF00789C)
    val ink = Color(0xFF0D1012)
    val paper = Color(0xFFF3F1EC)
}

val LightColors = LinkColors(
    dark = false,
    surfacePrimary = Palette.paper,
    surfaceSecondary = Palette.paper,
    surfaceTertiary = Color(0xFFEAE7E0),
    surfaceElevated = Color(0xFFFAF9F6),
    surfaceInput = Color(0xFFE8E5DE),
    surfaceOverlay = Color(0x99000000),
    surfaceHighlight = Color(0x0D000000),
    textPrimary = Color(0xFF15191B),
    textSecondary = Color(0xFF555B5E),
    textTertiary = Color(0xFF7C8184),
    textInverse = Palette.paper,
    textDisabled = Color(0xFFB4B3AE),
    accent = Palette.signalOnPaper,
    accentText = Palette.signalOnPaper,
    onAccent = Palette.paper,
    borderPrimary = Color(0xFFD5D1C8),
    borderSecondary = Color(0xFFE2DED6),
    divider = Color(0xFFD5D1C8),
    success = Color(0xFF2E7D4F),
    warning = Color(0xFFC77700),
    error = Color(0xFFC62828),
    info = Palette.signalOnPaper,
)

val DarkColors = LinkColors(
    dark = true,
    surfacePrimary = Palette.ink,
    surfaceSecondary = Palette.ink,
    surfaceTertiary = Color(0xFF161B1E),
    surfaceElevated = Color(0xFF161A1D),
    surfaceInput = Color(0xFF161A1D),
    surfaceOverlay = Color(0xB3000000),
    surfaceHighlight = Color(0x0FFFFFFF),
    textPrimary = Color(0xFFEDEFEF),
    textSecondary = Color(0xFFA3AAAD),
    textTertiary = Color(0xFF6E777B),
    textInverse = Palette.ink,
    textDisabled = Color(0xFF4A5154),
    accent = Palette.signal,
    accentText = Palette.signal,
    onAccent = Palette.ink,
    borderPrimary = Color(0xFF262D31),
    borderSecondary = Color(0xFF1E2427),
    divider = Color(0xFF262D31),
    success = Color(0xFF4CC38A),
    warning = Color(0xFFFFB13B),
    error = Color(0xFFFF6B6B),
    info = Palette.signal,
)
