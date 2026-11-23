package com.gamecore.ui.graphics

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class FsrCaptureSizingTest {
    @Test
    fun computesExactTwoXOutputDimensions() {
        assertEquals(
            FsrCaptureOutputSize(1440, 3200),
            FsrCaptureSizing.outputSize(720, 1600),
        )
    }

    @Test
    fun rejectsOutputAboveMemoryBudget() {
        assertNull(FsrCaptureSizing.outputSize(1080, 2400))
    }

    @Test
    fun rejectsInvalidAndOverflowingDimensions() {
        assertNull(FsrCaptureSizing.outputSize(0, 100))
        assertNull(FsrCaptureSizing.outputSize(-1, 100))
        assertNull(FsrCaptureSizing.outputSize(Int.MAX_VALUE, 1))
    }
}
