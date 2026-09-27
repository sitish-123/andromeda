package org.umbriel.link.transfer

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.ui.res.stringResource
import androidx.core.app.NotificationManagerCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.yield
import org.umbriel.link.LinkApplication
import org.umbriel.link.R
import org.umbriel.link.notifications.TransferNotifier
import org.umbriel.link.ui.LinkTheme

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
                AlertDialog(
                    onDismissRequest = ::finish,
                    title = { Text(title) },
                    confirmButton = { TextButton(onClick = { answer(true) }) { Text(stringResource(R.string.transfer_accept)) } },
                    dismissButton = { TextButton(onClick = { answer(false) }) { Text(stringResource(R.string.transfer_decline)) } },
                )
            }
        }
    }

    companion object {
        const val EXTRA_TRANSFER = "transfer"
        const val EXTRA_TITLE = "title"
    }
}
