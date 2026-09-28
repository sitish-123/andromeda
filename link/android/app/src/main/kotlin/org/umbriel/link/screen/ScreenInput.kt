package org.umbriel.link.screen

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.content.Intent
import android.graphics.Path
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.WindowManager
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import org.umbriel.link.core.domain.MIRROR_UNIT
import org.umbriel.link.core.domain.MirrorAction
import org.umbriel.link.core.domain.MirrorCommand

/**
 * The desktop's input on the mirrored screen: taps, long presses, swipes, and scrolls as gestures, Back, Home, and
 * Recents as global actions, and text into the focused field. An accessibility service is the only way a sideloaded
 * app can inject input; the user turns it on once in Accessibility settings.
 */
class ScreenInput : AccessibilityService() {
    override fun onServiceConnected() {
        instance = this
    }

    override fun onUnbind(intent: Intent?): Boolean {
        instance = null
        return super.onUnbind(intent)
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) = Unit

    override fun onInterrupt() = Unit

    private fun run(input: MirrorCommand.Input) {
        val (width, height) = screen()
        fun px(value: Int?, side: Int) = (value ?: 0).toFloat() / MIRROR_UNIT * side
        val x = px(input.x, width)
        val y = px(input.y, height)
        when (input.action) {
            MirrorAction.Tap -> gesture(line(x, y, x, y), TAP_MS)
            MirrorAction.LongPress -> gesture(line(x, y, x, y), input.ms?.toLong() ?: LONG_PRESS_MS)
            MirrorAction.Swipe -> gesture(line(x, y, px(input.x2, width), px(input.y2, height)), input.ms?.toLong() ?: SWIPE_MS)
            MirrorAction.Scroll -> {
                val toX = (x + px(input.x2, width)).coerceIn(0f, width - 1f)
                val toY = (y + px(input.y2, height)).coerceIn(0f, height - 1f)
                gesture(line(x, y, toX, toY), SCROLL_MS)
            }
            MirrorAction.Back -> performGlobalAction(GLOBAL_ACTION_BACK)
            MirrorAction.Home -> performGlobalAction(GLOBAL_ACTION_HOME)
            MirrorAction.Recents -> performGlobalAction(GLOBAL_ACTION_RECENTS)
            MirrorAction.Text -> type(input.text.orEmpty())
        }
    }

    private fun screen(): Pair<Int, Int> = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
        getSystemService(WindowManager::class.java).maximumWindowMetrics.bounds.let { it.width() to it.height() }
    } else {
        resources.displayMetrics.let { it.widthPixels to it.heightPixels }
    }

    private fun line(x: Float, y: Float, toX: Float, toY: Float) = Path().apply {
        moveTo(x, y)
        lineTo(toX, toY)
    }

    private fun gesture(path: Path, ms: Long) {
        val stroke = GestureDescription.StrokeDescription(path, 0, ms.coerceIn(1, MAX_GESTURE_MS))
        if (!dispatchGesture(GestureDescription.Builder().addStroke(stroke).build(), null, null)) {
            Log.i(TAG, "the gesture was not dispatched")
        }
    }

    /** Appends to the focused field; a backspace (U+0008) removes its last character, a newline presses Enter. */
    private fun type(text: String) {
        val field = rootInActiveWindow?.findFocus(AccessibilityNodeInfo.FOCUS_INPUT) ?: return
        if (text == "\n" && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            field.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_IME_ENTER.id)
            return
        }
        val showsHint = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && field.isShowingHintText
        val current = if (showsHint) "" else field.text?.toString().orEmpty()
        val next = if (text == BACKSPACE) current.dropLast(1) else current + text
        val arguments = Bundle().apply {
            putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, next)
        }
        field.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, arguments)
    }

    companion object {
        @Volatile
        private var instance: ScreenInput? = null
        private val main = Handler(Looper.getMainLooper())

        /** Whether the user turned the service on. */
        val enabled: Boolean get() = instance != null

        fun perform(input: MirrorCommand.Input) {
            val service = instance
            if (service == null) {
                Log.i(TAG, "input dropped: the accessibility service is off")
                return
            }
            main.post { service.run(input) }
        }

        private const val TAG = "ScreenInput"
        private const val BACKSPACE = "\u0008"
        private const val TAP_MS = 50L
        private const val LONG_PRESS_MS = 700L
        private const val SWIPE_MS = 300L
        private const val SCROLL_MS = 150L
        private const val MAX_GESTURE_MS = 10_000L
    }
}
