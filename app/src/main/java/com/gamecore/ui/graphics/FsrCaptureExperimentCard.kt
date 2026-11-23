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
 * A clearly separated experiment: upscale the consented, already-composited display capture.
 * This is not access to a game's render target and cannot reduce the game's rendering workload.
 */
@Composable
internal fun FsrCaptureExperimentCard(modifier: Modifier = Modifier) {
    val viewModel: CaptureExperimentViewModel = hiltViewModel()
    val state by viewModel.uiState.collectAsStateWithLifecycle()

    androidx.compose.material3.Card(modifier = modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Capture & 2x upscale (experimental)", style = MaterialTheme.typography.titleMedium)
            Text(
                "Captures the displayed screen after Android's compositor, then runs 2x Vulkan " +
                    "EASU+RCAS. This is visual post-processing only: it cannot access the game's " +
                    "original render buffer, restore detail already lost, or reduce the game's GPU load. " +
                    "The 2x output is memory-limited on purpose.",
                style = MaterialTheme.typography.bodySmall,
                fontSize = 13.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Button(enabled = !state.busy, onClick = viewModel::run) {
                Text(if (state.busy) "Capturing…" else "Capture & upscale one frame")
            }
            Text(state.statusLine, style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                state.captured?.let {
                    Image(it.asImageBitmap(), "Composited screen capture", Modifier.weight(1f).height(110.dp))
                }
                state.processed?.let {
                    Image(it.asImageBitmap(), "Two-times EASU and RCAS output", Modifier.weight(1f).height(110.dp))
                }
            }
        }
    }
}
