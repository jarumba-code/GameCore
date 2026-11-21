# Android Magpie M3 integration status

**Status: partial integration, M3 not complete.** This change introduces the Kotlin/JNI boundary and a synthetic-image CPU reference UI. It deliberately does not claim that app-side Vulkan FSR dispatch exists. The validated M2 Vulkan executable remains a standalone native test harness and is not loaded by the Android app.

## Implemented

- `app/src/main/cpp` defines a CMake shared library target linked to GameCore's existing EASU/RCAS CPU reference.
- JNI processes Android `RGBA_8888` bitmaps while their pixel locks are held, respecting input/output stride and avoiding an intermediate Kotlin `ByteArray` frame copy. Working `Image` allocations remain necessary for the retained reference implementation.
- Existing Graphics screen controls are preserved. A separate synthetic checker test-image card displays the input and CPU result. It does not use MediaProjection, capture another app, or change a game's render output.
- The JNI capability string distinguishes a Vulkan loader/physical-device probe from a Vulkan FSR dispatch. GLES dispatch is not implemented or claimed.
- AMD implementation sources and their MIT notice remain under `third_party/amd-fsr1`; Android CMake compiles GameCore's existing CPU reference.

## Validation performed

- JNI C++ syntax check on the Redmi A5 Termux Clang toolchain: passed, with two Vulkan aggregate-initializer warnings.
- Existing standalone M2 CMake/Ninja suite rebuilt on Redmi A5 and passed: 6/6, including CPU fixtures, three Vulkan GPU image fixtures and quality floors, and GLES runtime capability probe. These are M2 harness results, not Android app/JNI benchmark results.
- Android Gradle configuration did not reach project task execution: AGP 8.5.2 plugin resolution failed. Android SDK/NDK toolchain is not configured. The Android app, unit/instrumentation tests and APK therefore remain unbuilt.
- No M3 JNI output, app GPU timing, app memory overhead or app thermal measurement is claimed. No M3 physical app test was possible.

## Outstanding M3 work

1. Resolve Gradle's Android plugin artifacts and install/configure Android SDK platform/build tools plus NDK.
2. Promote the existing Vulkan harness's two AMD FSR shader passes into a reusable lifecycle-managed native engine (persistent instance/device/queue/pipelines, scoped image and staging allocations, barriers/fences and safe teardown). Keep standalone harness and CPU reference intact.
3. Load pinned shader artifacts through Android assets or reproducible build-time shader compilation, then validate app JNI output against AMD shader reference fixtures on-device. Do not report acceleration until both passes execute and output validation passes.
4. Add Android instrumentation tests for bitmap formats, stride/dimensions, failure cleanup, lifecycle close/recreate and CPU output. Add Vulkan device tests on target.
5. Test on Redmi A5 with separate CPU pass timings and Vulkan timestamp-query timings, memory accounting, and thermal observations over a controlled run.

## Quality limitations carried forward

M1's high-frequency checker can score below bilinear because its sharp alternating pattern is undersampled at low input resolution and the comparison metric is global: local edge reconstruction cannot recover lost phase/detail and ringing overshoot is penalized against the analytic target. FSR 1 EASU is spatial upscaling, not temporal reconstruction or guaranteed checkerboard recovery. RCAS intentionally sharpens; stronger settings can produce edge halos/ringing. Existing checker results and fixture-specific floors are retained and were not suppressed. See `tools/fsr1-reference/README.md` for fixture and score definitions. The new UI checker is a demonstration image, not an AMD-published expected-output fixture.

## Reproduction

Standalone target-device validation remains available as documented in `tools/fsr1-reference/README.md` and `tools/fsr1-reference/run_device_validation.sh`. Android integration build, once SDK/NDK and AGP artifacts are available:

```sh
./gradlew :app:assembleDebug
./gradlew :app:testDebugUnitTest :app:connectedDebugAndroidTest
```

These commands have not passed in this environment and are not verification claims.

## Environment recheck and SDK architecture review

After the original M3 status, Gradle's `--info` trace successfully resolved AGP 8.5.2 and downloaded its plugin dependencies; the failure was cold/incomplete dependency resolution followed by the definite missing-SDK-path failure. The Android command-line package, platform API 35, and Build Tools 35.0.0 have since been installed locally under `$HOME/Android/Sdk`. This ARM64 host cannot execute the downloaded Linux x86-64 `aapt2`, `zipalign`, or `adb`; AGP also requested desktop-host NDK r26.1.10909125. This does not establish an Android APK build path.

The companion/capture/developer integration separation and native SDK proposal are in [ANDROID_MAGPIE_NATIVE_SDK_ARCHITECTURE.md](ANDROID_MAGPIE_NATIVE_SDK_ARCHITECTURE.md). An isolated arm64 shared-library proof is in `native-sdk/proof`; it does not change this M3 CPU-only JNI state or claim FSR GPU dispatch.

## Native SDK proof addendum (2026-11-21)

The separate `native-sdk/proof` experiment now produces `libmagpie_core.so`, `libmagpie_fsr.so`, `libmagpie_vulkan.so` and `libmagpie_gles.so` as AArch64 Android ELF on the Redmi A5 Termux host. Its smoke test loads them, invokes CPU EASU+RCAS, probes Vulkan compute/RGBA8 support and creates a GLES 3.1 context; CTest passed 1/1. Both GPU libraries report `fsr_dispatch_validated=0`. This is not the production Vulkan engine, app packaging or M3 completion. See [native SDK architecture proposal](ANDROID_MAGPIE_NATIVE_SDK_ARCHITECTURE.md) and [build proof](../native-sdk/proof/README.md).
