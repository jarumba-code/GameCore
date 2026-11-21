# Android Magpie Native SDK architecture proposal

**Review status: proposal only.** This document requests review before a production renderer refactor, public ABI freeze, Unity/Unreal adapter, or app-mode merger. The small code under `native-sdk/proof/` is only a shared-library build experiment.

## Three modes with separate owners

| Mode | Owner and data path | Scope | Boundary |
|---|---|---|---|
| Companion | GameCore process; UsageStatsManager, Room profiles, Android services, overlays, public settings APIs | Detect configured foreground games, apply reversible supported settings, show OS-reported metrics | Cannot access another UID's private render target, inject a library into unrelated games, or replace arbitrary frames. |
| Consent capture/test | GameCore process; synthetic image or user-consented MediaProjection owned by its foreground service | Process test images; optionally transform a captured display frame for preview/export where policy allows | Captured output is not the game's internal render target and cannot be fed back into the game. Secure content may be blank. Projection, service and VirtualDisplay lifetimes stay explicit. |
| Developer-integrated rendering | Game process; game owner invokes Magpie from its renderer | Process game-owned input into game-owned output before presentation | Requires game source/plugin integration plus a validated backend/resource/sync contract. Compatibility is per game, engine version and renderer. |

GPU capability is not game compatibility. MediaProjection access is not render-target access. A compiling SDK header or standalone test is not a passing game integration.

## Existing GameCore reuse audit

Keep the current single `:app` Android compilation unless measurements justify extraction. `settings.gradle.kts` records extra Android/Kotlin modules as costly for memory/metaspace on the ARM host. Preserve its package boundaries first.

- **Session manager:** `GameDetector`, `GameWatch`, `GamingCoordinator`, `ProfileApplier`, Room `GameProfileRepository`, session recorders. Detection can be denied/unavailable; retain manual and unsupported states.
- **Performance manager:** `PerformanceMonitor`, `ThermalWatch`, `FrameRateProbe`, session telemetry and observed/unavailable models. Do not infer per-game GPU/FPS from device-wide metrics.
- **Overlay manager:** `OverlayController`, `GamingOverlayService`, permission/lifecycle checks and Compose HUDs. An overlay is UI, not an image replacement compositor.
- **Graphics manager:** `GraphicsCapabilityReader` already creates an EGL context and reports GL strings/limits; extend it with *dispatch readiness* separately from API/version discovery. Keep display size/refresh controls separate from FSR.
- **Reversible tuning:** reuse `OptimizationManager`, `StandardAndroidOptimizer`, `ShizukuOptimizer`, `WriteLedger` and restore flows. No clock, voltage, governor or thermal-limit writes.
- **Capture:** reuse `CaptureConsentActivity`, `ScreenRecordingService`, `ScreenCaptureController`, `CapturePurpose` and current ImageReader/VirtualDisplay plumbing. Keep one projection owner and audit fan-out before creating capture consumers.
- **Plugins:** `.gcplugin` and Plugin Studio are declarative data/UI. Do not load shader/native code from that plugin system; the developer SDK is a separately compiled, reviewed artifact.
- **M3 JNI:** currently processes Android Bitmap input with CPU EASU+RCAS. Vulkan only creates/destroys an instance and counts physical devices; it does not check compute queue/formats, create pipelines or dispatch. No persistent engine handle exists. Bitmap pixel locks avoid a Java `ByteArray`, but float `Image` allocations remain; it is not zero-copy. The screen is a synthetic test, not capture or game integration.

## Reusable M2 engine evidence

Keep `graphics/fsr1/src/{image,easu,rcas}.cpp` as CPU reference/fallback and AMD's pinned revision and MIT license under `third_party/amd-fsr1`.

