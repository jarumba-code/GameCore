package com.gamecore.core.system

import kotlin.math.abs

/**
 * Parsers for the two SurfaceFlinger dumps the per-game scaling feature verifies against, kept in
 * the same spirit as [DumpsysParsers]: parse only what the platform actually prints, return null on
 * anything unrecognizable, and never turn a parse failure into a plausible-looking value.
 *
 * The buffer parse is the authoritative half of the feature. `dumpsys SurfaceFlinger` prints one
 * `+ name:` line per gralloc allocation, and a game rendering through a SurfaceView names its
 * buffers after the view — measured on the CODM reference device:
 *
 * ```
 * + name:SurfaceView[com.activision.callofduty.shooter/com.tencent.tmgp.cod.CODMainActivity]#4(BLAST Consumer)4, id:..., size:1777.50KiB, w/h:640x1422, ...
 * ```
 *
 * so a package's live render-buffer dimensions are the `w/h:` values of the lines that name it. A
 * `wm size` exit code says nothing about whether an intervention resized a game; these numbers are
 * the only non-root evidence that does.
 *
 * The layer parse is the attribution half. `dumpsys SurfaceFlinger --list` prints one
 * `RequestedLayerState{...}` line per layer, and the game's own surface is the one containing
 * `SurfaceView[<package>/...` and `(BLAST)`. Naming that layer in a per-layer
 * `dumpsys SurfaceFlinger --latency '<name>'` call is what makes the frame timing the game's own —
 * the whole-device latency dump cannot be attributed, which is why [com.gamecore.core.system.FrameRateProbe]
 * refuses it; per-layer naming solves exactly that refusal, verified against the reference device,
 * where the named layer reports the game's paced 33.32 ms cadence while `gfxinfo` sees five UI frames.
 */
object SfDumpParsers {

    /**
     * The game's BLAST SurfaceView layer name in [listText], or null. Only a line that carries the
     * package inside its `SurfaceView[...]` bracket and a `(...BLAST...)` marker qualifies; the
     * first match wins, since `--list` prints each layer once. The returned string is what
     * [com.gamecore.core.shizuku.ShellCommand] validates again before it reaches a command — a
     * parser finding it does not exempt the command factory from checking it.
     */
    fun blastLayerNameFor(packageName: String, listText: String): String? {
        val validPackage = packageName.trim()
        if (validPackage.isEmpty()) return null
        for (rawLine in listText.lineSequence()) {
            val line = rawLine.trim()
            if (!line.contains("(BLAST)")) continue
            val start = line.indexOf("SurfaceView[")
            if (start < 0) continue
            val openBracket = start + "SurfaceView[".length
            val close = line.indexOf(']', openBracket)
            if (close < 0) continue
            val inner = line.substring(openBracket, close)
            // The bracket content is "<package>/<activity>"; the package must be the requested one.
            val pkg = inner.substringBefore('/').trim()
            if (pkg != validPackage) continue
            // The name this parser returns ends at the layer id; later fields are dump decoration.
            val idMatch = ID_SUFFIX.find(line.substring(close))
            if (idMatch != null) {
                return line.substring(start, close + 1) + idMatch.value
            }
        }
        return null
    }

    private val ID_SUFFIX = Regex("""\(BLAST\)#\d+""")

    /**
     * Every distinct render-buffer `width x height` the named package has allocated, most recent
     * information first is not knowable from the dump, so the caller compares a set: an applied
     * intervention shows *all* the game's current buffers at the scaled size. Distinct sizes are
     * returned sorted by area so a mixed state (a resize in progress) is visible rather than
     * collapsed.
     */
    fun bufferSizesFor(packageName: String, dumpText: String): Set<Pair<Int, Int>> {
        val results = LinkedHashSet<Pair<Int, Int>>()
        val validPackage = packageName.trim()
        if (validPackage.isEmpty()) return results
        for (rawLine in dumpText.lineSequence()) {
            val line = rawLine.trim()
            if (!line.startsWith("+ name:")) continue
            if (!line.contains("SurfaceView[")) continue
            val openBracket = line.indexOf("SurfaceView[") + "SurfaceView[".length
            val close = line.indexOf(']', openBracket)
            if (close < 0) continue
            if (line.substring(openBracket, close).substringBefore('/').trim() != validPackage) continue
            val size = SIZE_FIELD.find(line) ?: continue
            val (w, h) = size.destructured
            val width = w.toIntOrNull() ?: continue
            val height = h.toIntOrNull() ?: continue
            if (width <= 0 || height <= 0) continue
            results += width to height
        }
        return results.sortedBy { (w, h) -> w.toLong() * h }.toSet()
    }

