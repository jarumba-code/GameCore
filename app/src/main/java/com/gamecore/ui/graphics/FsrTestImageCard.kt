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
    var status by remember { mutableStateOf("FSR CPU reference; GPU dispatch is not integrated in this app build.") }
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
            Text(runCatching { FsrNative.capabilities() }.getOrElse { "Native engine unavailable: ${it.message}" })
            Text(status)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                source?.let { Image(it.asImageBitmap(), "Synthetic input", Modifier.weight(1f).height(120.dp)) }
                result?.let { Image(it.asImageBitmap(), "CPU EASU and RCAS output", Modifier.weight(1f).height(120.dp)) }
            }
        }
    }
}
