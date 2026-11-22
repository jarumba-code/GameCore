package com.gamecore.ui.graphics

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Synthetic checker fixture; this screen does not capture or alter a running game's renderer. */
@Composable
internal fun FsrTestImageCard(modifier: Modifier = Modifier) {
    var source by remember { mutableStateOf<Bitmap?>(null) }
    var result by remember { mutableStateOf<Bitmap?>(null) }
    var status by remember {
        mutableStateOf("FSR CPU reference and app-owned Vulkan engine available below; " +
            "both process this screen's own synthetic image only.")
    }
    var busy by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    androidx.compose.material3.Card(modifier = modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("FSR test image", style = MaterialTheme.typography.titleMedium)
            Text("Synthetic fixture only. This does not intercept game output.")
            Button(enabled = !busy, onClick = {
                busy = true
                scope.launch {
                    val pair = withContext(Dispatchers.Default) {
                        val input = Bitmap.createBitmap(160, 90, Bitmap.Config.ARGB_8888)
                        for (y in 0 until input.height) for (x in 0 until input.width) {
                            val v = if (((x / 8) + (y / 8)) % 2 == 0) 0xffe8e8e8.toInt() else 0xff202020.toInt()
                            input.setPixel(x, y, v)
                        }
                        val output = Bitmap.createBitmap(320, 180, Bitmap.Config.ARGB_8888)
                        val measured = runCatching { FsrNative.processCpu(input, output, 2f) }
                            .getOrElse { "Native CPU processing failed: ${it.message}" }
                        Triple(input, output, measured)
                    }
                    source = pair.first; result = pair.second; status = pair.third
                    busy = false
                }
            }) { Text(if (busy) "Processing…" else "Run CPU EASU + RCAS") }
            VulkanDispatchRow { vulkanStatus -> status = vulkanStatus }
            Text(runCatching { FsrNative.capabilities() }.getOrElse { "Native engine unavailable: ${it.message}" })
            Text(status)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                source?.let { Image(it.asImageBitmap(), "Synthetic input", Modifier.weight(1f).height(120.dp)) }
                result?.let { Image(it.asImageBitmap(), "FSR output", Modifier.weight(1f).height(120.dp)) }
            }
        }
    }
}

/**
 * The M4 Vulkan dispatch: a persistent engine handle held for the card's lifetime, created on
 * first dispatch at the fixture's 160×90 → 320×180 size and destroyed when the card leaves the
 * composition. Every dispatch is a real submit → fence-wait → readback cycle on the app-owned
 * Vulkan device; the status string reports the timestamped pass timings, and says so plainly when
 * the device cannot host the engine. It never touches anything but the bitmaps created here.
 */
@Composable
private fun VulkanDispatchRow(onStatus: (String) -> Unit) {
    val scope = rememberCoroutineScope()
    var busy by remember { mutableStateOf(false) }
    var engineHandle by remember { mutableStateOf(0L) }
    DisposableEffect(Unit) {
        onDispose {
            if (engineHandle != 0L) FsrNative.destroyVulkanFsrEngine(engineHandle)
        }
    }
    Button(enabled = !busy, onClick = {
        busy = true
        scope.launch {
            val outcome = withContext(Dispatchers.Default) {
                runCatching {
                    var handle = engineHandle
                    if (handle == 0L) {
                        handle = FsrNative.createVulkanFsrEngine(160, 90, 320, 180)
                        engineHandle = handle
                    }
                    if (handle == 0L) {
                        return@runCatching "This device could not host the Vulkan FSR engine " +
                            "(no compute-capable Vulkan device, or the format/limits checks failed). " +
                            "No dispatch was attempted and none is claimed."
                    }
                    val input = Bitmap.createBitmap(160, 90, Bitmap.Config.ARGB_8888)
                    for (y in 0 until input.height) for (x in 0 until input.width) {
                        val v = if (((x / 4) + (y / 4)) % 2 == 0) 0xffe8e8e8.toInt() else 0xff202020.toInt()
                        input.setPixel(x, y, v)
                    }
                    val output = Bitmap.createBitmap(320, 180, Bitmap.Config.ARGB_8888)
                    val measured = FsrNative.vulkanFsrProcess(handle, input, output, 0f)
                    val described = FsrNative.describeVulkanFsrEngine(handle)
                    input.recycle()
                    output.recycle()
                    "$measured\n$described"
                }.getOrElse { "Vulkan dispatch failed: ${it.message}" }
            }
            onStatus(outcome)
            busy = false
        }
    }) { Text(if (busy) "Dispatching…" else "Run Vulkan FSR (EASU + RCAS)") }
}
