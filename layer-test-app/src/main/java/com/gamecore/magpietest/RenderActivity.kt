package com.gamecore.magpietest

import android.app.Activity
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager

/**
 * Renders the deterministic test scene with [RendererNative] on a SurfaceView. The layer opt-in
 * happens at Vulkan instance creation, so a mode change always means relaunching this activity.
 */
class RenderActivity : Activity(), SurfaceHolder.Callback2 {
    private lateinit var surfaceView: SurfaceView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        setContentView(surfaceView)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val layerEnabled = intent.getBooleanExtra(EXTRA_LAYER, false)
        RendererNative.onSurfaceCreated(surfaceView.holder.surface, layerEnabled)
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

/** Thin JNI surface; the renderer owns everything Vulkan and logs its own diagnostics. */
object RendererNative {
    init {
        System.loadLibrary("magpie_test_renderer")
    }

    external fun onSurfaceCreated(surface: android.view.Surface, layerEnabled: Boolean)
    external fun onSurfaceDestroyed()
}
