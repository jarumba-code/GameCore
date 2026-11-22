# Android Magpie — Vulkan layer audit (pre-integration, per the nine criteria)
Date: 2026-11-22 · File audited: `vulkan-layer/src/magpie_layer.cpp` (≈1,250 lines)
Scope: correctness audit of the rewritten layer against the nine criteria from the project brief,
performed before the controlled test app was written. One critical defect found and fixed; the
rest verified sound. This document is the audit record; evidence labels in the final report
inherit from it (the layer is IMPLEMENTED, CI-compiled pending, device-unverified).

## 1. Engine initialization failure must not leave partially initialized swapchain state — VERIFIED, with one fix
- `initEngine()` failure latches `engineFailed` and the mode ladder settles to BLIT before any
  swapchain resource is created (the mode decision now runs `initEngine` first).
- `makeSized` failures (native-size images) → mode degrades to BLIT; partially created handles
  are null-tolerant (`SizedImage` members are individually null-checked in `destroySubstitution`)
  and are cleaned at swapchain destroy. No dangling partial state reaches the app: the app's
  handle is only returned after the ladder settles, and `vkGetSwapchainImagesKHR` serves the mode
  the ladder actually chose.
- Semaphore-creation failure at any non-PASS mode → full L3: substitution destroyed (including
  any fake/native images), owner-map entry erased, REAL swapchain destroyed and re-handed to the
  app untouched, mode recorded as PASS. The app sees an ordinary native swapchain.

## 2. Every fallback mode has matching semaphore/resource configuration — FIXED + VERIFIED
- DEFECT FOUND: present semaphores were created only inside the FSR branch, so the BLIT fallback
  (and the FSR→BLIT degrade at present time) had no layer semaphores for the semaphore chain.
  FIXED: semaphores are now created for the FINAL mode (FSR or BLIT, `mode != PASS`), after all
  ladder degrades, with failure degrading to L3 passthrough.
- Resources: FSR needs fake views (SAMPLED), native-size middle/target, descriptor sets; BLIT
  needs no views/descriptors and uses TRANSFER_SRC on the fake images. The fake-image usage
  flags are chosen per the final mode (SAMPLED for FSR, TRANSFER_SRC for BLIT) — consistent.

## 3. Semaphores are not reused before their previous operations complete — VERIFIED
- One binary semaphore per swapchain image, signalled by the layer's command buffer and waited
  by vkQueuePresentKHR. Index `i` can be re-acquired only after its presentation completes (WSI
  acquire semantics), so the re-signal always follows the previous wait.
- The app's own render-signal semaphore is waited by the layer's command buffer before the layer
  signals its own — so the app's re-signal next flight is always preceded by a wait.

## 4. Application image ownership vs layer-owned image ownership — VERIFIED
- Real swapchain images: owned by the presentation swapchain (created by the layer, destroyed
  with `vkDestroySwapchainKHR`); never hold device memory of their own.
- Fake low-resolution images, middle and target: layer-owned (created with `vkCreateImage`,
  bound to layer-allocated memory, destroyed in `destroySubstitution`).
- The app only ever sees fake handles from `vkGetSwapchainImagesKHR` when `mode != PASS`; in L3
  it sees the real images and the layer never touches them. Index mapping: `vkAcquireNextImageKHR`
  passes through to the real swapchain, and the app renders into `fake[index]` — the counts are
  identical by construction.

## 5. vkGetSwapchainImagesKHR returns the correct count and handles — VERIFIED, plus one fix
- Count query returns `imageCount` (the real chain's); handle query clamps and returns the fake
  handles. L3 passthrough returns the real images directly (a substitution can exist in PASS
  mode after a late-ladder degrade; that case now serves real images).

## 6. vkQueuePresentKHR waits for the final processing semaphore — VERIFIED
- `pWaitSemaphores` is rewritten to the layer's per-image semaphore; the layer's command buffer
  waits the app's original render-signal semaphore(s), so the dependency chain is
  app render → Sapp → layer CB → Sl[i] → present. `pWaitDstStageMask = NULL` (all-commands wait)
  is the legal form. Multi-swapchain presents are passed through untouched rather than guessed.

## 7. The real swapchain image is transitioned to PRESENT_SRC_KHR — VERIFIED
- The command buffer ends with the real image in TRANSFER_DST (after the FSR CopyImage or the
  blit) and records TRANSFER_DST → PRESENT_SRC_KHR with `dstAccessMask = 0` before presenting.
- The UNDEFINED→TRANSFER_DST barrier at the head of the real-image path is correct from any
  actual layout (contents are fully overwritten; UNDEFINED in an image barrier means the previous
  layout is unspecified), which also covers the first frame and post-recreation frames.

## 8. Destruction and recreation cannot leave stale engine resources — VERIFIED
- Per swapchain: `DestroySwapchainKHR` destroys fake/middle/target images+memory+views, the
  descriptor pool, and the present semaphores, erases the substitution and the owner-map entry,
  then calls the next `vkDestroySwapchainKHR`.
- Per device: `DestroyDevice` destroys all remaining substitutions, the command pool, query
  pool, sampler, pipelines, shader modules and layouts, erases the device from the global map,
  deletes the DeviceData, then calls the next `vkDestroyDevice`.
- Recreation: each `CreateSwapchainKHR` builds a fresh `Substitution`; the app's
  `oldSwapchain` handle is destroyed through the normal path, which cleans its own substitution.

## 9. Failure during EASU/RCAS must not present an unfinished output image — VERIFIED (with the honest semantics stated)
- All EASU/RCAS failure modes are at RECORD time or SUBMIT time; in both cases the layer's
  command buffer never executes, the real image still holds the *previous complete frame* in
  PRESENT_SRC, and the passthrough presents that stale-but-complete image with the app's own
  semaphores. GPU-side dispatch failure is only observable as device loss, at which point every
  Vulkan operation is invalid by definition.
- The latch semantics: the first record/submit failure permanently degrades the device to BLIT
  (or PASS if even the blit path cannot be set up), with the reason logged — the layer never
  retries a failed pipeline every frame and never submits a partially recorded buffer.

## Critical defect fixed during this audit (would have failed validation on device)
- The fake-image barriers used `oldLayout = COLOR_ATTACHMENT_OPTIMAL`, but the app's render pass
  ends with `finalLayout = PRESENT_SRC_KHR` — at present time the fake images are in PRESENT_SRC.
  Fixed: both FSR and BLIT fake-image barriers now transition from `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`
  with `srcAccessMask = 0` (required for a source layout of PRESENT_SRC) and
  `srcStage = TOP_OF_PIPE` (the app's color-write ordering is already guaranteed by the semaphore
  chain). Transitioning away from PRESENT_SRC between acquire and present is explicitly
  spec-expected; the images are left in SHADER_READ_ONLY (FSR) / TRANSFER_SRC (BLIT), and the
  app's next render pass declares `initialLayout = UNDEFINED`, which matches any actual layout.

## Residual risks carried to device validation (labeled)
- Vendor swapchain usage-flag behavior: the AOSP reference swapchain reports
  TRANSFER_SRC|TRANSFER_DST|SAMPLED|STORAGE|COLOR|INPUT unconditionally (swapchain.cpp:1051);
  Mali's ICD values on the Spark 40 are UNVERIFIED until the layer's logs are read on device.
- The present queue's compute capability on Mali is expected (graphics family includes compute)
  but is asserted only by Arm's documentation, not yet by this device's logs.
- Multi-swapchain present, compute-only queue families, and protected-buffer surfaces are
  passthrough-only paths (documented), not validated.
