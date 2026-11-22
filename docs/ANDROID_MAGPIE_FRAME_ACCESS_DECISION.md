# Android Magpie: frame-access architecture decision

Date: 2026-11-22 · Branch: `feature/android-magpie` · Status: **decided**
This document is the Phase-1 deliverable of the resolution-reduction + FSR pipeline investigation.
It compares every supported Android route for getting a game's frame, states what each can and
cannot reach, and commits Magpie to the architectures this repo actually implements. Measured
evidence comes from the CODM reference device work (Tecno Spark 40, Android 15, MT6769, CODM
1.0.60) and is cited where it decides an answer.

## The question that decides everything

> Can Magpie receive a retail game's render target *before* the compositor enlarges it?

Answer: **No — not for an ordinary retail game, through any documented interface, without the
game's cooperation.** Every route below is judged against that question. Nothing here is
addressed by process injection, anti-cheat bypass, APK repackaging, or undocumented hooks; those
are permanently out of scope for this project.

## The options, compared

### A. Android Game Mode backbuffer downscaling (implemented; shipped)

- Reaches the game's render target? **It changes it** — the platform resizes the game's own window
  buffers to `ratio × window` at next launch (measured: 0.75 → 540×1200, 0.67 → 482×1072,
  0.5 → 360×800 on the 720×1600 panel; in-session application is a verified no-op; restart
  required per official docs and measurement).
- Before or after composition? The resized buffers are what SurfaceFlinger composes — so the game
  genuinely renders fewer pixels. The enlargement that follows is SurfaceFlinger/HWC's fixed
  hardware scaler.
- Can Magpie run EASU/RCAS before presentation? **No.** Magpie receives no pixels through this
  mechanism — it is a configuration channel, not a frame channel. The upscale is done by the
  display hardware inside composition, where no app can insert shader work.
- Retail games? Yes (any game that has not opted out via `game-mode-config`; CODM has not).
- Requirements: Android 13+ GameManagerService builds (verified on this Android 15 MTK device),
  Shizuku shell for `cmd game`, per-game restart.
- Overhead: none at runtime for Magpie; the point is the GPU load reduction.
- Affects game rendering? **Yes — genuinely.** This is the one implemented lever that changes what
  the game itself renders.

### B. In-engine FSR via a supported game SDK/API

- Reaches the original render target? **Yes** — EASU/RCAS run inside the game's own frame, before
  present, exactly the pipeline in this investigation's title. This is what AMD designed FSR for.
- Before composition? Yes.
- Retail games? **No.** It requires the game's developer to integrate it (engine plugin, native
  SDK adapter, or mod interface). No retail CODM build ships it, and no external process can add
  it. This is the repo's documented M6 direction (developer-owned renderer integration), not a
  retail-game capability.
- Verdict: the correct long-term integration; unavailable for retail games today.

### C. Android Vulkan layers

- Reaches the render target? A Vulkan layer loaded into the game's process would see swapchain
  images — but Android's loader restricts implicit layers: retail (non-debuggable) apps load
  layers only from system/vendor driver paths, never from app storage or debuggable-package
  declarations. The externally-known attempts (e.g. the "DLSS5-Android-Upscaler" repository
  examined in ANDROID_MAGPIE_GAME_INTEGRATION_RESEARCH.md) confirm the boundary: its layer
  forwards `vkQueuePresentKHR` untouched and cannot attach to commercial games on stock devices.
- Verdict: not viable on stock devices without privileged modification. Out of scope.

### D. MediaProjection + ImageReader (+ AHardwareBuffer → Vulkan)

- Reaches the render target? **No — and this is the key honest finding.** MediaProjection captures
  the *composited, post-enlargement* output. On a 0.5-downscaled game the capture already contains
  the HWC's ×2 enlargement of the 360×800 buffers; the pre-enlargement detail is gone before any
  pixel reaches Magpie. EASU on such a capture cannot recover what bilinear already discarded; at
  same-size input the engine's EASU is a no-op scale and only RCAS contributes (sharpening the
  displayed image, not restoring resolution).
- Before or after composition? After. Always.
- Retail games? Yes — it captures whatever is on screen, with per-session user consent.
- Requirements: projection consent (its own Activity), Android 14+ requires a
  `mediaProjection`-type foreground service (GameCore already has both: `CaptureGate`,
  `ScreenRecordingService`, `CaptureConsentActivity`), plus the existing feed `ImageReader` path
  in `ScreenCaptureController`.
- Overhead: one RGBA8 surface copy per captured frame plus the processing pass — expect roughly
  one to two display refresh intervals of additional capture-to-display latency, measured, not
  assumed. Memory: one staging image + engine intermediates.
- Affects the game? **No.** It changes only the displayed result Magpie itself produces.
- Verdict: implementable, honest only as a **separate experimental capture-and-process mode**,
  never presented as renderer-integrated FSR. Implemented as such (see below).

### E. Other documented interfaces

- `SurfaceControl` frame callbacks / Choreographer: timestamps, not pixels.
- ANGLE (Game Mode can load ANGLE as the GLES driver): a driver translation, not a per-frame
  hook for third parties.
- Vendor display scalers (MTK PQ paths): undocumented, not publicly accessible; out of scope.
- Perfetto / GPU Inspector: timings and counters, not frame pixels.

## Decision

1. **Retail games (today): Game Mode backbuffer downscaling only — no FSR, honestly labeled.**
   The resolution reduction is real and measured; the upscale is the platform's fixed hardware
   scaler; Magpie's value here is configuration, verification (measured buffers) and benchmarking,
   all of which are implemented. No FSR claim is made on this path.
2. **Magpie-owned frames: the in-app Vulkan EASU+RCAS engine (M4, implemented).** It proves the
   two passes execute on the device's GPU and is the correctness/performance reference. It never
   claims to touch a game.
3. **Experimental capture-and-process mode (MediaProjection, implemented as a clearly separated
   experiment):** captures the displayed output with user consent, processes it on the Vulkan
   engine, and measures the added latency. Documentation and UI state in plain words that this
   processes the displayed capture — the post-enlargement image — and cannot restore detail the
   compositor's bilinear pass already discarded.
4. **Game-developer integration (M6) is the only route to the true pipeline** (game renders low →
   Magpie/engine EASU → RCAS → present) and is out of scope until a cooperating game exists.

## Hard blockers (stated plainly)

- No documented API hands a third-party app another app's pre-composition render target.
- Compositor upscaling happens inside HWC; no public per-layer shader hook exists.
- Vulkan implicit layers cannot load into retail games on stock devices.
- Therefore: the four-stage pipeline in the project title is achievable **only** for games that
  integrate Magpie/FSR themselves. For retail games, "downscaled rendering + platform upscale +
  honest measurement" is the ceiling, and it is implemented.

## What is implemented against this decision (this branch)

| Capability | Where | Status |
|---|---|---|
| Per-game Game Mode downscale (incl. 0.5 aggressive) | `domain/gameMode/GameModeScalingController.kt`, typed `cmd game` commands in `ShellCommand.kt` | Implemented; device-measured earlier on the reference session |
| Buffer verification (registered / applied / verified) | `SfDumpParsers.kt` + controller verify | Implemented; unit tests carry the measured fixtures |
| In-app Vulkan EASU+RCAS engine | `native-sdk/proof/src/vulkan_engine.cpp` | Implemented; CI-compiled; device dispatch validated only where the instrumentation test has run |
| Capture-and-process experiment (post-composition, labeled) | Graphics screen capture experiment card | Implemented as an explicitly separate mode |
| Game-developer integration path | documented (M6) | Not implemented — no cooperating game |
