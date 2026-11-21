# Android Magpie: Game Frame Access Research and Implementation Plan

Date: 2026-11-21
Branch: `feature/android-magpie`

## Executive decision

Android Magpie should use a **three-tier integration model**, not promise that one technique can modify every game:

1. **In-app Vulkan test path** — prove the persistent EASU + RCAS GPU engine against Magpie-owned images.
2. **Consent-based MediaProjection path** — support compatible third-party games through a capture → process → overlay pipeline. This changes the displayed image, not the game's internal render resolution.
3. **Game/engine integration SDK** — provide the high-quality path for games whose developers integrate Magpie or expose a supported plugin/mod interface. This is the only general route that reliably exposes pre-present render targets and engine metadata.

Do not build a global Android game hook, process injection mechanism, or anti-cheat workaround. Do not claim arbitrary-game compatibility until a tested, supported path exists.

## Findings from reference projects

### 1. ATP0925/DLSS5-Android-Upscaler

Repository: https://github.com/ATP0925/DLSS5-Android-Upscaler

**Naming correction:** Despite its repository name, its README describes a two-pass Vulkan spatial upscaler inspired by AMD FSR 1 EASU and Qualcomm GSR. The code inspected does not implement NVIDIA DLSS 5 neural rendering.

Its README claims:
- A Vulkan layer intended to intercept presentation.
- A companion app for foreground-app detection and overlays.
- A retail Android limitation: release apps do not expose debug Vulkan layers to ordinary third-party apps; the README says its layer cannot attach to commercial games on stock non-root devices without privileged modification or repackaging.

**Source audit limitation:** The current `src/dlss_layer.cpp` is not evidence of a functioning arbitrary-game upscaler. It obtains `vkQueuePresentKHR`, increments a frame counter, checks a thermal flag, then forwards the original `VkPresentInfoKHR` unchanged. It does not acquire or inspect swapchain images, create a replacement image, record EASU/RCAS dispatches into a command buffer, submit those commands, or replace the presented image. Its `vkCreateDevice` path calls `SpatialUpscalerEngine::Initialize` with `VK_NULL_HANDLE` as the compute queue and does not check the result. Although the engine declares a command-recording API, the inspected layer does not call it.

**Decision:** Use this repository as a study of the proposed layer/companion-app split and its stated Android limitations, not as a proven implementation or a source of production-ready game interception code. Do not copy its claims into Magpie marketing or architecture without independent validation.

### 2. FrankBarretta/LSFG-Android-Application

Repository: https://github.com/FrankBarretta/LSFG-Android-Application

This is the strongest reference for a stock-Android, non-renderer-integrated processing path.

Its documented flow is:
1. User grants MediaProjection consent.
2. A VirtualDisplay sends the captured display stream to an ImageReader.
3. Frames are exposed through AHardwareBuffer.
4. Native code imports the AHardwareBuffer as a VkImage using Android external-memory support.
5. A separate Vulkan session processes the captured stream.
6. Processed output is presented through an overlay surface.

The README documents an AHardwareBuffer/Vulkan bridge, a persistent Vulkan session, lifecycle handling, frame pacing, and a CPU-blit fallback. It reports that end-to-end frame generation is validated on newer Adreno 7xx devices, while older Adreno and most Mali devices may fall back to capture/mirror mode because required Vulkan extensions are unavailable.

Important distinctions:
- This does not obtain the target game's VkDevice or replace its internal renderer.
- It processes the visible captured image and draws a separate overlay.
- It cannot lower the game's internal render resolution.
- Capture may already include the game's HUD/UI; spatial upscaling can therefore affect text and interface elements.
- The project's GPL-3.0 license must be reviewed before reusing code. Studying the architecture does not grant permission to copy code into a differently licensed project.

**Decision:** Adopt the architecture as a reference for an optional capture-and-overlay mode, but implement our own small FSR 1 pipeline and verify AHardwareBuffer support on each target device. Do not import the LSFG proprietary/user-supplied DLL or copy GPL code without a deliberate license decision.

### 3. AMD FSR 1 integration guidance

Official overview: https://gpuopen.com/fidelityfx-superresolution/

FSR 1 is a spatial upscaler consisting of EASU followed by RCAS. AMD recommends integrating it after tone mapping in the game's rendering pipeline. A capture-and-overlay implementation will generally receive a later, already-composited image and therefore cannot reproduce the ideal in-engine integration point.

**Decision:** Keep FSR 1 as the first algorithm. Preserve a developer-owned renderer path that can place EASU/RCAS at an appropriate point before UI composition where possible.

### 4. Actual NVIDIA DLSS 5 is a different technology

Official NVIDIA developer article: https://developer.nvidia.com/blog/whats-new-for-game-developers-dlss-5-with-3d-guided-neural-rendering-nvidia-ace-updates-and-new-rtx-kit-capabilities/

NVIDIA describes DLSS 5 as a developer-integrated neural-rendering stage grounded in the game engine's rendered frame and engine-provided data, including motion vectors and artist-authored scene information. It is not simply a generic image upscaler DLL that can be dropped into any game.

**Decision:** Do not describe Magpie's FSR 1 work as DLSS or neural rendering. Temporal/neural approaches are future research and require richer engine data and substantially different compute budgets.

## Current GameCore baseline

Inspected files on `feature/android-magpie`:
- `GraphicsScreen.kt`: current graphics controls adjust system display size and refresh-rate ceiling. The screen explicitly states that it does not alter a game's internal renderer.
- `FsrNative.kt`: loads the native libraries and exposes CPU bitmap processing and capability text.
- `native_fsr.cpp`: JNI currently locks Android Bitmap pixels and invokes the CPU FSR reference API.
- `native-sdk/proof/src/vulkan_probe.cpp`: probes Vulkan availability and supported formats; its own diagnostic says no FSR dispatch is performed.
- `app/src/main/cpp/CMakeLists.txt`: Android Vulkan/EGL/GLES system libraries are resolved through NDK `find_library`.

