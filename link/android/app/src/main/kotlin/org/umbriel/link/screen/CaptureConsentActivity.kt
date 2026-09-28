package org.umbriel.link.screen

import android.app.Activity
import android.media.projection.MediaProjectionConfig
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import org.umbriel.link.LinkApplication

/** Shows Android's screen capture prompt and hands its answer to [ScreenCapture]; nothing of its own on screen. */
class CaptureConsentActivity : ComponentActivity() {
    private val consent = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        val desktop = intent.getStringExtra(ScreenMirror.EXTRA_DESKTOP)
        val data = result.data
        when {
            desktop == null -> Unit
            result.resultCode == Activity.RESULT_OK && data != null -> ScreenCapture.start(this, desktop, result.resultCode, data)
            else -> (application as LinkApplication).container.screen.refuse(desktop, "declined on the phone")
        }
        finish()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (savedInstanceState == null) {
            val projections = getSystemService(MediaProjectionManager::class.java)
            // The desktop mirrors the whole screen; asking for it spares the user Android's "a single app" choice.
            val prompt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                projections.createScreenCaptureIntent(MediaProjectionConfig.createConfigForDefaultDisplay())
            } else {
                projections.createScreenCaptureIntent()
            }
            consent.launch(prompt)
        }
    }
}
