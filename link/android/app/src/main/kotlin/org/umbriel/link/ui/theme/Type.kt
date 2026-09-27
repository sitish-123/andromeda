package org.umbriel.link.ui.theme

import androidx.compose.runtime.Immutable
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import org.umbriel.link.R

/** Source Sans 3 (SIL OFL 1.1, bundled with its license in assets/licenses). */
val SourceSans = FontFamily(
    Font(R.font.source_sans_3_regular, FontWeight.Normal),
    Font(R.font.source_sans_3_medium, FontWeight.Medium),
    Font(R.font.source_sans_3_semibold, FontWeight.SemiBold),
    Font(R.font.source_sans_3_bold, FontWeight.Bold),
    Font(R.font.source_sans_3_bold, FontWeight.ExtraBold),
)

/** The 15 type roles: a small phone scale where body text is 14 sp. */
@Immutable
data class LinkType(
    val displayLarge: TextStyle = role(32, FontWeight.ExtraBold, 1.10f, -1.0f),
    val displayMedium: TextStyle = role(28, FontWeight.Bold, 1.15f, -0.5f),
    val displaySmall: TextStyle = role(24, FontWeight.Bold, 1.20f, -0.5f),
    val headlineLarge: TextStyle = role(20, FontWeight.Bold, 1.20f, -0.2f),
    val headlineMedium: TextStyle = role(18, FontWeight.Bold, 1.25f, -0.2f),
    val headlineSmall: TextStyle = role(16, FontWeight.SemiBold, 1.30f, 0f),
    val titleLarge: TextStyle = role(16, FontWeight.SemiBold, 1.30f, 0f),
    val titleMedium: TextStyle = role(14, FontWeight.SemiBold, 1.35f, 0f),
    val titleSmall: TextStyle = role(13, FontWeight.SemiBold, 1.35f, 0f),
    val bodyLarge: TextStyle = role(14, FontWeight.Normal, 1.50f, 0f),
    val bodyMedium: TextStyle = role(13, FontWeight.Normal, 1.45f, 0f),
    val bodySmall: TextStyle = role(12, FontWeight.Normal, 1.40f, 0f),
    val labelLarge: TextStyle = role(12, FontWeight.SemiBold, 1.30f, 0.2f),
    val labelMedium: TextStyle = role(11, FontWeight.SemiBold, 1.30f, 0.3f),
    val labelSmall: TextStyle = role(10, FontWeight.Medium, 1.30f, 0.4f),
)

/** `tracking` is in px at the role's size, as the tokens give it; Compose wants em. */
private fun role(size: Int, weight: FontWeight, height: Float, tracking: Float) = TextStyle(
    fontFamily = SourceSans,
    fontWeight = weight,
    fontSize = size.sp,
    lineHeight = (size * height).sp,
    letterSpacing = (tracking / size).em,
)
