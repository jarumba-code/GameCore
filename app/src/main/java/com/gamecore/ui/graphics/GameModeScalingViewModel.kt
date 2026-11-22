package com.gamecore.ui.graphics

import android.content.Context
import android.content.Intent
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.gamecore.core.common.IoDispatcher
import com.gamecore.core.common.valueOrNull
import com.gamecore.core.model.GameModeFeatureSupport
import com.gamecore.core.model.GameModeIntervention
import com.gamecore.core.model.GameModeScale
import com.gamecore.core.model.GameModeScalingVerdict
import com.gamecore.data.repository.GameProfileRepository
import com.gamecore.data.repository.GameScalingProfile
import com.gamecore.data.repository.GameScalingProfileRepository
import com.gamecore.domain.display.DisplaySizeController
import com.gamecore.domain.gameMode.GameModeApplyOutcome
import com.gamecore.domain.gameMode.GameModeRestoreOutcome
import com.gamecore.domain.gameMode.GameModeScalingController
import com.gamecore.domain.gameMode.GameModeVerifyOutcome
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import javax.inject.Inject

/**
 * The per-game scaling section of the Graphics screen: Android's Game Mode backbuffer-resize
 * intervention, driven per game and verified by measurement.
 *
 * Everything the card shows comes from the device or from the scaling table, never from what was
 * requested. The three-state rule the controller keeps — registered, applied, verified — is what
 * this card narrates: after an apply the card says the game must be closed and reopened (GameCore
 * does not force-stop it), the relaunch is offered only once the game's process is confirmed gone,
 * and the verdict line names what the compositor actually measured, including "not applied" for a
 * game that opts out and "could not be read" when the dump did not parse. A chip that looks
 * selected and a number the device did not produce are the two failures this screen exists to
 * prevent.
 */