`tools/fsr1-reference/vulkan_runner.cpp` is the proven GPU reference: Vulkan instance/device/compute queue; RGBA8 images and staging; descriptors, pipelines, barriers, command buffer/fence; distinct EASU and RCAS dispatches; timestamp queries; final readback. The EASU image remains GPU-resident for RCAS. Its destructor cleans the persistent Vulkan base resources. But this is a PPM/CLI benchmark: pipelines/descriptors/images are built per run and it also performs CPU comparison. Extract it behind an agreed interface rather than copying a second engine. M2 GLES evidence is capability/shader compilation only; no GLES FSR dispatch has passed.

## Four shared libraries and ABI

Initial Android ABI is `arm64-v8a` for Redmi A5. Add other ABIs only after a separate build and correctness test.

- `libmagpie_core.so`: versioned API, opaque engine/config lifecycle, statuses/errors, logging callback and backend selection. No Android UI or profile persistence.
- `libmagpie_fsr.so`: CPU reference/fallback and common FSR options, preserving AMD source provenance.
- `libmagpie_vulkan.so`: runtime probes now; real Vulkan compute only after extraction and app validation.
- `libmagpie_gles.so`: independent EGL/GLES probes; real compute remains future work and unavailable until validated.

Use a versioned C ABI at `.so` edges (`extern "C"`, fixed-width values, opaque handles, sized/versioned structs, explicit format/stride, status+reason). Offer a header-only C++ RAII facade over that ABI. Do not pass STL containers/strings, exceptions or allocator-owned objects across library boundaries. Version symbols/SONAME/structs; hide internal symbols; add ABI/API compatibility tests. Core must not depend on Vulkan/EGL. Backend libraries link platform APIs. CPU FSR runs with no GPU driver.

Companion JNI loads libraries in dependency order through a Kotlin repository/engine wrapper. Process off main; validate dimensions, format, stride and overflow; use opaque handles and deterministic close. Convert native exceptions to status. This path handles app-owned test images only.

## Public API and target ownership proposal

The common C++ facade is backend-neutral; backend headers carry Vulkan/GLES types:

```cpp
struct ImageView;       // borrowed, validated extent/format/stride
struct RenderTarget;    // opaque backend target, borrowed by default
struct DeviceContext;   // host device/queue/context and API version
struct SyncContract;    // input-ready/output-ready synchronization
struct UpscaleOptions;  // target extent, sharpness, color/alpha policy
struct Result;          // status, backend and timing availability

Result recordFsrPasses(const DeviceContext&, const RenderTarget& source,
                       RenderTarget& destination, const SyncContract&,
                       const UpscaleOptions&, CommandRecorder&);
```

Prefer **record-only** for game integration: host controls queue submission/presentation and places FSR before present. Initial Vulkan interop requires the same instance/physical device/device, compatible queue family and caller command buffer (or narrowly defined callback). Magpie's own device cannot assume it may use a game's `VkImage`. Initial GLES interop requires the game's current EGL context/share group and render thread. External memory/cross-device import is future work.

- Source/output are borrowed: Magpie never destroys a host image, texture, context or surface. Host keeps resources alive through completion. Retained resources use leases/retirement tokens and release only after associated fence/sync signals.
- Vulkan descriptions include image/view, format, extent, samples, usage, current layout, queue family, ownership and ready semaphore/fence. Record explicit barriers and return output layout/access/stage; never guess layout.
- GLES descriptions include texture name/target/internal format/extent, context/share group and render thread. Host establishes context. Use GLsync or specified same-context order; do not move the context to another thread.
- EASU intermediate is Magpie-owned and GPU resident; RCAS consumes it directly. No readback between passes. Read back only for tests/capture/export/explicit diagnostics.
- Use host allocation hooks (Vulkan allocator/RHI) or bounded scratch allocations with a clear byte budget and retirement rules. Check target extents/transient memory for 4 GB devices; report unavailable or use CPU fallback instead of unpredictable game-memory pressure.
- Surface lifecycle is distinct from render-target lifecycle. Game adapter recreates size-dependent resources on resize/rotation and drains work before target destruction. App Activity/Service lifecycle owns only app test/capture sessions.

## Future game-owned adapters

