package com.gamecore.domain.gameMode

import com.gamecore.core.common.DataSource
import com.gamecore.core.common.IoDispatcher
import com.gamecore.core.common.Observed
import com.gamecore.core.common.valueOrNull
import com.gamecore.core.model.GameModeFeatureSupport
import com.gamecore.core.model.GameModeIntervention
import com.gamecore.core.model.GameModeScale
import com.gamecore.core.model.GameModeScalingVerdict
import com.gamecore.core.shizuku.ElevatedShell
import com.gamecore.core.shizuku.ShellCommand
import com.gamecore.core.system.DumpsysParsers
import com.gamecore.core.system.SfDumpParsers
import com.gamecore.data.repository.GameScalingProfile
import com.gamecore.data.repository.GameScalingProfileRepository
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.withContext
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The per-game Android Game Mode backbuffer scaling feature, and the reason it is not
 * [com.gamecore.domain.display.DisplaySizeController]: that controller drives the global `wm size`
 * override, which resizes the display for everything on it and cannot make a game render at a lower
 * resolution inside the resized surface. This controller drives the platform's per-game
 * WindowManager backbuffer-resize intervention (Game Manager custom mode, `cmd game set
 * --downscale`), which resizes one package's own window buffers and nothing else — verified on the
 * CODM reference device, where the intervention produced exactly the buffers the ratio predicts
 * (0.75 → 540×1200, 0.67 → 482×1072, 0.5 → 360×800 on the 720×1600 panel) while every other app and
 * the display stayed where they were.
 *
 * Three facts this class is built to keep separate, because collapsing them is how a scaling
 * feature starts claiming things it did not do:
 *
 *  1. **Registered** — GameManagerService accepted the config (`list-configs` shows it). A command
 *     exit code proves this and nothing more.
 *  2. **Applied** — the game's own buffers now measure `ratio × window`. Only true after the game's
 *     windows are (re)created: the intervention does not reach a running game, which Android's
 *     documentation states ("The downscaling intervention requires app restart") and which the
 *     reference measurement confirmed — an in-session apply changed nothing.
 *  3. **Verified** — [verify] measured the buffers and compared them with the expected size. This
 *     is the only state the UI is allowed to present as "working", and [GameModeScalingVerdict]
 *     carries the four honest outcomes including "not applied" (a game may opt out of the
 *     intervention in its own manifest config) and "unknown" (the dump could not be parsed).
 *
 * Preservation: the device's intervention config is read before the first write for each game, and
 * whatever was there — ours, an OEM's, a user's — is stored on the scaling row. GameCore never
 * writes the `device_config game_overlay` namespace the OEM configs arrive through, and there is no
 * command in [ShellCommand] that could: that absence is the guarantee.
 *
 * Restart policy: GameCore does not force-stop games. When the game is running at apply time the
 * outcome says so and the UI asks the user to close and reopen it; GameCore confirms the process is
 * gone (via the same activity dump the memory-reclaim pass reads) before it offers to relaunch,
 * and relaunching is an ordinary in-process `startActivity` on the game's launcher intent — no
 * shell authority involved.
 */