@HiltViewModel
class GameModeScalingViewModel @Inject constructor(
    private val controller: GameModeScalingController,
    private val gameProfiles: GameProfileRepository,
    private val scalingProfiles: GameScalingProfileRepository,
    private val displaySize: DisplaySizeController,
    @ApplicationContext private val context: Context,
    @IoDispatcher private val io: CoroutineDispatcher,
) : ViewModel() {

    private val state = MutableStateFlow(GameModeScalingUiState())
    val uiState: StateFlow<GameModeScalingUiState> = state.asStateFlow()

    init {
        refresh()
    }

    /** Reads everything from the device and the tables. All reads; nothing is changed. */
    fun refresh() {
        viewModelScope.launch {
            state.update { it.copy(isLoading = true) }
            val shellReady = controllerFeatureSupport()
            // One snapshot is enough for a picker; the Games screen owns profile editing.
            val games = gameProfiles.profiles.first().filter { it.isEnabled }
            val selected = state.value.selectedPackage ?: games.firstOrNull()?.packageName
            val panel = displaySize.state().valueOrNull?.physical
            val intervention = selected?.let { controller.readIntervention(it).valueOrNull }
            val stored = selected?.let { scalingProfiles.profileFor(it) }
            val running = selected?.let { controller.isGameRunning(it) } ?: false
            state.update {
                it.copy(
                    isLoading = false,
                    shellAvailable = shellReady != GameModeFeatureSupport.UNDETERMINED,
                    support = shellReady,
                    games = games,
                    selectedPackage = selected,
                    selectedScale = stored?.let { row ->
                        GameModeScale.ofRatio(row.ratio.takeIf { r -> r != GameModeScalingController.NATIVE_RATIO_MARKER })
                    } ?: GameModeScale.NATIVE,
                    intervention = intervention,
                    stored = stored,
                    gameRunning = running,
                    panelSize = panel?.let { it.widthPixels to it.heightPixels },
                )
            }
        }
    }

    fun selectGame(packageName: String) {
        state.update { it.copy(selectedPackage = packageName) }
        refresh()
    }

    fun selectScale(scale: GameModeScale) {
        state.update { it.copy(selectedScale = scale) }
    }

    /**
     * Applies the selected scale. The first call that meets an intervention GameCore did not write
     * returns the confirmation flow instead of writing; the card then offers "replace" explicitly.
     */
    fun apply() {
        val pkg = state.value.selectedPackage ?: return
        val scale = state.value.selectedScale ?: return
        viewModelScope.launch {
            state.update { it.copy(isBusy = true, message = null) }
            val outcome = controller.apply(pkg, scale, state.value.replaceConfirmed)
            when (outcome) {
                is GameModeApplyOutcome.ExistingIntervention -> state.update {
                    it.copy(
                        isBusy = false,
                        pendingExistingConfig = outcome.configText,
                        replaceConfirmed = false,
                    )
                }
                is GameModeApplyOutcome.Applied -> state.update {
                    it.copy(
                        isBusy = false,
                        pendingExistingConfig = null,
                        replaceConfirmed = false,
                        message = messageFor(outcome),
                    )
                }
                else -> state.update {
                    it.copy(
                        isBusy = false,
                        pendingExistingConfig = null,
                        replaceConfirmed = false,
                        message = messageFor(outcome),
                    )
                }
            }
            refresh()
        }
    }

    /** Confirmation step for the [GameModeApplyOutcome.ExistingIntervention] flow. */
    fun confirmReplace() {
        state.update { it.copy(replaceConfirmed = true) }
        apply()
    }

    /**
     * Measures the game's buffers against the expected size. Requires the game to be running: the
     * card keeps the button disabled with that reason rather than reporting a hollow verdict.
     */
    fun verifyNow() {
        val pkg = state.value.selectedPackage ?: return
        val panel = state.value.panelSize
        if (panel == null) {
            state.update { it.copy(message = "The panel size could not be read, so there is nothing to verify against.") }
            return
        }
        viewModelScope.launch {
            state.update { it.copy(isBusy = true, message = null) }
            val outcome = controller.verify(pkg, panel.first, panel.second)
            state.update {
                it.copy(
                    isBusy = false,
                    message = when (outcome) {
                        is GameModeVerifyOutcome.Verified -> verdictMessage(outcome)
                        is GameModeVerifyOutcome.NoProfile ->
                            "Apply a scale first — there is no scaling state to verify."
                        is GameModeVerifyOutcome.Failed ->
                            "Verification could not run (${outcome.detail})."
                    },
                )
            }
            refresh()
        }
    }

    fun restore() {
        val pkg = state.value.selectedPackage ?: return
        viewModelScope.launch {
            state.update { it.copy(isBusy = true, message = null) }
            val outcome = controller.restore(pkg)
            state.update {
                it.copy(
                    isBusy = false,
                    message = when (outcome) {
                        is GameModeRestoreOutcome.Restored ->
                            "Restored to the device default." + (outcome.priorInterventionNote?.let { prior ->
                                " The intervention that was here before GameCore's first change was: $prior"
                            } ?: "")
                        is GameModeRestoreOutcome.FeatureUnsupported ->
                            "This build's Game Manager shell cannot carry this feature."
                        is GameModeRestoreOutcome.NeedsShell -> outcome.reason
                        is GameModeRestoreOutcome.Failed ->
                            "The restore failed (${outcome.detail})."
                    },
                )
            }
            refresh()
        }
    }

    /**
     * Relaunches the game once its process is confirmed gone. This is the restart step of the
     * flow — the user closes the game themselves, GameCore verifies closure through the activity
     * dump, and this button starts the game's own launcher intent. GameCore never force-stops.
     */
    fun launchGame() {
        val pkg = state.value.selectedPackage ?: return
        viewModelScope.launch(io) {
            if (controller.isGameRunning(pkg)) {
                state.update {
                    it.copy(message = "The game is still running. Close it first — GameCore does not close games for you.")
                }
                return@launch
            }
            val intent = context.packageManager.getLaunchIntentForPackage(pkg)
            if (intent == null) {
                state.update { it.copy(message = "No launcher activity was found for this game.") }
                return@launch
            }
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(intent)
            // The surface takes a few seconds to appear; the verdict needs the game's own
            // steady-state buffers, so verification stays a user-initiated step.
            state.update { it.copy(message = "Launching. Give the game a moment to settle, then verify.") }
        }
    }

    fun dismissMessage() = state.update { it.copy(message = null) }

    private suspend fun controllerFeatureSupport(): GameModeFeatureSupport = controller.featureSupport()

    private fun messageFor(outcome: GameModeApplyOutcome): String = when (outcome) {
        is GameModeApplyOutcome.Applied ->
            when {
                outcome.scale == GameModeScale.NATIVE ->
                    "Per-game scaling removed. The device is back at its own configuration."
                outcome.restartNeeded ->
                    "Registered for this game. Close and reopen the game for it to take effect — " +
                        "GameCore does not close it for you."
                else ->
                    "Registered for this game. It will take effect the next time the game starts."
            }
        is GameModeApplyOutcome.FeatureUnsupported ->
            "This build's Game Manager shell does not expose the per-game downscale intervention."
        is GameModeApplyOutcome.NeedsShell -> outcome.reason
        is GameModeApplyOutcome.ExistingIntervention ->
            "This device already carries an intervention for this game. Review and confirm to replace it."
        is GameModeApplyOutcome.Failed -> "The change failed (${outcome.detail})."
    }

    private fun verdictMessage(outcome: GameModeVerifyOutcome.Verified): String = when (outcome.verdict) {
        GameModeScalingVerdict.APPLIED ->
            "Verified: the game's buffers measure ${outcome.expected}" +
                (outcome.measured.takeIf { it.isNotEmpty() }?.let { m -> " (all measured: ${m.joinToString { "${it.first}x${it.second}" }})" } ?: "") +
                "."
        GameModeScalingVerdict.NOT_APPLIED ->
            "Measured ${outcome.measured.joinToString { "${it.first}x${it.second}" }}, expected ${outcome.expected}. " +
                "The game may opt out of Game Mode interventions, or this build may not enforce them."
        GameModeScalingVerdict.PENDING ->
            "No measurable surface yet. Wait for the game to settle, then verify again."
        GameModeScalingVerdict.UNKNOWN ->
            "The compositor output could not be parsed — nothing is claimed."
    }
}

/**
 * Everything the per-game scaling card draws. [stored] is GameCore's own record — including the
 * pre-change intervention it preserved — and [intervention] is what the device reports now; the
 * card shows both, because the difference between them is the feature's audit trail.
 */
data class GameModeScalingUiState(
    val isLoading: Boolean = true,
    val isBusy: Boolean = false,
    val shellAvailable: Boolean = false,
    val support: GameModeFeatureSupport = GameModeFeatureSupport.UNDETERMINED,
    val games: List<com.gamecore.core.model.GameProfile> = emptyList(),
    val selectedPackage: String? = null,
    val selectedScale: GameModeScale = GameModeScale.NATIVE,
    val intervention: GameModeIntervention? = null,
    val stored: GameScalingProfile? = null,
    val gameRunning: Boolean = false,
    /** The panel's own pixels, for the expected-size computation. Null when unreadable. */
    val panelSize: Pair<Int, Int>? = null,
    /** The config text of an intervention that needs the user's confirmation to replace. */
    val pendingExistingConfig: String? = null,
    val replaceConfirmed: Boolean = false,
    val message: String? = null,
)
