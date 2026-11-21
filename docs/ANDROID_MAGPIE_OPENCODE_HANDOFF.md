# Android Magpie handoff

Status date: 2026-11-21. This handoff was prepared from local branch `feature/android-magpie`, based on `322d9cb352d4ea17ec0ea9afe371fa647b3884be`; the handoff commit follows that base.

## Repository and publication status

- Local branch: `feature/android-magpie`.
- Local `origin`: `https://github.com/Dreamucxe/GameCore.git` (upstream; **do not push here**).
- The owner-authorized fork was created as `https://github.com/jarumba-code/GameCore`, with parent `Dreamucxe/GameCore`; the authenticated account has push permission.
- The eight Magpie commits were present locally before this handoff commit. The handoff commit is being published on `feature/android-magpie`; `origin` remains the upstream URL above.
- The workflow exists locally at `.github/workflows/android-magpie.yml`. A workflow run, Android build, and APK are not verified by this handoff.

## Architecture

GameCore remains a Kotlin/Jetpack Compose Android application with its existing modules and features. The Magpie work adds a native C++/CMake proof and JNI bridge. Kotlin owns the UI and Android lifecycle; C++ owns the current FSR CPU reference path and capability probes. The intended product separates companion monitoring, consent-based capture/image testing, and developer-owned renderer integration. It does not inject into unrelated games or claim that capture controls a game's internal render resolution.

## Milestone status

- **M0:** GameCore foundation audit documented (`559c6a4`).
- **M1:** AMD FSR 1.0.2-pinned CPU EASU + RCAS reference proof and standalone fixture harness (`9cc4dfe`). The AMD MIT notice is retained. Checkerboard quality degradation is documented; weak results remain visible.
- **M2:** Standalone Vulkan EASU and RCAS dispatch was tested on the Redmi A5 in the native harness (`d474cd2b49861e99ff67ede8d120bd767fc161d5`). This does not establish Android app/JNI GPU dispatch. GLES capability was probed, but GLES FSR dispatch was not validated. The device and one-shot benchmark details are in `docs/ANDROID_MAGPIE_M2_DEVICE_CAPABILITIES.md` and `docs/ANDROID_MAGPIE_M2_BENCHMARK.md`.
- **M3:** Partial Android integration (`5180b0f27b8dbdbf7b00e386383f0e0325f75432`). The Compose test-image screen calls native CPU EASU + RCAS through JNI. The app-side Vulkan/GLES libraries are capability probes, not GPU upscalers. App build, APK installation, JNI runtime, and app-side Vulkan output remain unverified. The four-library SDK proof (`be9a696`) is not the production FSR GPU engine.
- **CI:** Workflow and follow-up native-test configuration are local commits `88f48b3`, `b5a7929`, and `322d9cb`. GitHub Actions has not been verified to run.

## Build configuration

Audited local project settings: Gradle wrapper 8.9, AGP 8.5.2, Kotlin 2.0.21, JDK target 17, compile/target SDK 34, min SDK 26. The custom native build targets `arm64-v8a`; CMake minimum is 3.22.1. The CI workflow pins Android NDK 26.1.10909125 and CMake 3.22.1. The Android module currently invokes the native build script rather than using AGP `externalNativeBuild`.

## Tests and evidence

- Historical M1 standalone CPU harness: CMake/Ninja build and CTest passed 1/1 on the Termux device, as recorded in `docs/ANDROID_MAGPIE_M1_FSR_POC.md`.
- Historical M2 standalone device validation: six harness checks passed, including actual Vulkan FSR shader dispatch and output fixtures on the Redmi A5. These are not Android app tests. GPU timings, CPU timings, quality, and memory values are reported in `docs/ANDROID_MAGPIE_M2_BENCHMARK.md`; thermal measurements were unavailable.
- Android source currently has 278 app unit-test source files, 25 plugin SDK test files, 11 plugin-studio test files, 1 plugin-maker test file, and 1 app instrumentation test file. These counts do not mean they passed in this checkout.
- No Android Gradle build, APK, install, instrumentation run, or GitHub Actions run is verified.

## Known problems and next engineering task

The fork is available and grants push access. Keep `origin` unchanged and use the separate `magpie` remote for the personal fork:

```sh
git remote add magpie https://github.com/jarumba-code/GameCore.git
git push -u magpie feature/android-magpie
```

Use a normal push only. Never push to `origin` (Dreamucxe upstream).

After publication, the highest-value engineering task is to run and repair the hosted CI workflow, obtain a real APK artifact, and verify all native libraries and CPU JNI execution in an Android test environment. Only then should app-side Vulkan engine lifecycle/resource ownership and true EASU/RCAS dispatch be completed and validated. Do not advertise Vulkan FSR execution from capability detection or library compilation.

## Commands

Standalone FSR 1 CPU/device harness (may require the Android NDK/CMake tools described in its README):

```sh
tools/fsr1-reference/run_device_validation.sh
```

Android app build and tests (not yet verified in this environment):

```sh
./gradlew --no-daemon --stacktrace --console=plain :app:testDebugUnitTest :plugin-studio:testDebugUnitTest :plugin-maker:testDebugUnitTest :gamecore-plugin-sdk:test :app:assembleDebug :app:assembleDebugAndroidTest
./gradlew --no-daemon --stacktrace --console=plain :app:connectedDebugAndroidTest
```

The committed workflow `.github/workflows/android-magpie.yml` is the intended hosted build path; inspect its pinned runner/toolchain and artifact checks before relying on it. No successful command above should be inferred from the existence of its configuration.
