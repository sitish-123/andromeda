package org.umbriel.link.ui.theme

import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.unit.dp

/** The 8 dp grid, with 2, 4, 6, 10, and 14 for tight places. */
object Space {
    val s2 = 2.dp
    val s4 = 4.dp
    val s6 = 6.dp
    val s8 = 8.dp
    val s10 = 10.dp
    val s12 = 12.dp
    val s14 = 14.dp
    val s16 = 16.dp
    val s20 = 20.dp
    val s24 = 24.dp
    val s28 = 28.dp
    val s32 = 32.dp
    val s40 = 40.dp
    val s48 = 48.dp
    val page = 20.dp
    val section = 16.dp
    val block = 12.dp
}

object Radius {
    val micro = RoundedCornerShape(2.dp)
    val small = RoundedCornerShape(8.dp)
    val button = RoundedCornerShape(12.dp)
    val list = RoundedCornerShape(16.dp)
    val hero = RoundedCornerShape(20.dp)
    val softTech = RoundedCornerShape(28.dp)
    val pill = RoundedCornerShape(100.dp)
}

object Size {
    val touch = 48.dp
    val touchLarge = 56.dp
    val iconSmall = 16.dp
    val icon = 24.dp
    val iconLarge = 32.dp
    val iconContainer = 40.dp
    val orb = 56.dp
    val pill = 48.dp
    val chip = 32.dp
}

/** Durations in ms and curves the design system uses; nothing slower than 500 ms. */
object Motion {
    const val BUTTON_PRESS = 100
    const val FADE_OUT = 150
    const val FAST = 200
    const val MEDIUM_FAST = 250
    const val NORMAL = 320
    const val MORPH = 380
    const val MEDIUM = 400
    const val SLOW = 500
    const val STAGGER_ITEM = 50
    val standard = CubicBezierEasing(0.645f, 0.045f, 0.355f, 1f)
    val enter = CubicBezierEasing(0.215f, 0.61f, 0.355f, 1f)
    val exit = CubicBezierEasing(0.55f, 0.055f, 0.675f, 0.19f)
    val smoothEnter = CubicBezierEasing(0.165f, 0.84f, 0.44f, 1f)
    val spring = CubicBezierEasing(0.34f, 1.56f, 0.64f, 1f)
}
