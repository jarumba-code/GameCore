# Android Magpie — M0 repository audit

Audit date: 2026-11-21  
Upstream: `Dreamucxe/GameCore`, commit `1cb00b241e7190a9b6580646f3ba77ac48dafc7c` (GameCore 5.8)  
Working branch: `feature/android-magpie`

## Audit scope and environment

The upstream repository was cloned into this workspace and inspected without replacing its application architecture. The working tree was clean at the start of the audit. No Redmi A5 is attached to this build environment, and the repository is being inspected on an Android/Termux host rather than a conventional desktop Android build host.

The checkout is a sizeable Android application: one main app module plus `gamecore-plugin-sdk`, `plugin-studio`, and `plugin-maker`; 278 unit/instrumentation test source files are present. The project pins AGP 8.5.2, Kotlin 2.0.21, Compose BOM 2024.09.00, Hilt 2.52, Room 2.6.1 and Java 17. The app currently uses `compileSdk = 34`, `targetSdk = 34`, `minSdk = 26`, and packages ARM64 and ARMv7. A target-35/API-35 release therefore requires a deliberate SDK/behavior audit, including Android 14+ MediaProjection requirements already implemented by the upstream service flow.

Build attempt: `./gradlew :app:testDebugUnitTest --no-daemon` could not start because no usable Java runtime is on PATH (`JAVA_HOME is not set and no 'java' command could be found`). `adb` is also unavailable. The filesystem reported only about 1 GB free at audit time. No Gradle tests or APK build have run; no device performance or signing result is claimed. These are environment constraints to resolve before claiming milestone verification.

## Architecture and reusable features

The root Gradle settings explicitly keep most of GameCore in one Android module. The comments explain this was intentional: package boundaries enforce core/data/domain/UI layering while several extra Android/Kotlin compilations exceeded metaspace on the original low-memory ARM build host. Preserve that measured constraint. Begin graphics separation with package/API boundaries inside `:app`; only promote a package to a Gradle module after measuring compilation and memory impact on the intended build host.

Reusable foundations identified in source:

| Magpie need | Existing GameCore foundation | M0 assessment |
|---|---|---|
| Kotlin UI, MVVM, DI, persistence | Compose UI, ViewModels/StateFlows, Hilt, Room | Retain; use established navigation, DI and migration patterns. |
| Device capability reporting | `core/capability/GraphicsCapabilityReader.kt`, capability UI | Already creates a real offscreen EGL context and reports GL strings/limits. Android Java APIs expose Vulkan feature/version/compute tiers, but current source intentionally leaves device properties/extensions restricted because no native Vulkan probe exists. Extend with runtime NDK probing; do not treat feature declarations as successful backend initialization. |
| Graphics settings | `ui/graphics`, `GraphicsViewModel`, profile resolution scale and refresh controls | This is display configuration, not FSR. Preserve it and clearly distinguish display changes from image processing. |
| Game profiles and restoration | `domain/gaming`, `ProfileApplier`, optimization manager, `WriteLedger`, profile editor | Reuse its nullable “leave unchanged” semantics and restoration/ownership rules. Add graphics settings only as app-owned pipeline configuration unless a documented reversible platform setting is involved. |
| Monitoring and reports | `domain/monitoring`, thermal, performance and session systems; observed-value model | Reuse source/precision/unavailable conventions. Do not derive GPU/FPS readings from unsupported interfaces. |
| Elevated operations | optional Shizuku shell and enumerated command path | Reuse only for existing supported actions. Graphics processing must not depend on Shizuku. Do not introduce CPU/GPU clock, governor, voltage or thermal-limit writes. |
| Capture | capture consent activity, capture-purpose and service lifecycle, image extraction/replay paths | Existing capture plumbing and Android 14 one-session rules are valuable. Audit current surface ownership before adding a consumer; one projection session cannot be treated as an unlimited source for independent virtual displays. |
| Plugin system | data-only `.gcplugin`, shared plugin studio | Do not execute shaders/native code from plugins. FSR is compiled, reviewed code behind the app’s JNI boundary. |
| Tests | pure-domain JVM tests and Android instrumentation tests | Extend existing seams. The present environment currently cannot execute them. |

The current Graphics screen explicitly explains that outside controls cannot replace a game’s textures, filtering or shaders. Preserve and refine that truthful boundary as Magpie adds user-owned image pipelines and a possible game integration SDK.

## M1/M2 technical direction

FSR 1’s EASU upsampling and RCAS sharpening should be integrated as two independently testable passes from a pinned upstream source revision. Preserve AMD’s MIT `license.txt` and copyright notice with vendored code and include the license in distributed notices. Validate shader output against a deterministic CPU/reference implementation and approved reference images before interpreting benchmark deltas.

The GPU path must keep source, EASU output and RCAS output GPU-resident. Import or allocate compatible images once, chain passes without CPU readback between them, and read back only for a requested saved image or a correctness comparison. JNI should accept validated dimensions/format and opaque, lifetime-scoped handles to native pipeline resources or Android hardware buffers; avoid copying full frames through Kotlin `ByteArray` on every frame. Ownership, synchronization, cancellation and teardown must be explicit.

Runtime capability is a conjunction of observed facts, not a GPU model string:

- Vulkan: NDK loader and instance/device creation succeed; select a compute-capable queue; query device/API properties; check required storage-image format/usage and shader support; allocate and dispatch a minimal probe; tear it down cleanly. Record timestamp support separately from compute support.
- GLES: create an EGL offscreen context, query actual GL version/extensions, require ES 3.1 compute and the needed image formats, compile/dispatch a minimal shader, validate GL errors and synchronization, then clean up. An ES 2/3.0 context is not a compute backend.
- CPU reference: always available for bounded test images subject to memory/size limits, explicitly identified as CPU processing. Never label it GPU acceleration.
- Backend chooser: return a typed available/unavailable result with reason. A declared Android Vulkan feature or Mali-G57 name is a useful probe hint, not proof of usable FSR dispatch.

