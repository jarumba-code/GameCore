# Android Magpie — M1 FSR image proof of concept

Date: 2026-11-21
Branch: `feature/android-magpie`
Upstream AMD FidelityFX-FSR revision: `a21ffb8f6c13233ba336352bdff293894c706575` (`v1.0.2`)
Upstream URL: https://github.com/GPUOpen-Effects/FidelityFX-FSR

## Architecture decision

The requested hybrid design fits the existing GameCore architecture. Keep GameCore's Kotlin/Compose/MVVM/Hilt/Room app as the Android shell for UI, profiles, permissions, capture lifecycle, monitoring and settings. Put the image algorithms, Vulkan/GLES backends, GPU resource ownership, pipeline and graphics benchmarks in a native C++ engine built with CMake/Android NDK. Keep JNI as a narrow control/data boundary: Kotlin owns Android lifecycle and permissions; C++ owns native pipeline resources and processing. Expose opaque, scoped native handles and structured results instead of copying each frame through `ByteArray`.

GameCore intentionally avoids splitting the large Android application into many Gradle modules because of build memory pressure. The native engine can have CMake targets for graphics API, FSR, image pipeline and backends without adding many Android Gradle compilations. This M1 host library is portable C++ and has no Android or JNI dependency yet. JNI and Android NDK packaging are deliberately deferred until the algorithm proof is reviewed.

Screen capture remains a separate Kotlin/Android capability. A captured image can be passed into a Magpie-owned image pipeline for analysis, preview or saving. MediaProjection does not grant access to another game's internal render target and does not change that game's render resolution or presented frames. Game-integrated rendering will require a separate explicit integration surface.

## Implemented

- Pinned the official FSR 1 source revision above. Vendored `ffx_a.h`, `ffx_fsr1.h`, exact upstream commit, and AMD's complete MIT `license.txt` under `third_party/amd-fsr1/`.
- Added separate `easu()` and `rcas()` scalar C++ CPU reference passes under `graphics/fsr1/`, with an `Image` API and basic dimension/scale/sharpness validation.
- EASU uses the FSR 1 edge-direction accumulation, Lanczos-like 12-tap filtering and neighborhood deringing equations. RCAS uses the separate 3×3 contrast-adaptive sharpening pass and configurable sharpness stops. The implementation documents the precision and edge-sampling differences from GPU shader execution.
- Added a standalone CMake/Ninja test and benchmark path in `tools/fsr1-reference/`; it requires no JVM, Android SDK, Vulkan or GLES. PPM input, high-resolution reference and local expected-output snapshot fixtures are deterministic.
- Test checks include EASU then RCAS ordering, dimensions, finite output, uniform-color preservation, scale bound and fixture quality floors. It compares local golden snapshots within one 8-bit quantization step and compares PSNR/global SSIM against each analytic high-resolution target.

## Measurements

Environment was read from the active Termux host: Xiaomi model `25028RN03A`, Android 15/API 35, SoC `UMS9230E`, ARM64, Mali EGL property. This establishes that the CPU benchmark ran on the target handset; it does not establish GPU support or game compatibility.

Build/test commands:

```sh
cmake -S tools/fsr1-reference -B .build/fsr1-reference -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build/fsr1-reference
ctest --test-dir .build/fsr1-reference --output-on-failure
.build/fsr1-reference/fsr1_reference --iterations 16
```

Result: build succeeded; CTest passed (1/1 test). The standalone run completed 16 measured iterations per pass after 3 warmups. Each test input was 160×90 and output/reference was 320×180. Times are arithmetic mean CPU wall time per pass for this small synthetic workload.

| Fixture | EASU CPU ms | RCAS CPU ms | Bilinear PSNR dB | EASU+RCAS PSNR dB | Bilinear global SSIM | EASU+RCAS global SSIM |
|---|---:|---:|---:|---:|---:|---:|
| Diagonal edge | 16.0916 | 9.4366 | 30.2132 | 32.5772 | 0.9970 | 0.9983 |
| Checker pattern | 17.5768 | 9.3698 | 18.7646 | 17.0880 | 0.9545 | 0.9359 |
| Shape edge | 16.3394 | 9.2224 | 25.1353 | 25.5273 | 0.7439 | 0.7766 |

Quality is calculated against mathematically generated sharp high-resolution patterns whose 2×2 box averages create the low-resolution inputs. These fixtures isolate edge behavior and are not game captures. The checker pattern is an adversarial high-frequency case: EASU+RCAS is lower than bilinear on both PSNR (17.0880 vs 18.7646 dB) and global SSIM (0.9359 vs 0.9545). On the diagonal and shape edges the combined pass scores above bilinear. This mixed result is retained as evidence rather than hidden by a single average. PSNR and the global SSIM calculation are comparative fixture metrics, not perceptual-study results.

The CPU timings are far above a real-time budget at 320×180 output. They are evidence the separate passes run and are measurable, not a performance claim for a game or for GPU execution. No thermal delta was measured.

## Implemented versus simulated / not implemented

**Implemented and measured:** CPU scalar EASU and RCAS reference passes; synthetic PPM fixtures; local regression snapshots; quality calculations; CPU wall-time measurements on the target handset; standalone CMake build and CTest.

**Simulated:** test images and their analytic high-resolution references. The `*_golden_fsr1_320x180.ppm` snapshots were created by this Magpie CPU port for regression checking. They are not AMD's official golden images or independent AMD shader output.

**Not implemented or measured:** Vulkan, GLES, GPU-resident buffers, JNI, Android app integration, MediaProjection processing, game framebuffer access, GTA San Andreas, multi-game compatibility, frame-time/FPS impact, GPU execution time, GPU memory, thermal overhead, physical image capture, and signed APK.

This milestone does not assert that the CPU port is bit-identical to AMD GPU shader output. Upstream shader reciprocal approximations are replaced with ordinary IEEE floating-point operations; EASU sampling is explicitly clamp-to-edge; input is RGB only, with no implicit gamma/color-space conversion; RCAS denoise stays disabled. Before GPU backend work, review those algorithm conventions and the golden/quality evidence. Vulkan/GLES integration has not started.

## Environment recovery and cleanup

No JDK, Java/Javac, Android SDK/platform 35, installed Gradle runtime, or ADB was present. The repository Gradle wrapper exists but cannot launch without Java. Termux package metadata offers OpenJDK 21 (105,367,008-byte package; 246,004 KiB installed-size) and Gradle 9.6.0, but no Android SDK package was found. There was about 996 MiB free. Installing a JDK would not supply Android platform/build tools and would consume roughly a quarter of remaining space, so the reliable M1 path uses the already-installed Clang 21.1.8, CMake 4.3.4, Ninja and NDK sysroot. No package installation or deletion of user data, caches or source repositories was performed.

The previous audit's note that no physical target was attached was incomplete: ADB was absent, but this Termux session itself reports the exact Redmi A5 model, API 35 and SoC. This M1 CPU measurement is therefore on the target device. GPU and capture validation remain pending.
