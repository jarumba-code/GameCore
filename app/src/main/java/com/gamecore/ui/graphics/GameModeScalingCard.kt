package com.gamecore.ui.graphics

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.gamecore.core.model.GameModeFeatureSupport
import com.gamecore.core.model.GameModeScale
import com.gamecore.ui.components.ChoiceRow
import com.gamecore.ui.components.NoteBanner
import com.gamecore.ui.components.SectionCard
import com.gamecore.ui.components.Tone

/**
 * The per-game Android Game Mode scaling card — the counterpart to [ResolutionCard] above it, and
 * a different lever. That card resizes the display for everything; this one registers Android's
 * WindowManager backbuffer-resize intervention for one game, which is what actually changes the
 * resolution the game renders into, and which the platform only applies when the game's windows
 * are next created.
 *
 * The card's copy carries the three facts the controller keeps separate: registered (the device
 * accepted the config), applied (needs the game restarted — by the user; GameCore does not close
 * games), and verified (the compositor measured the buffers). The verdict line names the measured
 * sizes, including a mismatch, because "0.5 selected" is not evidence and never will be.
 */
@Composable
fun GameModeScalingCard(
    modifier: Modifier = Modifier,
    viewModel: GameModeScalingViewModel = hiltViewModel(),
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    val busy = state.isBusy || state.isLoading

    SectionCard(
        title = "Per-game scaling (Android Game Mode)",
        subtitle = "Resizes one game's own render buffers — not the display",
        modifier = modifier,
    ) {
        if (!state.shellAvailable) {
            Text(
                text = "This needs the elevated shell (Shizuku). Connect it under Settings first.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            return@SectionCard
        }
        if (state.support == GameModeFeatureSupport.UNSUPPORTED) {
            Text(
                text = "This ROM's Game Manager shell does not expose the per-game downscale " +
                    "intervention, so there is nothing here to drive.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            return@SectionCard
        }

        if (state.games.isEmpty()) {
            Text(
                text = "Create a game profile first — scaling is configured per game, from the " +
                    "profiles the Games screen holds.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            return@SectionCard
        }

        Text(
            text = "Game",
            style = MaterialTheme.typography.labelLarge,
        )
        ChoiceRow(
            options = state.games.take(GAME_PICKER_LIMIT).map { it.packageName },
            selected = state.selectedPackage,
            onSelect = viewModel::selectGame,
            label = { pkg -> state.games.firstOrNull { it.packageName == pkg }?.label ?: pkg },
            enabled = !busy,
            perRow = 2,
        )

        Text(
            text = "Scale (takes effect when the game next starts)",
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.padding(top = 8.dp),
        )
        ChoiceRow(
            options = SCALE_OPTIONS,
            selected = state.selectedScale,
            onSelect = viewModel::selectScale,
            label = { it.label },
            enabled = !busy,
            perRow = 2,
        )

        if (state.selectedScale == GameModeScale.EXTREME) {
            NoteBanner(
                text = "0.5 is the aggressive option: a quarter of the pixels, clearly blurrier, " +
                    "and below Android's own guidance to keep the resize at 0.70 or above because " +
                    "child processes such as toasts may not resize correctly below it.",
                tone = Tone.Warning,
                icon = Icons.Filled.Info,
            )
        }

        state.pendingExistingConfig?.let { config ->
            NoteBanner(
                text = "This device already carries an intervention for this game:\n$config\n" +
                    "Replace it? The existing config is kept in the scaling record and reported " +
                    "on restore.",
                tone = Tone.Warning,
                icon = Icons.Filled.Info,
                action = {
                    Button(onClick = viewModel::confirmReplace, enabled = !busy) {
                        Text("Replace it")
                    }
                },
            )
        }

        Row(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            modifier = Modifier.fillMaxWidth(),
        ) {
            Button(onClick = viewModel::apply, enabled = !busy && state.selectedPackage != null) {
                Text("Apply")
            }
            OutlinedButton(
                onClick = viewModel::verifyNow,
                enabled = !busy && state.gameRunning,
            ) {
                Text("Verify buffers")
            }
        }
        if (!state.gameRunning) {
            Text(
                text = "Verification reads the game's live buffers, so the game needs to be " +
                    "running for it to mean anything.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        state.stored?.let { stored ->
            val verdict = stored.lastVerdict
            val buffers = stored.lastBuffers
            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(
                    text = when (verdict) {
                        "APPLIED" -> "Verified: buffers measure ${buffers ?: "?"} — the scale is live."
                        "NOT_APPLIED" -> "Measured $buffers — not the scaled size. See the verdict note after verifying."
                        "PENDING" -> "Not measured yet — start the game, let it settle, then verify."
                        "UNKNOWN" -> "The last verify could not read the buffers."
                        else -> "No buffer measurement yet."
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                stored.priorIntervention?.let { prior ->
                    Text(
                        text = "Preserved from before GameCore's first change: ${prior.take(160)}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }

        OutlinedButton(onClick = viewModel::restore, enabled = !busy && state.selectedPackage != null) {
            Text("Restore device default")
        }

        state.message?.let { message ->
            Text(
                text = message,
                style = MaterialTheme.typography.bodyMedium,
            )
        }
    }
}

/** The presets the card offers, in display order. [GameModeScale.NATIVE] is the off state. */
private val SCALE_OPTIONS: List<GameModeScale> = listOf(
    GameModeScale.NATIVE,
    GameModeScale.BALANCED,
    GameModeScale.EXPERIMENTAL,
    GameModeScale.EXTREME,
)

/** The picker shows the first few profiles; the Games screen owns profile management. */
private const val GAME_PICKER_LIMIT = 6
