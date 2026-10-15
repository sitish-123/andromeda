package org.umbriel.link.control

import android.view.KeyEvent as AndroidKeyEvent
import androidx.compose.foundation.background
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.nativeKeyCode
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.ImeAction
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.umbriel.link.R
import org.umbriel.link.core.domain.PointerButton
import org.umbriel.link.core.domain.RemoteInput
import org.umbriel.link.core.domain.RemoteKey
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.text
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Space

/** The desktop's touchpad: one finger moves and taps, two fingers scroll and tap to right-click; a field types. */
@Composable
fun RemoteScreen(viewModel: RemoteViewModel, desktopName: String, onBack: () -> Unit) {
    val failure by viewModel.failure.collectAsStateWithLifecycle()
    val resources = LocalContext.current.resources
    Screen(
        title = stringResource(R.string.remote_title),
        onBack = onBack,
        toast = failure?.text(resources),
        onToastShown = viewModel::failureShown,
    ) {
        Column(
            Modifier.fillMaxSize().padding(horizontal = Space.page).imePadding(),
            verticalArrangement = Arrangement.spacedBy(Space.s12),
        ) {
            Label(stringResource(R.string.remote_hint, desktopName), LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
            Touchpad(viewModel, Modifier.weight(1f).fillMaxWidth())
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(Space.s12)) {
                PillButton(
                    stringResource(R.string.remote_left), { viewModel.send(RemoteInput.Click(PointerButton.Left)) },
                    Modifier.weight(1f), PillKind.Tonal,
                )
                PillButton(
                    stringResource(R.string.remote_right), { viewModel.send(RemoteInput.Click(PointerButton.Right)) },
                    Modifier.weight(1f), PillKind.Tonal,
                )
            }
            Typing(viewModel)
            Row(
                Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(bottom = Space.s12),
                horizontalArrangement = Arrangement.spacedBy(Space.s8),
            ) {
                listOf(
                    "Esc" to RemoteKey.Escape, "Tab" to RemoteKey.Tab, "⌫" to RemoteKey.Backspace,
                    "←" to RemoteKey.Left, "↑" to RemoteKey.Up, "↓" to RemoteKey.Down, "→" to RemoteKey.Right,
                    "Home" to RemoteKey.Home, "End" to RemoteKey.End, "Del" to RemoteKey.Delete,
                ).forEach { (label, key) -> PillButton(label, { viewModel.key(key) }, kind = PillKind.Quiet) }
            }
        }
    }
}

@Composable
private fun Touchpad(viewModel: RemoteViewModel, modifier: Modifier) {
    Box(
        modifier.clip(Radius.list).background(LinkTheme.colors.surfaceTertiary).pointerInput(viewModel) {
            awaitEachGesture {
                val down = awaitFirstDown()
                var fingers = 1
                var travelled = 0f
                var lifted = down.uptimeMillis
                do {
                    val event = awaitPointerEvent()
                    val pressed = event.changes.filter { it.pressed }
                    fingers = maxOf(fingers, pressed.size)
                    if (pressed.isNotEmpty()) {
                        val delta = pressed.fold(Offset.Zero) { sum, change -> sum + change.positionChange() } / pressed.size.toFloat()
                        travelled += delta.getDistance()
                        if (pressed.size >= 2) viewModel.scroll(delta.x, delta.y) else if (fingers == 1) viewModel.move(delta.x, delta.y)
                    }
                    event.changes.forEach {
                        lifted = it.uptimeMillis
                        it.consume()
                    }
                } while (pressed.isNotEmpty())
                if (lifted - down.uptimeMillis < TAP_MS && travelled < viewConfiguration.touchSlop) {
                    viewModel.send(RemoteInput.Click(if (fingers >= 2) PointerButton.Right else PointerButton.Left))
                }
            }
        },
        contentAlignment = Alignment.Center,
    ) {
        Label(stringResource(R.string.remote_touchpad), LinkTheme.type.bodyMedium, LinkTheme.colors.textTertiary)
    }
}

/** Text goes as it is typed; a correction the keyboard makes goes as Backspace and the new text. */
@Composable
private fun Typing(viewModel: RemoteViewModel) {
    var text by rememberSaveable { mutableStateOf("") }
    BasicTextField(
        value = text,
        onValueChange = {
            viewModel.typed(text, it)
            text = it
        },
        modifier = Modifier.fillMaxWidth().clip(Radius.list).background(LinkTheme.colors.surfaceSecondary)
            .padding(Space.s12),
        textStyle = LinkTheme.type.bodyLarge.copy(color = LinkTheme.colors.textPrimary),
        cursorBrush = SolidColor(LinkTheme.colors.accent),
        singleLine = true,
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
        keyboardActions = KeyboardActions(onSend = {
            viewModel.key(RemoteKey.Enter)
            text = ""
        }),
        decorationBox = { field ->
            if (text.isEmpty()) Label(stringResource(R.string.remote_type), LinkTheme.type.bodyLarge, LinkTheme.colors.textTertiary)
            field()
        },
    )
}

/** Slides: next and previous, also on the volume keys, start and end; the screen stays on. */
@Composable
fun PresenterScreen(viewModel: RemoteViewModel, desktopName: String, onBack: () -> Unit) {
    val failure by viewModel.failure.collectAsStateWithLifecycle()
    val resources = LocalContext.current.resources
    val view = LocalView.current
    DisposableEffect(view) {
        view.keepScreenOn = true
        onDispose { view.keepScreenOn = false }
    }
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    Screen(
        title = stringResource(R.string.presenter_title),
        onBack = onBack,
        toast = failure?.text(resources),
        onToastShown = viewModel::failureShown,
    ) {
        Column(
            Modifier.fillMaxSize().padding(horizontal = Space.page).focusRequester(focus).focusable()
                .onPreviewKeyEvent { event ->
                    val key = when (event.key.nativeKeyCode) {
                        AndroidKeyEvent.KEYCODE_VOLUME_DOWN -> RemoteKey.PageDown
                        AndroidKeyEvent.KEYCODE_VOLUME_UP -> RemoteKey.PageUp
                        else -> return@onPreviewKeyEvent false
                    }
                    if (event.type == KeyEventType.KeyDown) viewModel.key(key)
                    true
                },
            verticalArrangement = Arrangement.spacedBy(Space.s12),
        ) {
            Label(stringResource(R.string.presenter_hint, desktopName), LinkTheme.type.bodyMedium, LinkTheme.colors.textSecondary)
            PillButton(
                stringResource(R.string.presenter_next), { viewModel.key(RemoteKey.PageDown) },
                Modifier.fillMaxWidth().weight(1f),
            )
            PillButton(
                stringResource(R.string.presenter_previous), { viewModel.key(RemoteKey.PageUp) },
                Modifier.fillMaxWidth(), PillKind.Tonal,
            )
            Row(Modifier.fillMaxWidth().padding(bottom = Space.s24), horizontalArrangement = Arrangement.spacedBy(Space.s12)) {
                PillButton(
                    stringResource(R.string.presenter_start), { viewModel.key(RemoteKey.F5) }, Modifier.weight(1f), PillKind.Quiet,
                )
                PillButton(
                    stringResource(R.string.presenter_end), { viewModel.key(RemoteKey.Escape) }, Modifier.weight(1f), PillKind.Quiet,
                )
            }
        }
    }
}

private const val TAP_MS = 250L