@Singleton
class GameModeScalingController @Inject constructor(
    private val shell: ElevatedShell,
    private val repository: GameScalingProfileRepository,
    @IoDispatcher private val io: CoroutineDispatcher,
) {

    /**
     * Whether this build's `cmd game` can carry the feature at all, decided from the interface's
     * own help text rather than from an Android version. Cached for the process: a ROM does not
     * change its shell mid-run.
     */
    suspend fun featureSupport(): GameModeFeatureSupport = withContext(io) {
        if (!shell.isAvailable()) return@withContext GameModeFeatureSupport.UNDETERMINED
        val result = shell.execute(ShellCommand.gameModeHelp(), ElevatedShell.LONG_TIMEOUT)
        if (!result.isSuccess) return@withContext GameModeFeatureSupport.UNSUPPORTED
        val help = result.stdout
        val hasListModes = help.contains("list-modes")
        val hasDownscale = help.contains("--downscale")
        if (hasListModes && hasDownscale) {
            GameModeFeatureSupport.SUPPORTED
        } else {
            GameModeFeatureSupport.UNSUPPORTED
        }
    }

    /** One game's Game Mode state as the device reports it right now. */
    suspend fun readIntervention(packageName: String): Observed<GameModeIntervention> =
        withContext(io) {
            if (!shell.isAvailable()) {
                return@withContext Observed.Failed(SHELL_UNAVAILABLE)
            }
            val modesCommand = ShellCommand.gameModeListModes(packageName)
                ?: return@withContext Observed.Failed(NOT_A_PACKAGE)
            val modesResult = shell.execute(modesCommand, ElevatedShell.LONG_TIMEOUT)
            if (!modesResult.isSuccess) {
                return@withContext Observed.Failed(modesResult.failureReason())
            }
            val configsCommand = ShellCommand.gameModeListConfigs(packageName)
                ?: return@withContext Observed.Failed(NOT_A_PACKAGE)
            val configsResult = shell.execute(configsCommand, ElevatedShell.LONG_TIMEOUT)
            val configsText = configsResult.takeIf { it.isSuccess }?.stdout?.trim()
            val parsed = parseIntervention(modesResult.stdout, configsText)
            if (parsed == null) {
                Observed.Failed("The Game Manager output could not be parsed.")
            } else {
                Observed.of(parsed, com.gamecore.core.system.DataSource.DUMPSYS_SHIZUKU)
            }
        }

    /** Whether the game currently has a live process, from the same dump the reclaim pass reads. */
    suspend fun isGameRunning(packageName: String): Boolean = withContext(io) {
        if (!shell.isAvailable()) return@withContext false
        val result = shell.execute(ShellCommand.RunningProcesses, ElevatedShell.LONG_TIMEOUT)
        if (!result.isSuccess) return@withContext false
        DumpsysParsers.parseMainProcessPid(result.stdout, packageName) is Observed.Value
    }

    /**
     * Applies [scale] to one game, or removes the intervention for [GameModeScale.NATIVE].
     *
     * An intervention the device already holds and GameCore has no record of having written is not
     * replaced silently: the outcome asks for confirmation and carries the config text, and the UI
     * re-calls with [replaceExisting] once the user agrees. The pre-write config is stored as the
     * preservation record either way.
     */
    suspend fun apply(
        packageName: String,
        scale: GameModeScale,
        replaceExisting: Boolean = false,
    ): GameModeApplyOutcome = withContext(io) {
        if (!shell.isAvailable()) return@withContext GameModeApplyOutcome.NeedsShell(SHELL_UNAVAILABLE)
        val support = featureSupport()
        if (support != GameModeFeatureSupport.SUPPORTED) {
            return@withContext GameModeApplyOutcome.FeatureUnsupported
        }

        val existingRow = repository.profileFor(packageName)
        val existingIntervention = readIntervention(packageName).valueOrNull()
        val deviceHasIntervention = existingIntervention?.hasIntervention == true
        if (deviceHasIntervention && existingRow == null && !replaceExisting) {
            return@withContext GameModeApplyOutcome.ExistingIntervention(
                existingIntervention.rawConfig ?: "",
            )
        }
        val priorIntervention = existingRow?.priorIntervention
            ?: existingIntervention?.rawConfig?.takeIf { it != null && deviceHasIntervention }

        val command = when (scale) {
            GameModeScale.NATIVE -> ShellCommand.gameModeResetOverrides(packageName)
            else -> ShellCommand.gameModeSetDownscale(packageName, scale.ratio ?: return@withContext
                GameModeApplyOutcome.Failed("${scale.name} has no ratio to write."),
            )
        }
        if (command == null) {
            return@withContext GameModeApplyOutcome.Failed(NOT_A_PACKAGE)
        }
        val result = shell.execute(command, ElevatedShell.LONG_TIMEOUT)
        if (!result.isSuccess) {
            return@withContext GameModeApplyOutcome.Failed(result.failureReason())
        }
        if (scale == GameModeScale.NATIVE) {
            val modeCommand = ShellCommand.gameModeSetStandardMode(packageName)
            if (modeCommand != null) {
                // Not fatal: `reset` already returns the device default on builds where the two
                // coincide; the mode switch is the explicit off-switch for the ones where the
                // custom mode can persist without a config.
                shell.execute(modeCommand, ElevatedShell.LONG_TIMEOUT)
            }
        }

        // Read-back: registration is confirmed from the device, not from the exit code. For a
        // native restore the honest confirmation is the intervention's absence.
        val after = readIntervention(packageName).valueOrNull()
        val confirmed = when (scale) {
            GameModeScale.NATIVE -> after?.hasIntervention != true
            else -> after?.hasIntervention == true
        }

        val gameRunning = isGameRunning(packageName)
        repository.save(
            GameScalingProfile(
                packageName = packageName,
                ratio = scale.ratio ?: NATIVE_RATIO_MARKER,
                updatedAt = System.currentTimeMillis(),
                priorIntervention = priorIntervention,
                // A verdict about the previous ratio is not evidence about this one.
                lastVerdict = null,
                lastBuffers = null,
            ),
        )
        when {
            !confirmed -> GameModeApplyOutcome.Failed(
                "The intervention was sent but the device does not report it registered.",
            )
            scale == GameModeScale.NATIVE -> GameModeApplyOutcome.Applied(
                scale,
                restartNeeded = gameRunning,
                gameRunning = gameRunning,
            )
            else -> GameModeApplyOutcome.Applied(
                scale,
                // A running game keeps its current buffers until its windows are recreated.
                restartNeeded = gameRunning,
                gameRunning = gameRunning,
            )
        }
    }

    /**
     * Measures the game's actual render buffers and compares them with what the stored ratio
     * predicts on a [panelWidth] × [panelHeight] window.
     *
     * Every outcome is persisted on the scaling row, so the UI shows the latest measured truth
     * rather than the last thing that was requested. [GameModeScalingVerdict.PENDING] is the normal
     * state right after a relaunch: the surface appears within seconds but the buffers a load
     * screen allocates are not the game's steady-state ones, so the UI offers re-verify rather
     * than declaring either success or failure.
     */
    suspend fun verify(packageName: String, panelWidth: Int, panelHeight: Int): GameModeVerifyOutcome =
        withContext(io) {
            if (!shell.isAvailable()) return@withContext GameModeVerifyOutcome.Failed(SHELL_UNAVAILABLE)
            val row = repository.profileFor(packageName)
                ?: return@withContext GameModeVerifyOutcome.NoProfile
            val scale = GameModeScale.ofRatio(row.ratio.takeIf { it != NATIVE_RATIO_MARKER })
            val expected = if (scale == null || scale == GameModeScale.NATIVE) {
                null
            } else {
                scale.expectedBuffers(panelWidth, panelHeight)
            }

            if (expected == null) {
                // Native state: the honest verification is the intervention's absence, already
                // recorded at apply time. Nothing buffer-shaped to claim.
                val outcome = GameModeVerifyOutcome.Verified(
                    verdict = GameModeScalingVerdict.APPLIED,
                    expected = null,
                    measured = emptySet(),
                )
                persistVerdict(packageName, row, outcome)
                return@withContext outcome
            }

            val listCommand = ShellCommand.surfaceFlingerLayerList()
            val listResult = shell.execute(listCommand, ElevatedShell.LONG_TIMEOUT)
            if (!listResult.isSuccess) {
                val outcome = GameModeVerifyOutcome.Failed(listResult.failureReason())
                return@withContext outcome
            }
            val layerName = SfDumpParsers.blastLayerNameFor(packageName, listResult.stdout)
                ?: run {
                    val pending = GameModeVerifyOutcome.Verified(
                        verdict = GameModeScalingVerdict.PENDING,
                        expected = "${expected.first}x${expected.second}",
                        measured = emptySet(),
                    )
                    persistVerdict(packageName, row, pending)
                    return@withContext pending
                }
            val latencyCommand = ShellCommand.surfaceFlingerLayerLatency(packageName, layerName)
            if (latencyCommand == null) {
                val outcome = GameModeVerifyOutcome.Failed(
                    "The compositor layer name did not belong to this package.",
                )
                return@withContext outcome
            }

            val dumpCommand = ShellCommand.surfaceFlingerDump()
            val dumpResult = shell.execute(dumpCommand, ElevatedShell.LONG_TIMEOUT)
            if (!dumpResult.isSuccess) {
                return@withContext GameModeVerifyOutcome.Failed(dumpResult.failureReason())
            }
            val measured = SfDumpParsers.bufferSizesFor(packageName, dumpResult.stdout)
            val verdict = when {
                measured.isEmpty() -> GameModeScalingVerdict.PENDING
                (expected.first to expected.second) in measured -> GameModeScalingVerdict.APPLIED
                else -> GameModeScalingVerdict.NOT_APPLIED
            }
            val outcome = GameModeVerifyOutcome.Verified(
                verdict = verdict,
                expected = "${expected.first}x${expected.second}",
                measured = measured,
            )
            persistVerdict(packageName, row, outcome)
            outcome
        }

    /**
     * Removes the intervention and returns the device to its own configuration.
     *
     * The preservation record is reported, not silently discarded: a device that held an OEM or
     * user intervention before GameCore's first write cannot have that exact config re-registered
     * through `cmd game` (the shell exposes only a downscale ratio and an fps value, not the
     * per-mode OEM document), and GameCore never writes `device_config game_overlay` — so the
     * outcome says what was there and what that means, rather than pretending a full restore.
     */
    suspend fun restore(packageName: String): GameModeRestoreOutcome = withContext(io) {
        if (!shell.isAvailable()) return@withContext GameModeRestoreOutcome.NeedsShell(SHELL_UNAVAILABLE)
        val support = featureSupport()
        if (support != GameModeFeatureSupport.SUPPORTED) {
            return@withContext GameModeRestoreOutcome.FeatureUnsupported
        }
        val resetCommand = ShellCommand.gameModeResetOverrides(packageName)
            ?: return@withContext GameModeRestoreOutcome.Failed(NOT_A_PACKAGE)
        val result = shell.execute(resetCommand, ElevatedShell.LONG_TIMEOUT)
        if (!result.isSuccess) {
            return@withContext GameModeRestoreOutcome.Failed(result.failureReason())
        }
        val modeCommand = ShellCommand.gameModeSetStandardMode(packageName)
        if (modeCommand != null) {
            shell.execute(modeCommand, ElevatedShell.LONG_TIMEOUT)
        }
        val after = readIntervention(packageName).valueOrNull()
        val row = repository.profileFor(packageName)
        val priorNote = row?.priorIntervention
        repository.save(
            GameScalingProfile(
                packageName = packageName,
                ratio = NATIVE_RATIO_MARKER,
                updatedAt = System.currentTimeMillis(),
                priorIntervention = priorNote,
                lastVerdict = null,
                lastBuffers = null,
            ),
        )
        if (after?.hasIntervention == true) {
            GameModeRestoreOutcome.Failed(
                "The reset was sent but the device still reports an intervention.",
            )
        } else {
            GameModeRestoreOutcome.Restored(
                // Non-null means the device held someone else's config before GameCore touched it.
                priorNote,
            )
        }
    }

    /** The stored row for one game, for the UI's initial state. */
    suspend fun storedProfile(packageName: String): GameScalingProfile? =
        repository.profileFor(packageName)

    private suspend fun persistVerdict(
        packageName: String,
        row: GameScalingProfile,
        outcome: GameModeVerifyOutcome,
    ) {
        if (outcome !is GameModeVerifyOutcome.Verified) return
        repository.save(
            row.copy(
                lastVerdict = outcome.verdict.name,
                lastBuffers = outcome.measured.joinToString(",") { "${it.first}x${it.second}" }
                    .takeIf { it.isNotEmpty() },
            ),
        )
    }

    private fun parseIntervention(modesText: String, configsText: String?): GameModeIntervention? {
        val mode = CURRENT_MODE.find(modesText)?.groupValues?.get(1)
        val available = AVAILABLE_MODES.find(modesText)?.groupValues?.get(1)
            ?.split(',')
            ?.map { it.trim() }
            ?.filter { it.isNotEmpty() }
            ?.toSet()
            ?: emptySet()
        val mode4Scaling = configsText?.let { text ->
            MODE_ENTRY.findAll(text)
                .firstOrNull { it.groupValues[1].trim() == "4" }
                ?.let { SCALING.find(it.groupValues[2])?.groupValues?.get(1) }
        }?.toFloatOrNull()
        val anyScaling = mode4Scaling
            ?: configsText?.let { SCALING.find(it)?.groupValues?.get(1) }?.toFloatOrNull()
        return GameModeIntervention(
            currentMode = mode,
            availableModes = available,
            scalingRatio = anyScaling,
            rawConfig = configsText?.takeIf { it.isNotEmpty() && !it.contains("No intervention found") },
        )
    }

    companion object {
        /** Stored on the scaling row to mean "no intervention" — [NATIVE]'s persisted form. */
        const val NATIVE_RATIO_MARKER = 1.0f

        private const val SHELL_UNAVAILABLE =
            "This needs the elevated shell (Shizuku). Connect it under Settings first."
        private const val NOT_A_PACKAGE = "The stored package name is not a valid package name."

        private val CURRENT_MODE = Regex("""current mode:\s*(\w+)""")
        private val AVAILABLE_MODES = Regex("""available game modes:\s*\[([^\]]*)]""")
        private val MODE_ENTRY = Regex("""(\d+)=\[([^\]]*)]""")
        private val SCALING = Regex("""Scaling:([0-9.]+)""")
    }
}

