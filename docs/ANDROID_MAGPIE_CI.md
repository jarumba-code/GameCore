# Android Magpie hosted Android CI

The Android application build runs on a GitHub-hosted `ubuntu-24.04` runner. The Redmi A5 is not used for Gradle or Kotlin compilation.

## Audited project settings

| Setting | Repository value | CI value / reason |
| --- | --- | --- |
| Gradle wrapper | 8.9; committed distribution SHA-256 `d725d707bfabd4dfdc958c624003b3c80accc03f7037b5122c4b1d0ef15cecab` | The workflow invokes `./gradlew`; it does not install a separate Gradle. |
| Android Gradle Plugin | 8.5.2 (`gradle/libs.versions.toml`) | Unchanged. |
| Kotlin | 2.0.21 (`gradle/libs.versions.toml`) | Unchanged. |
| Java | Java/Kotlin bytecode target 17 (`app/build.gradle.kts`) | Temurin JDK 17. |
| compile SDK | 34 | `platforms;android-34`, `build-tools;34.0.0`. |
| target/min SDK | 34 / 26 | Unchanged. API 35 emulator is used for install and instrumentation coverage. |
| NDK | No `ndkVersion` is declared in Gradle. The M3 environment audit records AGP requesting NDK `26.1.10909125`; the custom native build consumes `ANDROID_NDK_HOME`. | Installs and exports exact NDK `26.1.10909125`. |
| CMake | `cmake_minimum_required(VERSION 3.22.1)` in `app/src/main/cpp/CMakeLists.txt` | Installs exact SDK CMake `3.22.1`. |
| Native ABI | The custom build script configures `ANDROID_ABI=arm64-v8a`; the app also packages SQLCipher for ARM64 and ARMv7. | Verifies each Magpie library as ELF64 AArch64 and requires it under `lib/arm64-v8a/` in the APK. |
| Application ID | `com.gamecore`; debug suffix `.debug` | Verifies APK ID `com.gamecore.debug`. |

The NDK version is a CI pin, not a Gradle `ndkVersion` declaration. The codebase currently has a custom CMake invocation from `tools/build-magpie-native-android.sh`, not AGP `externalNativeBuild`.

## Build and test tasks

The workflow runs:

```sh
./gradlew --no-daemon --stacktrace --console=plain \
  :app:testDebugUnitTest \
  :plugin-studio:testDebugUnitTest \
  :plugin-maker:testDebugUnitTest \
  :gamecore-plugin-sdk:test \
  :app:assembleDebug \
  :app:assembleDebugAndroidTest
```

It then verifies the generated `libmagpie_core.so`, `libmagpie_fsr.so`, `libmagpie_vulkan.so`, `libmagpie_gles.so`, and JNI bridge library; verifies the same libraries are packaged in the APK; and checks the APK application ID. An Android 15 ARM64 emulator runs `:app:connectedDebugAndroidTest`, including the native library load and CPU EASU+RCAS instrumentation checks.

The workflow is `.github/workflows/android-magpie.yml`. It saves Gradle console output, test reports, CMake metadata and environment information as `android-magpie-build-diagnostics-<commit>` even when the build fails. A verified debug APK is uploaded as `android-magpie-debug-apk-<commit>` for 14 days. Generated APKs are not committed.

## Interpretation limits

The Android instrumentation test exercises the CPU FSR route and reports Vulkan/GLES probes distinctly from FSR dispatch. Building `libmagpie_vulkan.so` or detecting Vulkan does not prove Vulkan EASU/RCAS execution. The CI emulator is not the Redmi A5; it cannot establish Mali-G57 capability, physical-device performance, thermal behavior, game compatibility, or install behavior on Xiaomi firmware. Those require separate physical-device validation.
