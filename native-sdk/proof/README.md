# Native SDK shared-library proof

This isolated proof builds four arm64 Android ELF shared libraries with Termux CMake/Ninja/Clang on-device:

- `libmagpie_core.so`: versioned ABI symbol.
- `libmagpie_fsr.so`: invokes retained CPU EASU and RCAS for RGBA8 image views.
- `libmagpie_vulkan.so`: runtime physical-device/compute-queue/RGBA8-format probe only.
- `libmagpie_gles.so`: temporary EGL/GLES context and compute-capability probe only.

**Neither GPU library dispatches FSR.** `fsr_dispatch_validated` stays zero. This is not the production engine lifecycle, APK packaging or game integration. It does not alter the standalone M2 harness. AMD source and MIT notice remain in `third_party/amd-fsr1`.

On the Redmi A5 (Termux ARM64), with CMake, Ninja, Clang, Vulkan/EGL headers and system graphics libraries:

```sh
native-sdk/proof/build_on_device.sh "$PREFIX/tmp/magpie-native-sdk-proof"
```

Output stays outside the checkout. The smoke test loads all four libraries, runs a small CPU FSR image and queries Vulkan/GLES readiness, while verifying neither is marked as FSR dispatch validated. It is not the AMD quality fixture suite; use `tools/fsr1-reference/run_device_validation.sh` for that.

This CMake file uses the Redmi's `/system/lib64` API libraries and is not a desktop cross-build recipe. Conventional app builds should use the pinned Gradle/AGP and SDK-managed NDK. The proof `.so` files depend on `libc++_shared.so`; package one compatible C++ runtime in an APK built with shared STL.

## Redmi A5 run (2026-11-21)

The device build completed and CTest reported `1/1` passed. `file` identified each `.so` as an AArch64 Android ELF targeting API 30, built by the installed Termux NDK r29 toolchain; artifacts are under `$PREFIX/tmp/magpie-native-sdk-proof` and are not committed. The smoke output reported `abi=1`, `cpu_fsr=passed`, Vulkan compute queue + RGBA8 sampled/storage format found, and GLES 3.1 compute context probed. Both backend probes reported `fsr_dispatch=0`. Mali driver sysfs warnings appeared while probing and did not fail the test.

This is evidence that the prototype ABI/shared objects load and the probe paths execute on this Redmi A5. It is not evidence that either shared library dispatches GPU FSR or that the Android app packages/loads these libraries.