/** What [GameModeScalingController.apply] concluded. Registered is never reported as applied. */
sealed class GameModeApplyOutcome {
    /** The build's `cmd game` cannot carry the feature (support check failed). */
    object FeatureUnsupported : GameModeApplyOutcome()

    data class NeedsShell(val reason: String) : GameModeApplyOutcome()

    /**
     * The device held an intervention GameCore has no record of writing. [configText] is what
     * `list-configs` reported; the UI shows it and re-calls with `replaceExisting` on agreement.
     */
    data class ExistingIntervention(val configText: String) : GameModeApplyOutcome()

    /**
     * The device reports the intervention registered (native: absent). [restartNeeded] is true
     * exactly when the game has a live process — the buffers will not change until the user closes
     * and reopens the game, and the UI says that in so many words.
     */
    data class Applied(
        val scale: GameModeScale,
        val restartNeeded: Boolean,
        val gameRunning: Boolean,
    ) : GameModeApplyOutcome()

    data class Failed(val detail: String) : GameModeApplyOutcome()
}

/** What [GameModeScalingController.verify] measured. */
sealed class GameModeVerifyOutcome {
    data class Verified(
        val verdict: GameModeScalingVerdict,
        val expected: String?,
        val measured: Set<Pair<Int, Int>>,
    ) : GameModeVerifyOutcome()

    /** No scaling row for the package: nothing to verify yet. */
    object NoProfile : GameModeVerifyOutcome()

    data class Failed(val detail: String) : GameModeVerifyOutcome()
}

/** What [GameModeScalingController.restore] did. */
sealed class GameModeRestoreOutcome {
    data class Restored(val priorInterventionNote: String?) : GameModeRestoreOutcome()

    object FeatureUnsupported : GameModeRestoreOutcome()

    data class NeedsShell(val reason: String) : GameModeRestoreOutcome()

    data class Failed(val detail: String) : GameModeRestoreOutcome()
}
