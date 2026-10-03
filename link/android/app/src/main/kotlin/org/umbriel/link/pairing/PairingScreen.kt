package org.umbriel.link.pairing

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import android.Manifest
import android.content.pm.PackageManager
import org.umbriel.link.ui.components.PillKind
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.umbriel.link.R
import org.umbriel.link.core.domain.Desktop
import org.umbriel.link.ui.components.CenteredText
import org.umbriel.link.ui.components.ConnectionOrb
import org.umbriel.link.ui.components.HeroSurface
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.Screen
import org.umbriel.link.ui.text
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Motion
import org.umbriel.link.ui.theme.Radius
import org.umbriel.link.ui.theme.Space

/**
 * Pairing: the camera first, reading the desktop's QR code and pairing at once; the 6-digit code one tap away; and a
 * QR link opened from outside the app shown for confirmation, since nobody here chose to scan it.
 */
@Composable
fun PairingScreen(viewModel: PairingViewModel, onBack: () -> Unit, onPaired: (Desktop) -> Unit) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    var typing by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(viewModel) { viewModel.paired.collect(onPaired) }
    Screen(title = stringResource(R.string.pair_title), onBack = onBack) {
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Space.page),
            verticalArrangement = Arrangement.spacedBy(Space.s20),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            when {
                state.busy -> {
                    ConnectionOrb(active = true, working = true, size = 140.dp, modifier = Modifier.padding(top = Space.s32))
                    Label(stringResource(R.string.pairing_progress), LinkTheme.type.headlineLarge, LinkTheme.colors.accentText)
                }
                state.link != null -> LinkConfirmation(state, viewModel)
                typing -> {
                    CodeEntry(state, viewModel)
                    PillButton(stringResource(R.string.scan_action), { typing = false }, kind = PillKind.Quiet)
                }
                else -> {
                    // A failed attempt re-arms the scanner, which reports one code per composition.
                    key(state.failure) { Scan(viewModel::pairScanned) }
                    PillButton(stringResource(R.string.scan_code_instead), { typing = true }, kind = PillKind.Quiet)
                }
            }
            state.failure?.let {
                Label(
                    it.text(LocalContext.current.resources),
                    LinkTheme.type.bodyLarge,
                    LinkTheme.colors.error,
                    Modifier.fillMaxWidth(),
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}

@Composable
private fun Scan(onPairingUri: (String) -> Unit) {
    val context = LocalContext.current
    val colors = LinkTheme.colors
    var granted by remember {
        mutableStateOf(ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED)
    }
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted = it }
    LaunchedEffect(Unit) { if (!granted) ask.launch(Manifest.permission.CAMERA) }
    Box(
        Modifier.padding(top = Space.s8).fillMaxWidth().aspectRatio(1f).clip(Radius.hero).border(1.dp, colors.borderPrimary, Radius.hero),
        contentAlignment = Alignment.Center,
    ) {
        if (granted) {
            QrScanner(onPairingUri, Modifier.fillMaxSize())
        } else {
            Column(Modifier.padding(Space.s24), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(Space.s16)) {
                Label(stringResource(R.string.scan_camera_needed), LinkTheme.type.bodyLarge, colors.textSecondary, textAlign = TextAlign.Center)
                PillButton(stringResource(R.string.scan_camera_allow), { ask.launch(Manifest.permission.CAMERA) })
            }
        }
    }
    Label(stringResource(R.string.scan_hint), LinkTheme.type.bodyLarge, colors.textSecondary, Modifier.fillMaxWidth(), textAlign = TextAlign.Center)
}

@Composable
private fun LinkConfirmation(state: PairingState, viewModel: PairingViewModel) {
    HeroSurface(Modifier.fillMaxWidth()) {
        Label(stringResource(R.string.pair_link_title), LinkTheme.type.displaySmall)
        Label(
            stringResource(R.string.pair_link_body),
            LinkTheme.type.bodyLarge,
            LinkTheme.colors.textSecondary,
            Modifier.padding(top = Space.s8),
        )
        PillButton(
            stringResource(R.string.pair_button),
            viewModel::pairWithLink,
            enabled = !state.busy,
            modifier = Modifier.fillMaxWidth().padding(top = Space.s20),
        )
    }
}

@Composable
private fun CodeEntry(state: PairingState, viewModel: PairingViewModel) {
    CenteredText(stringResource(R.string.pair_code_title), stringResource(R.string.pair_instructions))
    CodeCells(state.code, viewModel::updateCode, onDone = viewModel::pairWithCode)
    PillButton(
        stringResource(R.string.pair_button),
        viewModel::pairWithCode,
        enabled = state.canPairCode,
        modifier = Modifier.fillMaxWidth(),
    )
}

/**
 * The code as six pill cells over one text field, which keeps the keyboard, paste, and IME behaviour of a real field.
 */
@Composable
private fun CodeCells(code: String, onChange: (String) -> Unit, onDone: () -> Unit) {
    val colors = LinkTheme.colors
    val description = stringResource(R.string.pair_code_label)
    BasicTextField(
        value = code,
        onValueChange = onChange,
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword, imeAction = ImeAction.Done),
        keyboardActions = KeyboardActions(onDone = { onDone() }),
        modifier = Modifier.fillMaxWidth().semantics { contentDescription = description },
        decorationBox = { field ->
            Box {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(Space.s8)) {
                    repeat(CODE_LENGTH) { index ->
                        val filled = index < code.length
                        val current = index == code.length
                        val border by animateColorAsState(
                            if (current) colors.accent else if (filled) colors.textPrimary else colors.borderPrimary,
                            tween(Motion.FAST),
                            label = "cell",
                        )
                        Box(
                            Modifier.weight(1f).aspectRatio(0.72f).clip(Radius.list)
                                .border(if (current) 2.dp else 1.dp, border, Radius.list),
                            contentAlignment = Alignment.Center,
                        ) {
                            Label(code.getOrNull(index)?.toString().orEmpty(), LinkTheme.type.displayMedium)
                        }
                    }
                }
                // The field stays in the tree for focus, input, and the caret's position, drawn invisible.
                Box(Modifier.matchParentSize().alpha(0f)) { field() }
            }
        },
    )
}
