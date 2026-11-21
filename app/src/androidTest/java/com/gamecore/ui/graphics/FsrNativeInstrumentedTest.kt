package com.gamecore.ui.graphics

import android.graphics.Bitmap
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class FsrNativeInstrumentedTest {
    @Test
    fun sdkLibrariesLoadAndCpuPassesPreserveUniformImage() {
        val input = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888).apply {
            eraseColor(0xff406080.toInt())
        }
        val output = Bitmap.createBitmap(16, 16, Bitmap.Config.ARGB_8888)
        val result = FsrNative.processCpu(input, output, 2f)
        assertTrue(result, result.startsWith("CPU EASU+RCAS elapsed_ms="))
        for (y in 0 until output.height) for (x in 0 until output.width) {
            assertEquals("pixel ($x,$y)", 0xff406080.toInt(), output.getPixel(x, y))
        }
        input.recycle()
        output.recycle()
    }

    @Test
    fun invalidLargeScaleIsRejectedThroughSdkBoundary() {
        val input = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888)
        val output = Bitmap.createBitmap(24, 24, Bitmap.Config.ARGB_8888)
        val result = FsrNative.processCpu(input, output, 2f)
        assertTrue(result, result.startsWith("CPU FSR failed; SDK status="))
        input.recycle()
        output.recycle()
    }

    @Test
    fun capabilityReportDoesNotConfuseProbeWithFsrDispatch() {
        val report = FsrNative.capabilities()
        assertTrue(report, report.contains("Vulkan") && report.contains("fsr_dispatch=not-validated"))
        assertTrue(report, report.contains("GLES") && report.contains("fsr_dispatch=not-validated"))
    }
}
