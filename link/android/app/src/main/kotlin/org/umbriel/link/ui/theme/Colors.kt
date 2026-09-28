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
    /** Light cards carry a 2 dp white rim; dark ones separate by surface alone. */
    val cardRim: Color,
    /** Every shadow is this color; only blur and offset differ. */
    val shadow: Color,
)

private object Palette {
    val electricBlue = Color(0xFF00D7FF)
    val accessibleBlue = Color(0xFF0088BB)
    val brightenedBlue = Color(0xFF22B8DA)
    val charcoal = Color(0xFF323232)
    val stone = Color(0xFFF0F0F0)
    val darkSurface = Color(0xFF0A0A0A)
    val white = Color(0xFFFFFFFF)
}

val LightColors = LinkColors(
    dark = false,
    surfacePrimary = Palette.white,
    surfaceSecondary = Palette.white,
    surfaceTertiary = Color(0xFFF5F5F5),
    surfaceElevated = Palette.white,
    surfaceInput = Color(0xFFF5F5F5),
    surfaceOverlay = Color(0x80000000),
    surfaceHighlight = Color(0x0A000000),
    textPrimary = Palette.charcoal,
    textSecondary = Color(0xFF616161),
    textTertiary = Color(0xFF737373),
    textInverse = Palette.white,
    textDisabled = Color(0xFFBDBDBD),
    accent = Palette.electricBlue,
    accentText = Palette.accessibleBlue,
    onAccent = Palette.charcoal,
    borderPrimary = Color(0xFFE0E0E0),
    borderSecondary = Palette.stone,
    divider = Color(0xFFE0E0E0),
    success = Color(0xFF43A047),
    warning = Color(0xFFFF9800),
    error = Color(0xFFE53935),
    info = Color(0xFF2196F3),
    cardRim = Palette.white,
    shadow = Color(0x1A000000),
)

val DarkColors = LinkColors(
    dark = true,
    surfacePrimary = Palette.darkSurface,
    surfaceSecondary = Color(0xFF121212),
    surfaceTertiary = Color(0xFF2A2A2A),
    surfaceElevated = Color(0xFF1E1E1E),
    surfaceInput = Color(0xFF1E1E1E),
    surfaceOverlay = Color(0xB3000000),
    surfaceHighlight = Color(0x0AFFFFFF),
    textPrimary = Palette.stone,
    textSecondary = Color(0xFFBDBDBD),
    textTertiary = Color(0xFFB0B0B0),
    textInverse = Palette.charcoal,
    textDisabled = Color(0xFF757575),
    accent = Palette.electricBlue,
    accentText = Palette.brightenedBlue,
    onAccent = Palette.charcoal,
    borderPrimary = Color(0xFF333333),
    borderSecondary = Color(0xFF2A2A2A),
    divider = Color(0xFF333333),
    success = Color(0xFF43A047),
    warning = Color(0xFFFF9800),
    error = Color(0xFFE53935),
    info = Color(0xFF2196F3),
    cardRim = Color.Transparent,
    shadow = Color(0x66000000),
)
