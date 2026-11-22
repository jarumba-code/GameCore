package com.gamecore.data.database

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import kotlinx.coroutines.flow.Flow

/**
 * Per-game Android Game Mode scaling profiles. One row per game at most, keyed by package.
 *
 * Every write is an upsert — the controller rewrites the row on each apply/verify/restore — and
 * the table is deliberately tiny: it is the persistence of a per-game feature state, not a query
 * surface. Everything the UI draws arrives through [observeAll].
 */
@Dao
interface GameScalingProfileDao {

    @Query("SELECT * FROM game_scaling_profiles ORDER BY packageName")
    fun observeAll(): Flow<List<GameScalingProfileEntity>>

    @Query("SELECT * FROM game_scaling_profiles WHERE packageName = :packageName LIMIT 1")
    suspend fun byPackage(packageName: String): GameScalingProfileEntity?

    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun upsert(profile: GameScalingProfileEntity)

    @Query("DELETE FROM game_scaling_profiles WHERE packageName = :packageName")
    suspend fun delete(packageName: String)
}
