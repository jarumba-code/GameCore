# Android Magpie M2 device capability report

Probe date: 2026-11-21. Probes ran directly on the Redmi A5 Termux ARM64 userspace and linked to Android system Vulkan/EGL/GLES libraries. This verifies the device driver path, but is not yet an APK/JNI integration test.

## Device identification

| Property | Observed value | Source |
|---|---|---|
| Product model | Xiaomi 25028RN03A | Android `getprop ro.product.model` |
| SoC | UMS9230E | Android `getprop ro.soc.model` |
| Android/API | Android 15 / API 35 | Android system properties |
| ABI | arm64-v8a | Termux runtime and ARM64 executable |
| RAM | 3,885,420 kB total; about 1.64 GB available when sampled | `/proc/meminfo` |
| GPU | Mali-G57 | Runtime Vulkan and EGL renderer strings |

## Vulkan

- `dlopen` of `/system/lib64/libvulkan.so` and `vkCreateInstance` succeeded.
- Enumerated GPU: `Mali-G57`, vendor `0x13b5`, device ID `0x90910010`, Vulkan `1.3.278`.
- A queue family advertised graphics, compute and transfer flags (`0x7`), with two queues.
- `shaderStorageImageExtendedFormats` was reported enabled. The backend uses explicitly formatted `VK_FORMAT_R8G8B8A8_UNORM` storage images.
- AMD EASU and RCAS GLSL shader modules compiled to SPIR-V, passed `spirv-val`, created Vulkan compute pipelines, dispatched, and produced output images on the device.
- This is verified compute execution on this specific device/OS image. It does not establish support on other Mali devices or other firmware builds.

## OpenGL ES fallback

- Android EGL 1.5 initialized and created a pbuffer ES context.
- Runtime strings: OpenGL ES `3.2 v1.r49p1-04eac0.c82d7dfafb554e451d70465a9a21fa98`, GLSL ES `3.20`, renderer `Mali-G57`.
- GLES 3.1 core compute shaders and image load/store are therefore available from the context version. The probe confirms version/capability, not actual EASU/RCAS GPU dispatch.
- The two ESSL shader sources compile to OpenGL-target SPIR-V using shaderc and pass the SPIR-V validator. A temporary generated copy of AMD's header gets three `0` literals changed to `0u`, required by the stricter ESSL type rules; vendored upstream files remain unmodified. Driver shader compilation and dispatch remain untested, so GLES is a candidate fallback only.

## Limits of this probe

Thermal sysfs is denied to the shell (`/sys/class/thermal` permission denied), and `dumpsys thermalservice` is blocked without `android.permission.DUMP`. The shell also has no BatteryManager service. Consequently, this run has no temperature/thermal-throttling measurements. The Android app should collect thermal status through supported Android APIs in a later integration/device test. No clock/frequency controls were attempted.

This device was not tested with GTA San Andreas, MediaProjection, an APK, JNI, or an in-game frame path. No arbitrary game output interception is implied.