    private val SIZE_FIELD = Regex("""\bw/h:(\d+)x(\d+)""")

    /**
     * Frame-present timing from a per-layer `dumpsys SurfaceFlinger --latency` dump, or null when
     * the dump is not a usable latency section at all.
     *
     * The format is a refresh-period header in nanoseconds followed by rows of
     * `desiredPresent actualPresent frameReady`. Zero, one and negative timestamps are the
     * dump's "no data" markers, and an effectively-maximal timestamp is the "frame not yet
     * presented" marker — the same sentinels [DumpsysParsers.hasUsableFrameLatency] filters, listed
     * explicitly here rather than bounded by a magnitude guess. Consecutive actual-present times
     * give the cadence the game is pacing at; frameReady-to-present gives the queueing latency.
     */
    fun parseLayerLatency(text: String): SfLayerLatencyStats? {
        val lines = text.lineSequence().map { it.trim() }.filter { it.isNotEmpty() }.toList()
        if (lines.isEmpty()) return null
        val period = lines.first().toLongOrNull() ?: return null
        if (period !in 1_000_000L..100_000_000L) return null
        val presents = ArrayList<Long>()
        val latencies = ArrayList<Long>()
        for (line in lines.drop(1)) {
            val parts = line.split(WHITESPACE)
            if (parts.size < 3) continue
            val desired = parts[0].toLongOrNull() ?: continue
            val actual = parts[1].toLongOrNull() ?: continue
            val ready = parts[2].toLongOrNull() ?: continue
            if (desired <= 0L || actual <= 0L || ready <= 0L) continue
            if (desired == Long.MAX_VALUE || actual == Long.MAX_VALUE || ready == Long.MAX_VALUE) continue
            presents += actual
            if (actual > ready) latencies += actual - ready
        }
        if (presents.size < MIN_FRAMES) return null
        val deltas = ArrayList<Long>(presents.size)
        for (i in 1 until presents.size) {
            val delta = presents[i] - presents[i - 1]
            if (delta > 0) deltas += delta
        }
        if (deltas.isEmpty()) return null
        val spanNanos = presents.last() - presents.first()
        if (spanNanos <= 0) return null
        return SfLayerLatencyStats(
            refreshPeriodNanos = period,
            frameCount = presents.size,
            averageFps = (presents.size - 1) / (spanNanos / 1_000_000_000.0),
            medianDeltaNanos = percentile(deltas, 0.50),
            p95DeltaNanos = percentile(deltas, 0.95),
            p99DeltaNanos = percentile(deltas, 0.99),
            maxDeltaNanos = deltas.max(),
            medianLatencyNanos = percentile(latencies, 0.50),
            p99LatencyNanos = percentile(latencies, 0.99),
        )
    }

    private val WHITESPACE = Regex("""\s+""")
    private const val MIN_FRAMES = 10

    /** Nearest-rank percentile on a sorted copy; [values] is not mutated. */
    private fun <T : Comparable<T>> percentile(values: List<T>, fraction: Double): T {
        val sorted = values.sorted()
        val index = (fraction * (sorted.size - 1)).toInt().coerceIn(0, sorted.size - 1)
        return sorted[index]
    }
}

/** What one per-layer latency dump measured. All values come from the dump, none inferred. */
data class SfLayerLatencyStats(
    val refreshPeriodNanos: Long,
    val frameCount: Int,
    val averageFps: Double,
    val medianDeltaNanos: Long,
    val p95DeltaNanos: Long,
    val p99DeltaNanos: Long,
    val maxDeltaNanos: Long,
    val medianLatencyNanos: Long,
    val p99LatencyNanos: Long,
) {
    /** True when the game is pacing at half the panel's rate (the common forced 30 fps cap). */
    fun isHalfRefreshPaced(): Boolean {
        val target = refreshPeriodNanos * 2.0
        return abs(medianDeltaNanos - target) / target < 0.05
    }
}
