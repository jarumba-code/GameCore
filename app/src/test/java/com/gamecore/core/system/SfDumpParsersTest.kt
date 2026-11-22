package com.gamecore.core.system

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Fixture-driven tests for the two SurfaceFlinger dumps the per-game scaling feature verifies
 * against. The fixtures are the shapes measured on the CODM reference device (Tecno Spark 40,
 * Android 15), including the sentinel rows the latency dump uses for "not presented".
 */
class SfDumpParsersTest {

    // ---------------------------------------------------------------- layer resolution

    @Test
    fun `blast layer resolves from the list dump`() {
        val name = SfDumpParsers.blastLayerNameFor(
            "com.activision.callofduty.shooter",
            LIST_DUMP,
        )
        assertEquals(
            "SurfaceView[com.activision.callofduty.shooter/com.tencent.tmgp.cod.CODMainActivity](BLAST)#189977",
            name,
        )
    }

    @Test
    fun `each package resolves its own layer from the same dump`() {
        // The fixture holds two games' surfaces; each query returns its own.
        assertEquals(
            "SurfaceView[com.other.game/com.other.GameActivity](BLAST)#111",
            SfDumpParsers.blastLayerNameFor("com.other.game", LIST_DUMP),
        )
    }

    @Test
    fun `a package absent from the dump resolves to null`() {
        assertNull(SfDumpParsers.blastLayerNameFor("com.absent.game", LIST_DUMP))
    }

    @Test
    fun `a malformed package is refused`() {
        assertNull(SfDumpParsers.blastLayerNameFor("  ", LIST_DUMP))
        assertNull(SfDumpParsers.blastLayerNameFor("com.activision.callofduty.shooter", ""))
    }

    // ---------------------------------------------------------------- buffer measurement

    @Test
    fun `standard-mode buffers measure 640x1422`() {
        val sizes = SfDumpParsers.bufferSizesFor("com.activision.callofduty.shooter", STD_DUMP)
        assertEquals(setOf(640 to 1422), sizes)
    }

    @Test
    fun `scaled-mode buffers measure 360x800`() {
        val sizes = SfDumpParsers.bufferSizesFor("com.activision.callofduty.shooter", SCALED_DUMP)
        assertEquals(setOf(360 to 800), sizes)
    }

    @Test
    fun `other packages' buffers are ignored`() {
        val sizes = SfDumpParsers.bufferSizesFor("com.android.systemui", STD_DUMP)
        assertTrue(sizes.isEmpty())
    }

    @Test
    fun `mixed sizes are all reported`() {
        val mixed = STD_DUMP + "\n" + SCALED_BUFFER_LINE
        val sizes = SfDumpParsers.bufferSizesFor("com.activision.callofduty.shooter", mixed)
        assertEquals(setOf(640 to 1422, 360 to 800), sizes)
    }

    // ---------------------------------------------------------------- latency

    @Test
    fun `latency parses the paced 30 fps cadence with sentinels filtered`() {
        val stats = SfDumpParsers.parseLayerLatency(LATENCY_DUMP_PACED)!!
        assertEquals(16_666_667L, stats.refreshPeriodNanos)
        assertEquals(124, stats.frameCount)
        assertEquals(0.033_320 * 1_000, stats.medianDeltaNanos / 1e6, 0.01)
        assertTrue("the game paces at half refresh", stats.isHalfRefreshPaced())
        // The sentinel rows are excluded from every statistic; the only p99 elevation they can
        // cause is one two-period delta per skipped row (the row between two real presents is
        // absent), so anything at or beyond three periods would mean a sentinel leaked in.
        assertTrue(
            "p99 must stay within two present periods + jitter, got ${stats.p99DeltaNanos / 1e6} ms",
            stats.p99DeltaNanos <= 3L * 33_320_000L,
        )
    }

    @Test
    fun `latency parses a faster cadence as not half-refresh paced`() {
        val stats = SfDumpParsers.parseLayerLatency(LATENCY_DUMP_FAST)!!
        assertEquals(0.016_660 * 1_000, stats.medianDeltaNanos / 1e6, 0.01)
        assertTrue(!stats.isHalfRefreshPaced())
    }

    @Test
    fun `an unusable dump returns null`() {
        assertNull(SfDumpParsers.parseLayerLatency(""))
        assertNull(SfDumpParsers.parseLayerLatency("not a number\n1 2 3"))
        assertNull(SfDumpParsers.parseLayerLatency("999\n1 2 3\n"))
        // All-sentinel rows: nothing measurable.
        assertNull(
            SfDumpParsers.parseLayerLatency(
                "16666667\n" + (1..15).joinToString("\n") { "0 0 0" },
            ),
        )
    }