The earlier M2 standalone harness did execute AMD EASU and RCAS Vulkan shaders on the Redmi A5 Mali-G57. That is valuable device-level evidence, but it is separate from the Android app/JNI path. Do not conflate the two.

## Implementation roadmap

### M4 — Persistent in-app Vulkan FSR engine (next)

Goal: process a Magpie-owned test image on the Android app's Vulkan device, without MediaProjection or another game's process.

Tasks:
1. Replace the Vulkan capability-only probe with a real engine lifecycle: instance, physical-device selection, logical device, compute queue, command pool/buffers, fences, descriptor resources, shader modules and compute pipelines.
2. Reuse the already validated AMD EASU and RCAS shader sources and the existing M2 fixture harness as the correctness reference.
3. Implement a persistent two-pass path: input image → EASU intermediate → RCAS output. Keep intermediate and output GPU-resident.
4. Add explicit image layout/access barriers, format-feature checks, allocation limits, resize handling, queue-family ownership rules, device-loss handling and deterministic teardown.
5. First feed a Magpie-owned test texture/bitmap into a GPU image. Render/display the output in the existing FSR test UI. CPU readback is permitted for test comparison only, not in the eventual live frame loop.
6. Add Android instrumentation coverage for engine initialization, one successful dispatch, invalid input, resize/recreate, and clean shutdown. Compare output with the existing CPU/reference fixtures using documented tolerances.
7. Report backend state precisely: `runtime_available`, `compute_available`, and `fsr_dispatch_validated` must be separate facts.

Exit criteria:
- APK builds and installs.
- Android instrumentation proves a real EASU and RCAS dispatch from JNI on the app-owned Vulkan device.
- Output pixels are read back in the test only and compared against reference fixtures.
- Repeated dispatch and resize do not leak resources or crash.
- No claim of game compatibility is made yet.

### M5 — Consent-based capture and overlay prototype

Goal: process a visible game capture without modifying the target game's process.

Tasks:
1. Add a foreground MediaProjection service with a clear user consent flow and persistent notification.
2. Capture to ImageReader at a controlled resolution and frame rate.
3. Probe whether the device supports the required AHardwareBuffer usage, format, import, sampled/storage image features and synchronization path.
4. Import compatible AHardwareBuffers into Vulkan and run the same EASU/RCAS engine. If direct import is unsupported, expose a clearly labelled fallback or disable the mode; do not silently claim GPU zero-copy.
5. Present processed frames in a separately managed overlay surface, with correct lifecycle, rotation, orientation, resize, touch behavior, and secure-content handling.
6. Add bounded frame queues, drop stale frames rather than accumulating latency, and track capture/processing/presentation frame times.
7. Keep a side-by-side or toggleable original-vs-processed comparison for validation.

Exit criteria:
- User explicitly starts and stops capture.
- A known test app/game is captured and the processed output is visibly compared with the source.
- Frame pacing, dropped frames, latency, memory and thermal behavior are measured on the Redmi A5.
- UI/HUD blur and overlay interaction limitations are documented.

Limitations:
- This is post-capture display enhancement, not true internal render-resolution scaling.
- It cannot recover depth, motion vectors, object masks or the pre-UI scene from an ordinary screen capture.
- It may add latency and consume extra GPU bandwidth.
- Secure/protected content may be unavailable to capture.

### M6 — Developer-owned renderer integration SDK

Goal: enable games and apps that explicitly integrate Magpie to submit their own render targets.

Tasks:
1. Define a versioned C ABI that describes source/destination images, dimensions, formats, color space, Vulkan device/queue ownership, command buffer integration, image layouts and synchronization responsibilities.
2. Make the caller own its Vulkan device and command buffer; Magpie records work into a documented integration point or offers an explicitly owned backend. Do not assume Vulkan handles from unrelated devices are interchangeable.
3. Expose FSR 1 as the first spatial backend. Keep the API extensible for other backends.
4. Provide a small standalone Vulkan sample first, then consider a Godot/Unity/Unreal adapter only after the core contract is stable.
5. Allow the host engine to choose whether UI is composed before or after upscaling.

Exit criteria:
- A sample application integrates the SDK without Magpie's companion app.
- Resource lifetime, barriers, queue ownership and output correctness are tested.
- API and binary compatibility are versioned.

### M7 — Supported third-party game adapters

Only investigate games with an official plugin/mod API, source availability, or explicit developer permission. A generic Android companion app cannot attach itself to every retail game's renderer. Do not implement process injection, global loader manipulation, anti-cheat bypasses, or game repackaging as a product dependency.

### M8 — Performance, compatibility and product claims

- Build a device capability matrix (GPU, Vulkan extensions, formats, AHardwareBuffer import, thermal state).
- Benchmark 30/60 FPS frame budgets, p50/p95/p99 processing time, queue latency, dropped frames, memory and sustained thermal behavior.
- Distinguish display resolution changes, capture-and-overlay enhancement, and true renderer integration in the UI.
- Keep the Redmi A5 as a low-end compatibility target; do not promise 1080p/30 on this device without measured evidence.

## Immediate next action

Finish and verify the current Android CI build after the Vulkan linker correction. Then implement M4 only. Do not begin MediaProjection or game integration until the app-owned Vulkan EASU + RCAS dispatch is proven in the APK.
