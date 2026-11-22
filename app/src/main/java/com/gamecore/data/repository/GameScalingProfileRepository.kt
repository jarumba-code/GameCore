package com.gamecore.data.repository

import com.gamecore.core.common.IoDispatcher
import com.gamecore.data.database.GameScalingProfileDao
import com.gamecore.data.database.GameScalingProfileEntity
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.withContext
import javax.inject.Inject
import javax.inject.Singleton

/**
 * One game's per-game Android Game Mode scaling state, as the UI sees it.
 *
 * The Room entity leaks nothing here: `priorIntervention` is already the sanitized shape the
 * controller writes, and the verdict/buffers strings are kept as written because they are evidence
 * snapshots, not query keys. The mapping exists to keep `GameScalingProfileEntity` out of `domain`
 * and `ui`, the same way [GameProfileRepository] keeps `GameProfileEntity` out of them.
 */
data class GameScalingProfile(
    val packageName: String,
    val ratio: Float,
    val updatedAt: Long,
    val priorIntervention: String?,
    val lastVerdict: String?,
    val lastBuffers: String?,
)

/**
 * Persistence for the per-game scaling feature — one row per game, written by
 * `com.gamecore.domain.gameMode.GameModeScalingController` after each apply, verify and restore.
 *
 * The table records what was done and what the device then showed, never a requested value alone:
 * `lastVerdict`/`lastBuffers` are overwritten on every measurement, so a stale "applied" cannot
 * survive a verify that measured something else.
 */
@Singleton
class GameScalingProfileRepository @Inject constructor(
    private val dao: GameScalingProfileDao,
    @IoDispatcher private val io: CoroutineDispatcher,
) {

    val profiles: Flow<List<GameScalingProfile>> =
        dao.observeAll().map { rows -> rows.map(::toModel) }

    suspend fun profileFor(packageName: String): GameScalingProfile? = withContext(io) {
        dao.byPackage(packageName)?.let(::toModel)
    }

    suspend fun save(profile: GameScalingProfile) = withContext(io) {
        dao.upsert(
            GameScalingProfileEntity(
                packageName = profile.packageName,
                ratio = profile.ratio,
                updatedAt = profile.updatedAt,
                priorIntervention = profile.priorIntervention,
                lastVerdict = profile.lastVerdict,
                lastBuffers = profile.lastBuffers,
            ),
        )
    }

    suspend fun delete(packageName: String) = withContext(io) {
        dao.delete(packageName)
    }

    private fun toModel(row: GameScalingProfileEntity): GameScalingProfile =
        GameScalingProfile(
            packageName = row.packageName,
            ratio = row.ratio,
            updatedAt = row.updatedAt,
            priorIntervention = row.priorIntervention,
            lastVerdict = row.lastVerdict,
            lastBuffers = row.lastBuffers,
        )
}