    private companion object {
        const val LIST_DUMP = """
RequestedLayerState{Task=1#47 parentId=8}
RequestedLayerState{StatusBar#77 parentId=190120}
RequestedLayerState{SurfaceView[com.other.game/com.other.GameActivity](BLAST)#111 parentId=9}
RequestedLayerState{8fe9bc3 com.activision.callofduty.shooter/com.tencent.tmgp.cod.CODMainActivity#189884 parentId=189867}
RequestedLayerState{SurfaceView[com.activision.callofduty.shooter/com.tencent.tmgp.cod.CODMainActivity](BLAST)#189977 parentId=189976}
RequestedLayerState{Background for SurfaceView[com.activision.callofduty.shooter/com.tencent.tmgp.cod.CODMainActivity]#189978 parentId=189976}
"""

        const val STD_BUFFER_LINE = """
+ name:SurfaceView[com.activision.callofduty.shooter/com.tencent.tmgp.cod.CODMainActivity]#4(BLAST Consumer)4, id:5299990079990, size:1777.50KiB, w/h:640x1422, usage: 0x40000000000b00, req fmt:4
"""

        const val SCALED_BUFFER_LINE = """
+ name:SurfaceView[com.activision.callofduty.shooter/com.tencent.tmgp.cod.CODMainActivity]#4(BLAST Consumer)4, id:5299990099001, size:1131.00KiB, w/h:360x800, usage: 0x40000000000b00, req fmt:4
"""

        const val STD_DUMP = """
+ name:FramebufferSurface, id:5299990080044, size:4500.00KiB, w/h:720x1600, usage: 0x1b00
+ name:VRI[StatusBar]#4(BLAST Consumer)4, id:5299990080008, size:300.00KiB, w/h:48x1600
+ name:SurfaceView[com.other.game/com.other.GameActivity]#4(BLAST Consumer)4, id:5299990080000, size:900.00KiB, w/h:480x854
$STD_BUFFER_LINE
+ name:SurfaceView[com.activision.callofduty.shooter/com.tencent.tmgp.cod.CODMainActivity]#4(BLAST Consumer)4, id:5299990079989, size:1777.50KiB, w/h:640x1422, usage: 0x40000000000b00
+ name:SurfaceView[com.activision.callofduty.shooter/com.tencent.tmgp.cod.CODMainActivity]#4(BLAST Consumer)4, id:5299990079983, size:1777.50KiB, w/h:640x1422, usage: 0x40000000000b00
"""

        const val SCALED_DUMP = """
+ name:FramebufferSurface, id:5299990080044, size:4500.00KiB, w/h:720x1600, usage: 0x1b00
+ name:VRI[StatusBar]#4(BLAST Consumer)4, id:5299990080008, size:300.00KiB, w/h:48x1600
+ name:SurfaceView[com.activision.callofduty.shooter/com.tencent.tmgp.cod.CODMainActivity]#4(BLAST Consumer)4, id:5299990099001, size:1131.00KiB, w/h:360x800, usage: 0x40000000000b00
+ name:SurfaceView[com.activision.callofduty.shooter/com.tencent.tmgp.cod.CODMainActivity]#4(BLAST Consumer)4, id:5299990099002, size:1131.00KiB, w/h:360x800, usage: 0x40000000000b00
+ name:SurfaceView[com.activision.callofduty.shooter/com.tencent.tmgp.cod.CODMainActivity]#4(BLAST Consumer)4, id:5299990099003, size:1131.00KiB, w/h:360x800, usage: 0x40000000000b00
"""

        fun latencyDump(period: Long, deltaNanos: Long, frameCount: Int, sentinelEvery: Int = 0): String {
            var t = 100_000_000_000L
            val rows = StringBuilder()
            rows.append(period).append('\n')
            repeat(frameCount) { i ->
                t += deltaNanos
                if (sentinelEvery > 0 && i % sentinelEvery == 0) {
                    rows.append("0 ").append(Long.MAX_VALUE).append(" 0\n")
                } else {
                    rows.append(t - deltaNanos / 2).append(' ').append(t).append(' ')
                        .append(t - deltaNanos / 3).append('\n')
                }
            }
            return rows.toString()
        }

        val LATENCY_DUMP_PACED = latencyDump(16_666_667L, 33_320_000L, 130, sentinelEvery = 25)
        val LATENCY_DUMP_FAST = latencyDump(16_666_667L, 16_660_000L, 130)
    }
}
