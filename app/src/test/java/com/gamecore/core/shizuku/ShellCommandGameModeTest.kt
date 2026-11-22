package com.gamecore.core.shizuku

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Factory validation for the Game Mode and SurfaceFlinger verification commands. These tests pin
 * the exact argv each command reaches `exec` with — the sealed allowlist is only as good as the
 * argument discipline behind each factory, and a ratio that survives a locale slip (`0,75`) or a
 * layer name that names another package's surface is the failure class these tests exist for.
 */
class ShellCommandGameModeTest {

    // ---------------------------------------------------------------- ratio commands

    @Test
    fun `set downscale builds the exact argv for each preset`() {
        val balanced = ShellCommand.gameModeSetDownscale(PKG, GameModeRatioFixtures.BALANCED)!!
        assertEquals(
            listOf("cmd", "game", "set", "--downscale", "0.75", PKG),
            balanced.argv,
        )
        val experimental = ShellCommand.gameModeSetDownscale(PKG, 0.67f)!!
        assertEquals(
            listOf("cmd", "game", "set", "--downscale", "0.67", PKG),
            experimental.argv,
        )
        val extreme = ShellCommand.gameModeSetDownscale(PKG, 0.5f)!!
        assertEquals(
            listOf("cmd", "game", "set", "--downscale", "0.50", PKG),
            extreme.argv,
        )
    }

    @Test
    fun `ratios outside the documented range are refused`() {
        assertNull(ShellCommand.gameModeSetDownscale(PKG, 0.29f))
        assertNull(ShellCommand.gameModeSetDownscale(PKG, 0.91f))
        assertNull(ShellCommand.gameModeSetDownscale(PKG, Float.NaN))
        assertNull(ShellCommand.gameModeSetDownscale(PKG, Float.POSITIVE_INFINITY))
    }

    @Test
    fun `malformed packages are refused everywhere`() {
        assertNull(ShellCommand.gameModeSetDownscale("not a package", 0.5f))
        assertNull(ShellCommand.gameModeListModes("../escape"))
        assertNull(ShellCommand.gameModeListConfigs(""))
        assertNull(ShellCommand.gameModeSetStandardMode("com..double"))
        assertNull(ShellCommand.gameModeResetOverrides("has spaces"))
    }

    @Test
    fun `read commands carry their exact argv`() {
        assertEquals(
            listOf("cmd", "game", "help"),
            ShellCommand.gameModeHelp().argv,
        )
        assertEquals(
            listOf("cmd", "game", "list-modes", PKG),
            ShellCommand.gameModeListModes(PKG)!!.argv,
        )
        assertEquals(
            listOf("cmd", "game", "list-configs", PKG),
            ShellCommand.gameModeListConfigs(PKG)!!.argv,
        )
        assertEquals(
            listOf("cmd", "game", "mode", "standard", PKG),
            ShellCommand.gameModeSetStandardMode(PKG)!!.argv,
        )
        assertEquals(
            listOf("cmd", "game", "reset", PKG),
            ShellCommand.gameModeResetOverrides(PKG)!!.argv,
        )
    }

    // ---------------------------------------------------------------- effects

    @Test
    fun `reads are read-only and writes change state`() {
        assertTrue(ShellCommand.gameModeHelp().isReadOnly)
        assertTrue(ShellCommand.gameModeListModes(PKG)!!.isReadOnly)
        assertTrue(ShellCommand.gameModeListConfigs(PKG)!!.isReadOnly)
        assertTrue(!ShellCommand.gameModeSetDownscale(PKG, 0.5f)!!.isReadOnly)
        assertTrue(!ShellCommand.gameModeSetStandardMode(PKG)!!.isReadOnly)
        assertTrue(!ShellCommand.gameModeResetOverrides(PKG)!!.isReadOnly)
    }

    // ---------------------------------------------------------------- layer latency

    @Test
    fun `layer latency binds the name to the package`() {
        val command = ShellCommand.surfaceFlingerLayerLatency(PKG, CODM_LAYER)
        assertNotNull(command)
        assertEquals(
            listOf("dumpsys", "SurfaceFlinger", "--latency", CODM_LAYER),
            command!!.argv,
        )
    }

    @Test
    fun `another package's layer name is refused`() {
        assertNull(ShellCommand.surfaceFlingerLayerLatency("com.other.game", CODM_LAYER))
    }

    @Test
    fun `malformed layer names are refused`() {
        assertNull(ShellCommand.surfaceFlingerLayerLatency(PKG, "SurfaceView[$PKG/com.Activity]#189977"))
        assertNull(ShellCommand.surfaceFlingerLayerLatency(PKG, "not a layer at all"))
        assertNull(ShellCommand.surfaceFlingerLayerLatency(PKG, ""))
        // A crafted traversal in the component position does not survive the grammar check.
        assertNull(
            ShellCommand.surfaceFlingerLayerLatency(PKG, "SurfaceView[$PKG/../evil](BLAST)#1"),
        )
    }

    @Test
    fun `surfaceflinger dumps are read-only`() {
        assertTrue(ShellCommand.surfaceFlingerLayerList().isReadOnly)
        assertTrue(ShellCommand.surfaceFlingerDump().isReadOnly)
        assertTrue(ShellCommand.surfaceFlingerLayerLatency(PKG, CODM_LAYER)!!.isReadOnly)
    }

    private companion object {
        const val PKG = "com.activision.callofduty.shooter"
        const val CODM_LAYER =
            "SurfaceView[$PKG/com.tencent.tmgp.cod.CODMainActivity](BLAST)#189977"
    }
}

/** The presets as plain floats, so this test does not reach into the model module. */
private object GameModeRatioFixtures {
    const val BALANCED = 0.75f
}