The user-requested module names (`graphics-api`, `graphics-core`, `fsr1`, `easu`, `rcas`, `vulkan-backend`, `gles-backend`, `image-pipeline`, `capture-engine`, `display-engine`, `graphics-benchmark`) are retained as logical API/package boundaries initially. Do not add 11 Android compilations to a project whose upstream build documents a memory-related single-module constraint. Candidate future module splits should be measured on a provisioned build machine and may use a pure Kotlin/JVM module for algorithms/contracts if it avoids Android plugin overhead.

## M3-M5 integration boundaries

- Keep per-game profile identity, selection and existing safe apply/restore lifecycle. Add per-game Magpie pipeline configuration separately from device-wide display settings.
- Keep monitoring values sourced. App-level observation of CPU/RAM/thermal does not imply arbitrary-game FPS or GPU time. Use supported frame-timing data only when its source and scope are clear.
- Provide three distinct input modes: app-owned test images; user-consented MediaProjection capture/preview; and explicitly integrated game input/render targets. Do not describe capture mode as changing the game’s presented frames.
- Treat GTA San Andreas as the first named compatibility investigation. Verify its exact Android package/build, renderer and available integration route on device; compatibility cannot be inferred from the title alone. Preserve a multi-game model with per-game tested status and evidence.
- For Android 15 target 35, recheck foreground-service types/permissions and projection consent flows. On Android 14+, each projection session requires fresh consent and a projection token may create only one virtual display. Handle user stop, orientation/size changes, secure content, app death and service cleanup.

## Device validation checklist: Redmi A5

No physical-device values were observed during M0. On the exact Redmi A5 model/firmware, record:

1. Build fingerprint, Android/API level, security patch, model/product identifiers, ABI list, RAM class and free memory under idle and test load.
2. EGL/GL vendor, renderer, version, shading language, extensions, image formats, compute shader support and actual pbuffer shader dispatch result.
3. Vulkan loader, instance/device enumeration, driver/API version, queue families, compute queue, extensions, storage-image format/usage support, dispatch result and timestamp-query availability.
4. Screen physical modes, dimensions, refresh modes and current mode; distinguish public APIs from settings needing special or Shizuku access.
5. Thermal API/sensor availability, baseline and workload thermal status, battery state, app memory and CPU/GPU timing availability with source labels.
6. MediaProjection consent and VirtualDisplay behavior for app-window and whole-display selection, rotation/resize, secure/DRM content, system stop, screen lock and competing projection.
7. Sustained FSR test-image runs at stated resolutions/scales: output correctness, EASU/RCAS timing per available timing mechanism, end-to-end latency, memory deltas, capture overhead and thermal trend. Include warmup, repeat count, spread and device/firmware metadata.
8. GTA San Andreas exact package/version, graphics API/render path where observable, capture visibility/black-frame behavior and any officially supported integration route. Mark untested cases unsupported/unknown rather than compatible.

## Milestone sequence and exit evidence

| Milestone | Deliverable | Exit evidence |
|---|---|---|
| M0 | This repository audit, branch and baseline inventory | Build/test environment documented; no source behavior changed. |
| M1 | Image-only FSR 1 proof of concept, CPU reference and license notices | Deterministic algorithm tests and image-quality comparisons pass on a build host; output format/limits documented. |
| M2 | Runtime Vulkan and GLES compute probes plus GPU-resident backend paths | Unit/instrumentation checks and physical backend dispatch evidence; unavailable paths report reasons. |
| M3 | GameCore integration for profile settings and sourced monitoring | Existing suite/regression checks pass; safe restore behavior verified. |
| M4 | User-consented capture modes and per-game compatibility investigation | Consent/lifecycle tests; explicit separation of test image, capture and game-integrated paths; no arbitrary interception claim. |
| M5 | Unified Magpie app flows and documentation | Full build/test/lint and migration/regression report; licenses and attribution audited. |
| M6 | Redmi A5 physical validation and release signing | Device checklist completed with measured evidence; APK signature verified; signing identity/source provenance documented. |

Each milestone needs its own report with exact commands, environment, pass/fail counts and unrun checks. No performance result, hardware support claim or APK delivery should be stated without corresponding artifacts or device output.

## Risks and open constraints

- No Java runtime, Android SDK/ADB toolchain or attached target device was available to this session at M0; free disk was approximately 1 GB. This prevents test/build/device verification until provisioned.
- Upstream app targets API 34 and uses compile SDK 34. API 35 migration needs behavior and dependency compatibility work; do not raise target SDK without rebuilding and retesting capture, foreground services and permissions.
- GameCore is already large. Module splitting can exhaust the original low-memory build environment; keep package seams first and measure actual build cost.
- A 720p 4 GB phone can be memory- and thermally-constrained. Capture plus conversion plus two GPU passes can cost more than the enhancement saves. Profile each mode and include an opt-out/stop path.
- Ordinary-app MediaProjection is consented capture, not a mechanism to hook another game’s renderer or replace its swapchain. Protected content can be unavailable. True low-resolution rendering requires game-side integration or another supported integration surface.
- Mali-G57, T7250 and Android version do not establish driver correctness, extension/timestamp availability, power cost, or GTA San Andreas compatibility.
- Release signing needs an explicitly managed keystore and reproducible provenance. No private signing key should be committed or invented; no signed APK can be claimed until a real signing key is available and signature verification succeeds.
