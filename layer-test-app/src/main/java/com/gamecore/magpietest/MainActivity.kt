package com.gamecore.magpietest

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

/**
 * Launcher: picks the test configuration. The render activity and the layer stay strictly
 * separated from the main GameCore app; this prototype exists to prove the pipeline end to end.
 */
class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (resources.displayMetrics.density * 16).toInt()
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }
        val title = TextView(this).apply {
            text = "Magpie layer test\n\nA: native render, no layer (baseline)\n" +
                "B: reduced render + linear upscale (layer on, setprop debug.magpie.fsr 0)\n" +
                "C: reduced render + FSR EASU+RCAS (layer on, default)\n\n" +
                "Diagnostics: `adb logcat -s MagpieTest MagpieLayer`"
        }
        layout.addView(title)
        val native = Button(this).apply {
            text = "A — Native render (no layer)"
            setOnClickListener {
                startActivity(Intent(this@MainActivity, RenderActivity::class.java)
                    .putExtra(RenderActivity.EXTRA_LAYER, false))
            }
        }
        val fsr = Button(this).apply {
            text = "B/C — Reduced render through the layer"
            setOnClickListener {
                startActivity(Intent(this@MainActivity, RenderActivity::class.java)
                    .putExtra(RenderActivity.EXTRA_LAYER, true))
            }
        }
        layout.addView(native, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT))
        layout.addView(fsr, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT))
        setContentView(layout)
    }
}
