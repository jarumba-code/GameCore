package com.gamecore.ui.graphics

/**
 * Thin JNI API for app-owned test images; it cannot access another app's renderer.
 *
 * The M4 engine functions run the two AMD FSR 1 compute passes on an app-owned Vulkan device
 * against bitmaps GameCore itself creates. The handle is a persistent native engine — created for
 * one input/output size, reusable across dispatches, resizable, and explicitly destroyed — which
 * is what makes the timings per-dispatch GPU costs rather than one-shot setup figures. The engine
 * reports `fsr_dispatch=validated` only after a real dispatch has succeeded; creating it proves
 * nothing and this API never claims otherwise.
 */
internal object FsrNative {
    init {
        System.loadLibrary("magpie_core")
        System.loadLibrary("magpie_fsr")
        System.loadLibrary("magpie_vulkan")
        System.loadLibrary("magpie_vulkan_engine")
        System.loadLibrary("magpie_gles")
        System.loadLibrary("magpie_android_jni")
    }

    external fun capabilities(): String
    external fun processCpu(input: android.graphics.Bitmap, output: android.graphics.Bitmap, sharpnessStops: Float): String
    external fun createVulkanRuntime(): Long
    external fun describeVulkanRuntime(handle: Long): String
    external fun destroyVulkanRuntime(handle: Long)

    /** Returns a native engine handle, or 0 when the device or the configuration is unsupported. */
    external fun createVulkanFsrEngine(inputWidth: Int, inputHeight: Int, outputWidth: Int, outputHeight: Int): Long
    external fun describeVulkanFsrEngine(handle: Long): String
    external fun vulkanFsrProcess(handle: Long, input: android.graphics.Bitmap, output: android.graphics.Bitmap, sharpnessStops: Float): String
    external fun vulkanFsrResize(handle: Long, inputWidth: Int, inputHeight: Int, outputWidth: Int, outputHeight: Int): String
    external fun destroyVulkanFsrEngine(handle: Long)
}
