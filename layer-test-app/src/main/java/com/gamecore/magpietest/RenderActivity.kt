package com.gamecore.magpietest

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView

/**
 * Renders the deterministic test scene with [RendererNative] on a SurfaceView. Vulkan startup
 * diagnostics remain visible when native initialization fails instead of leaving a silent black screen.
 */
class RenderActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var surfaceView: SurfaceView
    private lateinit var statusView: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        statusView = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 14f
            setPadding(16, 12, 16, 12)
            setBackgroundColor(0xCC101820.toInt())
            text = "Starting Vulkan renderer…"
        }
        val root = FrameLayout(this).apply {
            addView(surfaceView, FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            ))
            addView(statusView, FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP
            ))
        }
        setContentView(root)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val layerEnabled = intent.getBooleanExtra(EXTRA_LAYER, false)
        RendererNative.onSurfaceCreated(surfaceView.holder.surface, layerEnabled, this)
    }

    /** Called synchronously by JNI during Vulkan startup; UI updates are posted to the main thread. */
    fun onNativeStatus(message: String) {
        runOnUiThread {
            statusView.text = message
            statusView.visibility = TextView.VISIBLE
            if (message.startsWith("Baseline renderer ready") ||
                message.startsWith("Layer renderer ready")) {
                statusView.postDelayed({ statusView.visibility = TextView.GONE }, 2500L)
            }
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        // Swapchain recreation is driven by VK_ERROR_OUT_OF_DATE_KHR inside the renderer.
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        RendererNative.onSurfaceDestroyed()
    }

    companion object {
        const val EXTRA_LAYER = "layer"
    }
}

/** Thin JNI surface; the renderer owns everything Vulkan and reports startup diagnostics here. */
object RendererNative {
    init {
        System.loadLibrary("magpie_test_renderer")
    }

    external fun onSurfaceCreated(
        surface: android.view.Surface,
        layerEnabled: Boolean,
        activity: RenderActivity
    )
    external fun onSurfaceDestroyed()
}
