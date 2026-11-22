package com.gamecore.ui.graphics

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle

/**
 * The capture-and-sharpen experiment card — the UI of the frame-access decision's option D
 * (docs/ANDROID_MAGPIE_FRAME_ACCESS_DECISION.md).
 *
 * What this card does: captures the **displayed** screen through the existing consented
 * MediaProjection path, runs that frame through the app-owned Vulkan engine at its captured size,
 * and measures how long the capture-plus-process step took. What it does not do, and says so in
 * plain words on the card: the captured frame is the compositor's post-enlargement image. A
 * downscaled game's pre-enlargement detail is already discarded by the platform's hardware scaler
 * before capture, EASU here runs at 1:1 (there is nothing to upsample), and only RCAS sharpening
 * contributes. This is a latency-and-feasibility experiment on the displayed capture — never a
 * claim of renderer-integrated FSR and never a claim of recovered resolution.
 */
@Composable
internal fun FsrCaptureExperimentCard(modifier: Modifier = Modifier) {
    val viewModel: CaptureExperimentViewModel = hiltViewModel()
    val state by viewModel.uiState.collectAsStateWithLifecycle()

    androidx.compose.material3.Card(modifier = modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Capture & sharpen (experimental)", style = MaterialTheme.typography.titleMedium)
            Text(
                "Processes the displayed capture — the image after Android's compositor has " +
                    "already enlarged it. It cannot restore detail that was discarded, and the " +
                    "game's own rendering is untouched.",
                style = MaterialTheme.typography.bodySmall,
                fontSize = 13.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Button(enabled = !state.busy, onClick = viewModel::run) {
                Text(if (state.busy) "Capturing…" else "Capture & sharpen one frame")
            }
            Text(state.statusLine, style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                state.captured?.let {
                    Image(it.asImageBitmap(), "Captured frame", Modifier.weight(1f).height(110.dp))
                }
                state.processed?.let {
                    Image(it.asImageBitmap(), "RCAS-sharpened capture", Modifier.weight(1f).height(110.dp))
                }
            }
        }
    }
}
