# M2 native FSR build and device test

The current Vulkan executor is a standalone native C++ device harness, not yet the app JNI library. It exists to validate FSR and device compute before app integration. The existing GameCore Kotlin app and Gradle architecture are untouched.

## Reproduce on the Redmi A5 in Termux

Install the local build/test tools (the M2 run used Termux clang 21, CMake 4.3, Ninja 1.13, shaderc 2026.2, SPIR-V Tools 1.4, Vulkan headers 1.4.354, and Mesa EGL/GLES headers):

```sh
pkg install clang cmake ninja shaderc spirv-tools vulkan-headers mesa-dev
cd ~/AndroidMagpie
tools/fsr1-reference/run_device_validation.sh
```

The script builds the preserved CPU reference, runs its automated CTest, compiles and validates upstream AMD EASU/RCAS Vulkan shaders, creates a live EGL/GLES capability context, compiles the GLES shader variants, dispatches the Vulkan image pipeline on all three known fixtures, and enforces GPU output PSNR floors. It uses a private temporary directory under `$PREFIX/tmp` and removes only that directory on completion.

Equivalent CMake steps for optional native targets:

```sh
cmake -S tools/fsr1-reference -B build/magpie-m2 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMAGPIE_BUILD_VULKAN_DEVICE_TEST=ON
cmake --build build/magpie-m2
ctest --test-dir build/magpie-m2 --output-on-failure
```

The optional CMake targets build the Vulkan harness, GLES context probe and shader variants. Running the Vulkan CTest suite requires an accessible Android Vulkan driver; the GLES context test requires Android EGL. An Android NDK build may use the same CMake target with the NDK toolchain and system `vulkan`, `EGL`, and `GLESv2` libraries. The app-level JNI boundary, persistent resource lifecycle, Kotlin capability reporting, and APK packaging are later integration work.

## File roles

- `graphics/fsr1/vulkan/*.comp`: Vulkan GLSL compute shaders directly invoking AMD FSR 1.0.2 `FsrEasuF` and `FsrRcasF`.
- `graphics/fsr1/gles/*.comp`: corresponding ESSL 3.10 shader sources for a potential fallback.
- `tools/fsr1-reference/vulkan_runner.cpp`: C++ Vulkan image allocation, host upload, GPU-resident two-pass dispatch, timestamp queries, final readback and CPU comparison.
- `tools/fsr1-reference/gles_capability_probe.cpp`: EGL runtime version/renderer probe only; it does not execute FSR compute.
- `tools/fsr1-reference/prepare_gles_headers.py`: creates an untracked temporary ESSL-compatible include copy; vendored AMD license/source files are unchanged.
