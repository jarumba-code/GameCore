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
 * State for one capture-and-upscale experiment run. [captured] is the composited screen capture;
 * [processed] is its 2x EASU+RCAS output. [statusLine] reports what actually happened.
 */
data class CaptureExperimentUiState(
    val busy: Boolean = false,
    val captured: Bitmap? = null,
    val processed: Bitmap? = null,
    val statusLine: String = "Not run. This experiment processes a post-composition screen capture; " +
        "it does not change a game's render resolution or recover lost detail.",
)

/**
 * Experimental post-composition capture processing.
 *
 * MediaProjection supplies the already-composited display image, not another app's render target.
 * This experiment therefore demonstrates a 2x spatial upscale of that captured image only. It
 * cannot reduce the game's GPU workload, access the game's pre-composition buffers, or restore
 * detail already discarded by Android's compositor/display scaler.
 */
@HiltViewModel
class CaptureExperimentViewModel @Inject constructor(
    private val capture: ScreenCaptureController,
) : ViewModel() {

    private val state = MutableStateFlow(CaptureExperimentUiState())
    val uiState: StateFlow<CaptureExperimentUiState> = state.asStateFlow()

    /** Persistent native engine, reused while the captured input dimensions remain unchanged. */
    private var engineHandle = 0L
    private var engineSize: Pair<Int, Int> = 0 to 0

    fun run() {
        if (state.value.busy) return
        viewModelScope.launch {
            state.update { it.copy(busy = true) }
            val outcome: RunResult = withContext(Dispatchers.Default) {
                runCatching { captureAndProcess() }.getOrElse {
                    RunResult("Capture experiment failed: ${it.message}", null, null)
                }
            }
            state.update {
                it.copy(
                    busy = false,
                    statusLine = outcome.statusLine,
                    captured = outcome.captured,
                    processed = outcome.processed,
                )
            }
        }
    }

    private data class RunResult(
        val statusLine: String,
        val captured: Bitmap?,
        val processed: Bitmap?,
    )

    private suspend fun captureAndProcess(): RunResult {
        val started = System.nanoTime()
        return when (val outcome = capture.captureFrameToMemory()) {
            CaptureOutcome.Extracted -> {
                val frame = capture.lastExtractedFrame.value
                    ?: return RunResult("Capture completed but produced no frame.", null, null)

                val inputWidth = frame.width
                val inputHeight = frame.height
                val outputSize = FsrCaptureSizing.outputSize(inputWidth, inputHeight)
                    ?: return RunResult(
                        "Captured ${inputWidth}x${inputHeight}, but its 2x output exceeds the " +
                            "safe bitmap budget (${FsrCaptureSizing.MAX_OUTPUT_PIXELS} pixels). " +
                            "No processing was attempted.",
                        frame,
                        null,
                    )

                var handle = engineHandle
                if (handle == 0L || engineSize != (inputWidth to inputHeight)) {
                    if (handle != 0L) {
                        FsrNative.destroyVulkanFsrEngine(handle)
                        engineHandle = 0L
                    }
                    handle = FsrNative.createVulkanFsrEngine(
                        inputWidth,
                        inputHeight,
                        outputSize.width,
                        outputSize.height,
                    )
                    engineHandle = handle
                    engineSize = if (handle != 0L) inputWidth to inputHeight else 0 to 0
                }

                if (handle == 0L) {
                    return RunResult(
                        "The Vulkan FSR engine could not create resources for " +
                            "${inputWidth}x${inputHeight} → ${outputSize.width}x${outputSize.height}. " +
                            "No processing is claimed.",
                        frame,
                        null,
                    )
                }

                val processed = Bitmap.createBitmap(
                    outputSize.width,
                    outputSize.height,
                    Bitmap.Config.ARGB_8888,
                )
                val processResult = FsrNative.vulkanFsrProcess(handle, frame, processed, 1f)
                val elapsedMs = (System.nanoTime() - started) / 1_000_000.0
                if (!processResult.startsWith("Vulkan EASU+RCAS dispatched;")) {
                    processed.recycle()
                    RunResult(
                        "Captured ${inputWidth}x${inputHeight}; Vulkan processing failed after " +
                            "${"%.1f".format(java.util.Locale.US, elapsedMs)}ms: $processResult",
                        frame,
                        null,
                    )
                } else {
                    RunResult(
                        "Post-composition capture ${inputWidth}x${inputHeight} → " +
                            "${outputSize.width}x${outputSize.height} (2x EASU+RCAS). " +
                            "Capture+process wall time " +
                            "${"%.1f".format(java.util.Locale.US, elapsedMs)}ms. " +
                            "Visual post-processing only; game rendering is unchanged. $processResult",
                        frame,
                        processed,
                    )
                }
            }
            CaptureOutcome.NeedsConsent -> RunResult(
                "Screen-capture consent is needed first. Nothing was captured.",
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