- **Unity:** native plugin plus C# wrapper and render-thread plugin event/RenderGraph adapter. Use Unity-documented native texture/render-event interfaces and versioned graphics fences. Validate each Unity LTS and renderer in a redistributable sample.
- **Unreal:** engine-version-specific RHI/Render Graph module, enqueue on render/RHI thread, use Unreal pooled textures and RHI fences. Vulkan/GLES remain distinct adapters.
- **General C++:** link public headers and arm64 libraries; game supplies device context and target callbacks; include explicit unavailable behavior and CPU fallback.

Adapters ship in the game's own signed package. Do not execute arbitrary downloaded `.so` or shader code from GameCore's plugin store. No Unity/Unreal/game support claim until its real sample passes output, synchronization and lifecycle tests.

## Capability reporting

Keep three independent results: (1) Android/API/driver discovery, (2) backend readiness (loader/context, queue, formats, limits, shader/pipeline, test dispatch, sync), and (3) game integration compatibility (game/engine/version/renderer/target format/ownership/sync and end-to-end result). Return typed `Available`, `Unavailable(reason)`, `ProbeOnly`, `DispatchValidated(testId, version)`. Mali-G57, Vulkan 1.3 or GLES 3.1 alone does not establish dispatch or game support.

## Incremental gates

1. **S0 review:** approve ABI, ownership/synchronization, supported API/ABIs and sample target. No renderer refactor yet.
2. **S1 build proof:** build/load four arm64 libraries; FSR calls retained CPU reference. Vulkan/GLES proof libs say probe-only/no dispatch. Preserve standalone M2 harness.
3. **S2 API tests:** C/C++ headers, malformed inputs, handle lifecycle, ABI symbols and package contents.
4. **S3 Vulkan extraction:** refactor M2 runner behind the approved API while keeping CLI tests; persistent pipelines/descriptors as practical, bounded scratch resources and fence-safe close; compare AMD fixture output and retain failed quality cases.
5. **S4 app JNI:** integrate through current Graphics ViewModel; test images are app-owned; instrumentation checks output and close/recreate. Claim dispatch only after in-app output validates.
6. **S5 GLES separately:** implement dispatch and validate same fixtures plus thread/context synchronization. Keep unavailable until passed.
7. **S6 capture adapter:** consented projection frames only; reuse current foreground-service projection owner; preview/export only; test revoke/stop/secure content/rotation/memory pressure.
8. **S7 game-owned Vulkan sample:** deterministic image, resource/sync/lifecycle tests, quality threshold and repeatable build. Support claim names exact game/device/backend tuple.
9. **S8 Unity/Unreal sample adapters:** isolated versioned projects, each with automated image/sync/lifecycle validation.
10. **S9 expand game list:** only documented APIs or developer-integrated games; companion profiles work independently.

Record device/build, dimensions/format, upstream revision/shader hash, flags, quality/error and failed fixtures, GPU timestamps separately from CPU wall time, allocation/peak memory, thermal readings and run duration. Keep checker degradation/ringing visible.

## Build environment findings

AGP 8.5.2 marker and artifact are reachable. The earlier plugin-resolution error was incomplete/uncached dependency resolution, not a wrong pin or repository block. A later Gradle run fetched AGP/dependencies, then stopped because no SDK path was configured. Android command-line tools, API 35 and Build Tools 35.0.0 are now under `$HOME/Android/Sdk`. API platform files are architecture-neutral. Google's Linux `adb`, `aapt2`, and `zipalign` are x86-64 ELF and cannot execute in ARM64 Termux. AGP requested NDK 26.1.10909125; Linux NDK host compiler tools are not Android-device binaries. Termux has Android-target Clang/headers but is not configured as AGP's external-native-build toolchain. Thus SDK presence is not APK build evidence; hardware ADB needs a compatible bridge or other host. On a conventional supported build host, keep pinned Gradle/AGP and use API 35/NDK from SDK Manager; do not change versions to bypass this host limitation.
