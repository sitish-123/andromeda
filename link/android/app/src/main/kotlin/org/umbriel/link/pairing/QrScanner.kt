package org.umbriel.link.pairing

import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.google.zxing.BarcodeFormat
import com.google.zxing.BinaryBitmap
import com.google.zxing.DecodeHintType
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.ReaderException
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import org.umbriel.link.ui.theme.LinkTheme

/** The back camera with corner ticks; [onPairingUri] fires once, for the first QR code that is a pairing URI. */
@Composable
fun QrScanner(onPairingUri: (String) -> Unit, modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val lifecycle = LocalLifecycleOwner.current
    val callback = rememberUpdatedState(onPairingUri)
    val view = remember { PreviewView(context).apply {
        scaleType = PreviewView.ScaleType.FILL_CENTER
        // A TextureView, which follows the rounded clip; a SurfaceView would punch through it.
        implementationMode = PreviewView.ImplementationMode.COMPATIBLE
    } }
    DisposableEffect(lifecycle) {
        val executor = Executors.newSingleThreadExecutor()
        val found = AtomicBoolean(false)
        val future = ProcessCameraProvider.getInstance(context)
        future.addListener({
            val provider = future.get()
            val preview = Preview.Builder().build().also { it.surfaceProvider = view.surfaceProvider }
            val analysis = ImageAnalysis.Builder().setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST).build()
            analysis.setAnalyzer(executor) { image ->
                val uri = image.use(::decode)?.takeIf { it.startsWith(PAIRING_PREFIX) }
                if (uri != null && found.compareAndSet(false, true)) {
                    ContextCompat.getMainExecutor(context).execute { callback.value(uri) }
                }
            }
            provider.unbindAll()
            provider.bindToLifecycle(lifecycle, CameraSelector.DEFAULT_BACK_CAMERA, preview, analysis)
        }, ContextCompat.getMainExecutor(context))
        onDispose {
            if (future.isDone) future.get().unbindAll()
            executor.shutdown()
        }
    }
    val tick = LinkTheme.colors.accent
    Box(modifier) {
        AndroidView({ view }, Modifier.fillMaxSize())
        Canvas(Modifier.fillMaxSize()) {
            val inset = size.minDimension * 0.18f
            val arm = size.minDimension * 0.10f
            val stroke = 3.dp.toPx()
            for (dx in listOf(1f, -1f)) for (dy in listOf(1f, -1f)) {
                val corner = Offset(if (dx > 0) inset else size.width - inset, if (dy > 0) inset else size.height - inset)
                drawLine(tick, corner, corner + Offset(arm * dx, 0f), stroke, StrokeCap.Round)
                drawLine(tick, corner, corner + Offset(0f, arm * dy), stroke, StrokeCap.Round)
            }
        }
    }
}

private val reader = QRCodeReader()
private val hints = mapOf(DecodeHintType.POSSIBLE_FORMATS to listOf(BarcodeFormat.QR_CODE), DecodeHintType.TRY_HARDER to true)

/** The luminance plane alone is enough for QR codes, so no color conversion happens. */
private fun decode(image: ImageProxy): String? {
    val plane = image.planes[0]
    val bytes = ByteArray(plane.buffer.remaining()).also { plane.buffer.get(it) }
    val source = PlanarYUVLuminanceSource(bytes, plane.rowStride, image.height, 0, 0, image.width, image.height, false)
    return try {
        reader.decode(BinaryBitmap(HybridBinarizer(source)), hints).text
    } catch (_: ReaderException) {
        null
    } finally {
        reader.reset()
    }
}

private const val PAIRING_PREFIX = "umbriel-link:pair?"
