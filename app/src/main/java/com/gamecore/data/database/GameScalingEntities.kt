package com.gamecore.data.database

import androidx.room.Entity
import androidx.room.PrimaryKey

/**
 * One game's per-game Android Game Mode scaling profile — the persisted half of
 * `com.gamecore.domain.gameMode.GameModeScalingController`.
 *
 * A separate table rather than a column on [GameProfileEntity], for the same reason the colour
 * presets are their own table: the scaling state has its own lifecycle (applied → restart needed →
 * verified) and its own restore record, and the game profile's import/export codec should not have
 * to learn it. Keyed by package name because the intervention itself is per-package.
 *
 * [priorIntervention] is the preservation record: the intervention config (`cmd game list-configs`
 * output) that the device held before GameCore's first write for this game, so a user or an OEM
 * that had registered something is remembered and can be told apart from GameCore's own state.
 * Null means "the device had none" — which is also what a first-time user sees.
 *
 * [lastVerdict] and [lastBuffers] hold the latest measured outcome and the buffers it measured
 * (as `widthxheight`), so the UI can show what the device did rather than what was requested.
 * Both are cleared when the ratio changes, because a verdict about the previous ratio is not
 * evidence about the new one.
 */
@Entity(tableName = "game_scaling_profiles")
data class GameScalingProfileEntity(
    @PrimaryKey val packageName: String,
    /** The applied ratio; matches one `GameModeScale` preset. Never written before a `set`. */
    val ratio: Float,
    val updatedAt: Long,
    /** The device's intervention config before GameCore's first write for this game, if any. */
    val priorIntervention: String?,
    /** The latest [com.gamecore.core.model.GameModeScalingVerdict] name, when one was measured. */
    val lastVerdict: String?,
    /** The buffers the verdict measured, as `widthxheight`. Null until a verify ran. */
    val lastBuffers: String?,
)
