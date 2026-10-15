package org.umbriel.link.control

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlin.math.roundToInt
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import org.umbriel.link.core.data.LinkRepository
import org.umbriel.link.core.domain.LinkFailure
import org.umbriel.link.core.domain.RemoteInput
import org.umbriel.link.core.domain.RemoteKey
import org.umbriel.link.core.domain.linkFailure

/**
 * The phone as one desktop's touchpad and keyboard. Moves and scrolls add up and go once a frame, so a fast finger
 * stays under the desktop's input rate; everything goes out in the order it happened.
 */
class RemoteViewModel(private val repository: LinkRepository, private val desktopId: String) : ViewModel() {
    private val outbox = Channel<RemoteInput>(Channel.UNLIMITED)
    private val wake = Channel<Unit>(Channel.CONFLATED)
    private var moveX = 0f
    private var moveY = 0f
    private var scrollX = 0f
    private var scrollY = 0f
    private val _failure = MutableStateFlow<LinkFailure?>(null)
    val failure: StateFlow<LinkFailure?> = _failure

    init {
        viewModelScope.launch {
            for (input in outbox) {
                repository.sendInput(desktopId, input).exceptionOrNull()?.let { _failure.value = it.linkFailure() }
            }
        }
        viewModelScope.launch {
            for (ignored in wake) {
                delay(FRAME_MS)
                flush()
            }
        }
    }

    /** By (dx, dy) of the phone's pixels. */
    fun move(dx: Float, dy: Float) {
        moveX += dx * POINTER_SPEED
        moveY += dy * POINTER_SPEED
        wake.trySend(Unit)
    }

    /** Fingers moved by (dx, dy) of the phone's pixels; the content follows them. */
    fun scroll(dx: Float, dy: Float) {
        scrollX -= dx
        scrollY -= dy
        wake.trySend(Unit)
    }

    fun send(input: RemoteInput) {
        flush()
        outbox.trySend(input)
    }

    fun key(key: RemoteKey) = send(RemoteInput.Key(key))

    /** What changed in the typing field: removed characters as Backspace, then the added text. */
    fun typed(before: String, after: String) {
        val kept = before.commonPrefixWith(after).length
        repeat(before.length - kept) { key(RemoteKey.Backspace) }
        if (after.length > kept) send(RemoteInput.Text(after.substring(kept)))
    }

    fun failureShown() {
        _failure.value = null
    }

    private fun flush() {
        val (mx, my) = moveX.roundToInt() to moveY.roundToInt()
        if (mx != 0 || my != 0) {
            outbox.trySend(RemoteInput.Move(mx, my))
            moveX -= mx
            moveY -= my
        }
        val (sx, sy) = scrollX.roundToInt() to scrollY.roundToInt()
        if (sx != 0 || sy != 0) {
            outbox.trySend(RemoteInput.Scroll(sx, sy))
            scrollX -= sx
            scrollY -= sy
        }
    }

    private companion object {
        const val FRAME_MS = 16L
        const val POINTER_SPEED = 1.5f
    }
}
