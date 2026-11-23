package com.gamecore.ui.graphics

/**
 * Computes the fixed 2x output dimensions required by the native Vulkan EASU+RCAS engine.
 *
 * Captured frames are already compositor output. Upscaling them is a visual post-process only;
 * it does not lower the game's render workload or recover detail lost before capture.
 */
internal data class FsrCaptureOutputSize(val width: Int, val height: Int)

internal object FsrCaptureSizing {
    // Keep one output bitmap under roughly 48 MiB (RGBA_8888), before Vulkan intermediates.
    // This is a conservative app-side guard for low-memory Android devices.
    const val MAX_OUTPUT_PIXELS: Long = 12_000_000L

    fun outputSize(inputWidth: Int, inputHeight: Int): FsrCaptureOutputSize? {
        if (inputWidth <= 0 || inputHeight <= 0) return null

        val outputWidth = inputWidth.toLong() * 2L
        val outputHeight = inputHeight.toLong() * 2L
        if (outputWidth > Int.MAX_VALUE || outputHeight > Int.MAX_VALUE) return null
        if (outputWidth * outputHeight > MAX_OUTPUT_PIXELS) return null

        return FsrCaptureOutputSize(outputWidth.toInt(), outputHeight.toInt())
    }
}
