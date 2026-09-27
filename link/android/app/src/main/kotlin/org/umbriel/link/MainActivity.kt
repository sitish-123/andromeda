package org.umbriel.link

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import kotlinx.coroutines.flow.MutableStateFlow
import org.umbriel.link.ui.LinkApp
import org.umbriel.link.ui.LinkTheme

class MainActivity : ComponentActivity() {
    /** A pairing URI opened from the desktop's QR code, waiting for the user to confirm it. */
    private val pairingLink = MutableStateFlow<String?>(null)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        pairingLink.value = intent.pairingUri()
        val container = (application as LinkApplication).container
        setContent {
            LinkTheme {
                LinkApp(container, pairingLink, onLinkHandled = { pairingLink.value = null })
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        intent.pairingUri()?.let { pairingLink.value = it }
    }
}

private fun Intent.pairingUri(): String? =
    dataString?.takeIf { action == Intent.ACTION_VIEW && it.startsWith("umbriel-link:pair?") }
