# GameCore Rendering and FSR Developer Integration Plan

Status: architecture plan; implementation and device validation are separate gates.

## Product direction

GameCore has two legitimate graphics paths:

1. **GameCore-controlled renderer** — a small Android Vulkan rendering environment owned by GameCore. It renders scene content at a configurable internal resolution, runs FSR 1 EASU + RCAS, then presents at the display resolution. This is the first end-to-end proof that lower internal resolution can reduce the renderer's pixel workload.
2. **Developer integration SDK** — a native SDK and engine adapters that willing game developers embed in their own renderer. The game owns the scene, render scale, render targets, frame graph, and presentation. GameCore supplies the upscaling implementation, quality controls, capability checks, telemetry, and integration guidance.

The existing MediaProjection path remains a separate **screen post-processing experiment**. It receives an already-composited frame and must not be described as a way to lower the game's render resolution or increase its FPS.

## What exists today

- A controlled Vulkan test application and a prototype Vulkan layer that attempts to substitute lower-resolution swapchain images.
- FSR 1 EASU and RCAS shader sources derived from AMD's open-source FSR 1 implementation.
- A native proof SDK with CPU RGBA8 processing and a standalone Vulkan compute engine.
- A MediaProjection experiment that currently uses Bitmap/JNI pixel transfers.

These pieces are prototypes. They do not yet constitute a production developer SDK or prove compatibility with third-party games.

## Architecture choice

### First-party controlled renderer: renderer-owned offscreen targets

The preferred production direction is to move the controlled demo away from swapchain interception:

1. Query physical-device features, limits, format support, and queue capabilities.
2. Create a native-resolution presentation swapchain.
3. Create a lower-resolution offscreen color target at the selected render scale.
4. Render the scene into that offscreen target.
5. Transition the image to the FSR input layout and dispatch EASU.
6. Dispatch RCAS into a native-resolution output image.
7. Transition/copy or composite the output into the acquired presentation image.
8. Submit and present using explicit synchronization and image ownership.
9. Recreate size-dependent resources on surface changes; retain pipelines where compatible.

This makes GameCore the renderer owner and removes the need to pretend an external app's swapchain can be safely rewritten. Keep the existing Vulkan layer as a research prototype and compatibility experiment until it has independent device validation.

### Developer SDK: caller-owned Vulkan device and render graph

The SDK must integrate into the game's existing Vulkan device and command submission. It must not create a second Vulkan instance/device and then require CPU pixel upload/readback for every frame.

The intended API boundary is:

- Caller provides VkDevice, physical-device properties, queue-family information, and the command buffer/render-graph context.
- Caller provides a sampled source image/view in the agreed format and layout.
- SDK records EASU/RCAS commands into a caller-owned command buffer, or exposes a backend callback contract for the engine's render graph.
- Caller retains ownership of queue submission, fences/semaphores, frame pacing, swapchain, and presentation.
- SDK reports required barriers, supported formats, dimensions, quality scale, and GPU timing where timestamp queries are available.
- SDK does not assume ownership of game resources and does not access game memory outside the supplied API objects.

A C ABI should remain stable; a thin C++ wrapper can provide RAII and typed Vulkan handles. Version every public struct with struct_size and every exported entry point with an ABI suffix.

### Engine adapters

Build adapters only after the native Vulkan contract is stable:

- **Unity:** native rendering plugin using Unity's render-thread callback/event mechanism; query the active graphics API and reject unsupported backends cleanly.
- **Unreal Engine:** renderer/RHI integration that inserts FSR passes into the render graph and respects RHI resource lifetime and barriers.
- **Custom Vulkan engines:** direct C/C++ SDK integration using the public API.

An adapter must never claim it can reduce resolution if the engine does not expose control over internal render targets. The game developer must opt in and choose the render scale.

## FSR 1 scope and limits

FSR 1 is a spatial upscaler. It consumes the current anti-aliased color frame; it does not require temporal history or motion vectors. Integrate after tone mapping in perceptual color space. Render noise and other high-frequency effects after upscaling where the engine permits.

FSR 1 can improve the performance/quality trade-off only when the game renders expensive scene work at a lower internal resolution before upscaling. It cannot recover detail that was never present, and the upscaler itself consumes GPU time and bandwidth. Net benefit must be measured per workload and GPU.

AMD's FSR 1 implementation is MIT-licensed. Preserve the copyright and license notice in redistributed source and substantial copies. Do not imply AMD endorsement or certification.

## Milestones and acceptance gates

### M0 — Establish a reliable baseline
- Keep the Vulkan test scene deterministic and visually non-uniform.
- Log the exact failing Vulkan function, symbolic VkResult, selected format, surface extent, render extent, and layer mode.
- Validate the baseline renderer with the layer disabled first.
- Do not proceed to FSR performance claims until the baseline produces frames reliably.

### M1 — First-party renderer proof
- Implement offscreen low-resolution rendering in the controlled app without swapchain interception.
- Demonstrate native-resolution rendering and at least two reduced render scales.
- Demonstrate EASU + RCAS output at native presentation size.
- Verify the output against the AMD FSR 1 CPU reference using non-uniform fixtures and documented tolerances.
- Capture GPU timings for scene rendering and FSR separately.

### M2 — Performance evidence
For each test device, compare native render vs reduced render + FSR under the same scene, thermal state, display mode, and frame cap. Record:
- median, p95 and p99 frame time;
- scene GPU time and FSR GPU time;
- dropped/missed frames and frame pacing;
- memory use, temperature and power where measurable;
- visual quality against the native reference.

A higher FPS claim requires a repeatable improvement in frame time, not just a lower render resolution or a successful shader dispatch.

### M3 — Developer SDK contract
- Replace the standalone-device / RGBA8 upload-readback proof path with caller-owned Vulkan device and command-buffer integration.
- Publish headers, CMake package, Android ABI packaging, sample renderer, integration guide, capability query, and error/fallback contract.
- Include a no-op/native-resolution fallback for unsupported devices and formats.
- Add non-uniform image correctness tests and device matrix results.

### M4 — Engine adapters
- Unity and Unreal adapters with versioned compatibility matrices.
- Sample game projects demonstrating render-scale control and FSR insertion.
- Developer-facing documentation and a reproducible sample APK.

### M5 — External developer pilot
- Recruit willing developers only after M1–M3 pass.
- Provide a sample project and a clear opt-in integration agreement.
- Collect device/engine compatibility data with explicit developer consent.
- Do not advertise arbitrary third-party game support.

## Non-goals

- No injection into unrelated non-debuggable games.
- No anti-cheat bypass, process tampering, or memory modification.
- No claim that MediaProjection post-processing lowers another game's GPU workload.
- No FSR 2/3/4 claims from color-only captured frames.
- No automatic resolution control for games that do not expose a renderer integration point.

## Current immediate next step

Fix and validate the controlled renderer's startup failure. The diagnostic update in this branch adds symbolic Vulkan result logging around render-pass, shader-module, pipeline-layout, graphics-pipeline, command-pool, command-buffer, framebuffer, semaphore, and fence creation. The next test run must identify the first failing operation before changing pipeline architecture.
