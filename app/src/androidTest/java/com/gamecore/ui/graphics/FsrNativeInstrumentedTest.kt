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
    fun vulkanRuntimeCanBeCreatedDescribedAndDestroyedRepeatedly() {
        repeat(3) {
            val handle = FsrNative.createVulkanRuntime()
            if (handle == 0L) {
                assertTrue(FsrNative.capabilities().contains("runtime=unavailable"))
                return
            }
            try {
                val description = FsrNative.describeVulkanRuntime(handle)
                assertTrue(description, description.contains("command buffer") &&
                    description.contains("FSR dispatch not yet validated"))
            } finally {
                FsrNative.destroyVulkanRuntime(handle)
            }
        }
    }

    @Test
    fun capabilityReportDoesNotConfuseProbeWithFsrDispatch() {
        val report = FsrNative.capabilities()
        assertTrue(report, report.contains("Vulkan") && report.contains("fsr_dispatch=not-validated"))
        assertTrue(report, report.contains("GLES") && report.contains("fsr_dispatch=not-validated"))
    }

    // ------------------------------------------------------ M4: Vulkan FSR engine

    @Test
    fun vulkanFsrEngineDispatchesReadsBackAndSurvivesRepeatAndResize() {
        var handle = FsrNative.createVulkanFsrEngine(64, 48, 128, 96)
        if (handle == 0L) {
            // An honest unavailable is a pass of this test: the device cannot host the engine and
            // the API says so instead of failing late or faking a dispatch.
            val capabilities = FsrNative.capabilities()
            assertTrue(capabilities, capabilities.contains("fsr_dispatch=not-validated"))
            return
        }
        try {
            val uniform = Bitmap.createBitmap(64, 48, Bitmap.Config.ARGB_8888).apply {
                eraseColor(0xff406080.toInt())
            }
            val output = Bitmap.createBitmap(128, 96, Bitmap.Config.ARGB_8888)
            val first = FsrNative.vulkanFsrProcess(handle, uniform, output, 0f)
            assertTrue(first, first.startsWith("Vulkan EASU+RCAS dispatched;"))
            // A uniform input stays uniform through EASU and RCAS (all neighbour samples equal).
            for (y in 0 until output.height step 7) for (x in 0 until output.width step 7) {
                assertEquals("pixel ($x,$y)", 0xff406080.toInt(), output.getPixel(x, y))
            }

            // Repeated dispatch: no resource exhaustion, stable timing report.
            val second = FsrNative.vulkanFsrProcess(handle, uniform, output, 1f)
            assertTrue(second, second.startsWith("Vulkan EASU+RCAS dispatched;"))

            // Resize then dispatch again: the sized resources were recreated correctly.
            val resizeResult = FsrNative.vulkanFsrResize(handle, 32, 32, 64, 64)
            assertTrue(resizeResult, resizeResult.startsWith("Resized"))
            val resizedInput = Bitmap.createBitmap(32, 32, Bitmap.Config.ARGB_8888).apply {
                eraseColor(0xff102030.toInt())
            }
            val resizedOutput = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888)
            val third = FsrNative.vulkanFsrProcess(handle, resizedInput, resizedOutput, 0f)
            assertTrue(third, third.startsWith("Vulkan EASU+RCAS dispatched;"))

            val description = FsrNative.describeVulkanFsrEngine(handle)
            assertTrue(
                "description reports an earned dispatch flag: $description",
                description.contains("fsr_dispatch=validated"),
            )
            uniform.recycle()
            output.recycle()
            resizedInput.recycle()
            resizedOutput.recycle()
        } finally {
            FsrNative.destroyVulkanFsrEngine(handle)
        }
    }

    @Test
    fun vulkanFsrEngineRejectsDimensionMismatch() {
        val handle = FsrNative.createVulkanFsrEngine(64, 48, 128, 96)
        if (handle == 0L) return
        try {
            val wrongInput = Bitmap.createBitmap(16, 16, Bitmap.Config.ARGB_8888)
            val output = Bitmap.createBitmap(128, 96, Bitmap.Config.ARGB_8888)
            val result = FsrNative.vulkanFsrProcess(handle, wrongInput, output, 0f)
            assertTrue(result, result.startsWith("Vulkan FSR dispatch failed; SDK status="))
            wrongInput.recycle()
            output.recycle()
        } finally {
            FsrNative.destroyVulkanFsrEngine(handle)
        }
    }
}
