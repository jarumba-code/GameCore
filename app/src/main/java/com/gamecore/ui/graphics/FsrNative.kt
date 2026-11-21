package com.gamecore.ui.graphics

import android.graphics.Bitmap

/** Thin JNI API. Vulkan/GLES are deliberately reported unavailable until app dispatch is validated. */
internal object FsrNative {
    init { System.loadLibrary("magpie_graphics") }
    external fun capabilities(): String
    external fun processCpu(input: Bitmap, output: Bitmap, sharpnessStops: Float): String
}
