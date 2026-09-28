package org.umbriel.link.transfer

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.core.app.NotificationManagerCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.yield
import org.umbriel.link.LinkApplication
import org.umbriel.link.R
import org.umbriel.link.notifications.TransferNotifier
import org.umbriel.link.ui.components.Label
import org.umbriel.link.ui.components.PillButton
import org.umbriel.link.ui.components.PillKind
import org.umbriel.link.ui.components.SoftCard
import org.umbriel.link.ui.theme.LinkTheme
import org.umbriel.link.ui.theme.Space

/** A desktop's offer, opened from its notification: the same Accept and Decline, for when Android hides the buttons. */
class OfferActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val transferId = intent.getStringExtra(EXTRA_TRANSFER) ?: return finish()
        val title = intent.getStringExtra(EXTRA_TITLE).orEmpty()
        val repository = (application as LinkApplication).container.repository
        val answer = { accept: Boolean ->
            NotificationManagerCompat.from(this).cancel(TransferNotifier.TAG, transferId.hashCode())
            // Finished only after the answer, so the transfer service starts while this is still in the foreground.
            lifecycleScope.launch {
                if (accept) repository.acceptTransfer(transferId) else repository.declineTransfer(transferId)
                yield()
                finish()
            }
        }
        setContent {
            LinkTheme {
                Box(
                    Modifier.fillMaxSize().background(LinkTheme.colors.surfaceOverlay)
                        .clickable(remember { MutableInteractionSource() }, indication = null, onClick = ::finish),
                    contentAlignment = Alignment.BottomCenter,
                ) {
                    val card = remember { MutableInteractionSource() }
                    SoftCard(
                        Modifier.fillMaxWidth().navigationBarsPadding().padding(Space.s12).clickable(card, indication = null) {},
                    ) {
                        Label(title, LinkTheme.type.headlineMedium)
                        Column(Modifier.padding(top = Space.s16), verticalArrangement = Arrangement.spacedBy(Space.s8)) {
                            PillButton(stringResource(R.string.transfer_accept), { answer(true) }, Modifier.fillMaxWidth())
                            PillButton(
                                stringResource(R.string.transfer_decline),
                                { answer(false) },
                                Modifier.fillMaxWidth(),
                                PillKind.Quiet,
                            )
                        }
                    }
                }
            }
        }
    }

    companion object {
        const val EXTRA_TRANSFER = "transfer"
        const val EXTRA_TITLE = "title"
    }
}
