package com.gamecore.core.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The preset/buffer math, checked against the four states measured on the CODM reference device
 * (Tecno Spark 40, 720×1600 panel) after a full game restart. These numbers are the contract the
 * verification step judges the compositor against:
 *
 *  - standard: 640×1422 (the game's own choice, not the panel)
 *  - 0.75 → 540×1200
 *  - 0.67 → 482×1072 (accepted although outside the documented 0.05-step list)
 *  - 0.5  → 360×800
 */
class GameModeScalingModelsTest {

    @Test
    fun `ratios reproduce the measured reference buffers`() {
        assertEquals(540 to 1200, GameModeScale.BALANCED.expectedBuffers(720, 1600))
        assertEquals(482 to 1072, GameModeScale.EXPERIMENTAL.expectedBuffers(720, 1600))
        assertEquals(360 to 800, GameModeScale.EXTREME.expectedBuffers(720, 1600))
    }

    @Test
    fun `native has no ratio and no buffers`() {
        assertNull(GameModeScale.NATIVE.ratio)
        assertNull(GameModeScale.NATIVE.expectedBuffers(720, 1600))
    }

    @Test
    fun `ratio arguments are US-locale two-decimal strings`() {
        assertEquals("0.75", GameModeScale.BALANCED.ratioArgument)
        assertEquals("0.67", GameModeScale.EXPERIMENTAL.ratioArgument)
        assertEquals("0.50", GameModeScale.EXTREME.ratioArgument)
        assertNull(GameModeScale.NATIVE.ratioArgument)
    }

    @Test
    fun `ratio lookup rounds to the preset it came from`() {
        assertEquals(GameModeScale.EXTREME, GameModeScale.ofRatio(0.5f))
        assertEquals(GameModeScale.EXTREME, GameModeScale.ofRatio(0.4999f))
        assertEquals(GameModeScale.EXPERIMENTAL, GameModeScale.ofRatio(0.67f))
        assertEquals(GameModeScale.BALANCED, GameModeScale.ofRatio(0.75f))
        assertNull(GameModeScale.ofRatio(0.605f))
        assertNull(GameModeScale.ofRatio(null))
    }

    @Test
    fun `presets stay inside the platform's documented range`() {
        for (preset in GameModeScale.entries) {
            val ratio = preset.ratio ?: continue
            assertTrue("$preset outside 0.3..0.9", ratio >= 0.30f && ratio <= 0.90f)
        }
    }
}
