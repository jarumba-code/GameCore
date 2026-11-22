package com.gamecore.ui.graphics

import android.graphics.Bitmap
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.gamecore.core.model.CaptureOutcome
import com.gamecore.core.system.ScreenCaptureController
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import javax.inject.Inject

/**
 * State for one capture-and-sharpen experiment run. [captured] and [processed] are the pre/post
 * frames of the displayed capture; [statusLine] carries the honest narration of what happened,
 * including the latency measurement.
 */
data class CaptureExperimentUiState(
    val busy: Boolean = false,
    val captured: Bitmap? = null,
    val processed: Bitmap? = null,
    val statusLine: String = "Not run. The processed frame is the displayed capture — not a " +
        "game's render target and not renderer-integrated FSR.",
)

/**
 * The capture-and-process experiment — option D of the frame-access decision
 * (docs/ANDROID_MAGPIE_FRAME_ACCESS_DECISION.md), kept deliberately separate from
 * renderer-integrated FSR.
 *
 * What a run does: captures the **displayed** screen through the existing consented
 * MediaProjection path ([ScreenCaptureController.captureFrameToMemory]), feeds that frame through
 * the app-owned Vulkan engine at its captured size, and measures the wall-clock cost of the whole
 * step. What it does not do, and says so in plain words: the captured frame is the compositor's
 * post-enlargement image. A downscaled game's pre-enlargement detail was already discarded by the
 * platform's hardware scaler before capture; EASU here runs at 1:1 (there is nothing to upsample)
 * and only RCAS sharpening contributes. This is a latency-and-feasibility experiment on the
 * displayed capture — never a claim of renderer-integrated FSR, never a claim of recovered
 * resolution, and it leaves the game's rendering untouched.
 */
@HiltViewModel
class CaptureExperimentViewModel @Inject constructor(
    private val capture: ScreenCaptureController,
) : ViewModel() {

    private val state = MutableStateFlow(CaptureExperimentUiState())
    val uiState: StateFlow<CaptureExperimentUiState> = state.asStateFlow()

    /** The native FSR engine, created lazily at the captured frame's size, reused across runs. */
    private var engineHandle = 0L
    private var engineSize: Pair<Int, Int> = 0 to 0

    fun run() {
        if (state.value.busy) return
        viewModelScope.launch {
            state.update { it.copy(busy = true) }
            val outcome = withContext(Dispatchers.Default) {
                runCatching { captureAndProcess() }.getOrElse { "Capture experiment failed: ${it.message}" }
            }
            state.update { it.copy(busy = false, statusLine = outcome.statusLine, captured = outcome.captured, processed = outcome.processed) }
        }
    }

    private data class RunResult(
        val statusLine: String,
        val captured: Bitmap?,
        val processed: Bitmap?,
    )

    private suspend fun captureAndProcess(): RunResult {
        val started = System.currentTimeMillis()
        return when (val outcome = capture.captureFrameToMemory()) {
            CaptureOutcome.Extracted -> {
                val frame = capture.lastExtractedFrame.value
                    ?: return RunResult("Capture completed but produced no frame.", null, null)
                val width = frame.width
                val height = frame.height
                var handle = engineHandle
                if (handle == 0L || engineSize != width to height) {
                    if (handle != 0L) FsrNative.destroyVulkanFsrEngine(handle)
                    handle = FsrNative.createVulkanFsrEngine(width, height, width, height)
                    engineHandle = handle
                    engineSize = width to height
                }
                if (handle == 0L) {
                    return RunResult(
                        "The Vulkan engine could not host this frame size (${width}x${height}); " +
                            "no processing was attempted and none is claimed.",
                        frame,
                        null,
                    )
                }
                val processed = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                val processResult = FsrNative.vulkanFsrProcess(handle, frame, processed, 1f)
                val totalMs = System.currentTimeMillis() - started
                RunResult(
                    "Captured ${width}x${height} (the displayed image, post-composition); " +
                        "capture+process wall time ${totalMs}ms; $processResult",
                    frame,
                    processed,
                )
            }
            CaptureOutcome.NeedsConsent -> RunResult(
                "Screen-capture consent is needed first — grant it from the capture feature that " +
                    "owns the projection. Nothing was captured.",
                null,
                null,
            )
            is CaptureOutcome.Failed -> RunResult("Capture failed: ${outcome.detail}", null, null)
            is CaptureOutcome.Unsupported -> RunResult("Capture unsupported: ${outcome.detail}", null, null)
            else -> RunResult("Capture did not produce a frame.", null, null)
        }
    }

    override fun onCleared() {
        if (engineHandle != 0L) {
            FsrNative.destroyVulkanFsrEngine(engineHandle)
            engineHandle = 0L
        }
        super.onCleared()
    }
}
