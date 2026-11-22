package com.gamecore.core.model

import kotlin.math.roundToInt

/**
 * A per-game render-scale preset backed by Android's Game Manager custom mode, and the counterpart
 * to [ResolutionScale] — which this deliberately is not.
 *
 * [ResolutionScale] drives the global `wm size` override: it resizes the *display* for everything on
 * it, and its own documentation records the honest limit — a game is free to keep rendering at its own
 * resolution inside the resized surface. The intervention behind this enum is the platform's per-game
 * WindowManager backbuffer resize (Android Game Mode interventions): the target game's own window
 * buffers become `ratio × window`, verified by measurement, and nothing on the device changes for any
 * other app. Measured on the CODM reference device (720×1600): standard 640×1422, 0.75 → 540×1200,
 * 0.67 → 482×1072, 0.5 → 360×800 — the formula below reproduces all four.
 *
 * [NATIVE] means "no intervention": it maps to `cmd game reset` + standard mode, never to a ratio of
 * 1.0 written into the custom config, because an override that happens to equal 1.0 is still an
 * override a later reader would report as active — the same reasoning that keeps
 * [ShellCommand.ResetDisplaySize] a distinct command from `wm size <the panel size>`.
 *
 * The ratio set is bounded to the platform's documented range (0.3–0.9, help text; the shell itself
 * accepts any float, verified against AOSP `GameManagerShellCommand`, which only runs
 * `Float.parseFloat`). Android's own documentation recommends keeping the resize at 0.70 or above
 * because child processes (toasts, pop-ups) may not resize correctly below it — that guidance is why
 * [EXTREME] exists as an explicit user choice rather than a default, and why its summary says so.
 *
 * Applying any non-null ratio registers the intervention with GameManagerService, but the running
 * game keeps its current buffers until it is next started: the downscale intervention is applied at
 * window creation, which Android's documentation states directly ("The downscaling intervention
 * requires app restart") and which was measured on the reference device — an in-session apply changed
 * nothing; a relaunch changed everything. The controller therefore reports "restart needed" rather
 * than "applied" until the buffers are measured.
 */
enum class GameModeScale(
    val ratio: Float?,
    val label: String,
    val summary: String,
) {

    /** No per-game override. Restores the device default and the game's own buffer choice. */
    NATIVE(
        null,
        "1.0 Native",
        "The game's own resolution. Removes the per-game override.",
    ),

    /**
     * Inside Android's documented ≥0.70 guidance. On the 720×1600 reference device this measured
     * −29% pixels against the game's standard-mode buffers, with UI text still fully legible.
     */
    BALANCED(
        0.75f,
        "0.75 Balanced",
        "A quarter fewer pixels each way. Inside Android's own ≥0.70 quality guidance.",
    ),

    /**
     * Below the documented 0.05-step list, but verified end-to-end on the reference device: the
     * shell accepts any float in range and the buffers came out exactly 482×1072. Named
     * Experimental because it is outside the documented preset list, not because it failed.
     */
    EXPERIMENTAL(
        0.67f,
        "0.67 Experimental",
        "Verified working though outside Android's documented preset list.",
    ),

    /**
     * Half resolution on each axis — quarter the pixel count. The aggressive option: measured
     * clearly blurrier on the reference device, and below the platform's own ≥0.70 recommendation
     * because of the child-process resizing caveat. User-selected, never a default.
     */
    EXTREME(
        0.5f,
        "0.5 Extreme",
        "Quarter the pixels. Clearly blurrier, and below Android's ≥0.70 guidance — your call.",
    ),
    ;

    /**
     * The buffer dimensions this scale produces on a window of [width] × [height], each axis
     * rounded to nearest. Verified against all four measured CODM states; see the class comment.
     */
    fun expectedBuffers(width: Int, height: Int): Pair<Int, Int>? {
        val r = ratio ?: return null
        return (width * r).roundToInt() to (height * r).roundToInt()
    }

    /** The exact string the shell is given for [ratio] — US locale, two decimals, never localized. */
    val ratioArgument: String?
        get() = ratio?.let { String.format(java.util.Locale.US, "%.2f", it) }

    companion object {

        /** Which preset, if any, an applied ratio corresponds to. Null ratios never match. */
        fun ofRatio(ratio: Float?): GameModeScale? =
            ratio?.let { r -> entries.firstOrNull { it.ratio != null && kotlin.math.abs(it.ratio - r) < 0.005f } }
    }
}

/**
 * What buffer verification concluded for one game after a restart. The states are ordered by how
 * much they claim: [APPLIED] claims a measured match, [NOT_APPLIED] claims a measured mismatch
 * (an opted-out game or an OEM variant — the intervention registered but the platform did not
 * resize), [PENDING] claims only that the game has not produced a surface to measure yet, and
 * [UNKNOWN] claims nothing because the evidence could not be parsed.
 */
enum class GameModeScalingVerdict {
    APPLIED,
    NOT_APPLIED,
    PENDING,
    UNKNOWN,
}

/**
 * Whether this device can carry the per-game scaling feature at all, decided from `cmd game help`
 * rather than from an Android version: OEM builds vary, and the help text is the feature contract.
 */
enum class GameModeFeatureSupport { SUPPORTED, UNSUPPORTED, UNDETERMINED }

/**
 * One game's Game Mode state as the device reports it, parsed from
 * `cmd game list-modes` / `list-configs`. [scalingRatio] is null both when no intervention is
 * registered and when the config names one without a scaling ratio; [raw] keeps the distinction
 * checkable (null [scalingRatio] with non-null [rawConfig] means "registered, ratio unparsed").
 */
data class GameModeIntervention(
    val currentMode: String?,
    val availableModes: Set<String>,
    val scalingRatio: Float?,
    val rawConfig: String?,
) {
    val hasIntervention: Boolean get() = rawConfig != null
}
