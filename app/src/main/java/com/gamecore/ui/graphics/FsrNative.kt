package com.gamecore.ui.graphics

/** Thin JNI API for app-owned test images; it cannot access another app's renderer. */
internal object FsrNative {
    init {
        System.loadLibrary("magpie_core")
        System.loadLibrary("magpie_fsr")
        System.loadLibrary("magpie_vulkan")
        System.loadLibrary("magpie_gles")
        System.loadLibrary("magpie_android_jni")
    }

    external fun capabilities(): String
    external fun processCpu(input: android.graphics.Bitmap, output: android.graphics.Bitmap, sharpnessStops: Float): String
    external fun createVulkanRuntime(): Long
    external fun describeVulkanRuntime(handle: Long): String
    external fun destroyVulkanRuntime(handle: Long)
}
