# Android Magpie M2 FSR 1 benchmark and quality report

Probe date: 2026-11-21. Target: the Redmi A5 identified in [the capability report](ANDROID_MAGPIE_M2_DEVICE_CAPABILITIES.md). Input was deterministic 160×90 8-bit PPM; output was 320×180. The GPU path uses an RGBA8 sampled input image, an RGBA8 storage EASU intermediate, then a separate RCAS dispatch to an RGBA8 output. EASU and RCAS remain separate GPU passes and the intermediate stays GPU-resident. The harness uploads source pixels and reads the final output back for comparison.

AMD source is pinned at `a21ffb8f6c13233ba336352bdff293894c706575` (`v1.0.2`) in `third_party/amd-fsr1/UPSTREAM_REVISION`. Shader code includes the upstream `FsrEasuF` and `FsrRcasF` routines. Their license is retained in `third_party/amd-fsr1/LICENSE.txt`.

## Results

Each GPU row is one device run. Compute pass timings are Vulkan timestamp-query durations. CPU timings are the existing scalar C++ reference, running EASU then RCAS on the same input and output dimensions. The GPU total is an independent timestamp interval over the command sequence (including upload/readback work recorded in that sequence); the CPU total is algorithm time only. Pipeline creation and per-process startup are excluded from GPU timestamps but included in `setup_submit_readback_ms`, so the latter is not representative of a persistent app pipeline.

| Fixture | GPU EASU ms | GPU RCAS ms | GPU timestamp total ms | CPU EASU ms | CPU RCAS ms | CPU total ms | GPU output PSNR vs analytic ref |
|---|---:|---:|---:|---:|---:|---:|---:|
| diagonal | 1.6492 | 0.5212 | 2.6885 | 14.9639 | 8.4683 | 23.4322 | 32.6339 dB |
| checker | 1.6488 | 0.5382 | 2.7472 | 16.3909 | 8.5771 | 24.9683 | 16.0770 dB |
| shape | 1.6532 | 0.4860 | 2.6599 | 15.0973 | 8.5015 | 23.5988 | 25.6036 dB |

The output was produced by actual Vulkan dispatch using AMD's shader routines. The output-quality test checks fixture floors of 32, 15 and 25 dB respectively. The CPU and GPU paths are not bit-identical: GPU EASU writes the RGBA8 intermediate before RCAS, while the scalar CPU reference keeps floating-point intermediate pixels. Measured GPU-vs-CPU output RMSE was 0.00373 (diagonal), 0.007995 (checker), and 0.00285 (shape); max channel errors were 0.0762, 0.0664 and 0.1516. These are diagnostic parity measurements, not a guarantee of identical image output.

## M1 checkerboard investigation

The high-resolution checker fixture uses 12-pixel alternating squares; 2×2 box reduction makes six-pixel squares in the 160×90 input. It is a high-contrast periodic edge pattern, but it is **not** a Nyquist checkerboard. The exact-reference metric favors the sharply aligned square boundaries created by the fixture generator. M1's scalar measurements show EASU alone at 17.3354 dB, already below bilinear at 18.7646 dB; adding RCAS lowers it to 17.0880 dB. Thus the main degradation begins with EASU's directional reconstruction on this synthetic pattern, while RCAS sharpening makes the metric slightly worse. In the official-shader Vulkan output, output channel values span 8–246 while the analytic reference spans 20–235, confirming some overshoot. A sampled edge row also shows ringing around the step (GPU grayscale values 210, 215, 161, 17, 246, 103 versus a reference step of 235 to 29). The PSNR result therefore reflects EASU/RCAS edge reconstruction and ringing against a specific pixelwise target. This is a measured case; it is not evidence the shader failed to execute or a universal predictor of game-image quality. The Vulkan test does not separately read back EASU-only output, so the detailed GPU-only attribution between stages remains unmeasured.

M1 CPU results were bilinear 18.7646 dB / global SSIM 0.9545; CPU EASU 17.3354 dB; CPU EASU+RCAS 17.0880 dB / 0.9359. The official AMD shader's GPU output scored 16.0770 dB against the same reference. Sharpening lowers M1 CPU PSNR by a further 0.2474 dB. These scores stay visible in per-fixture results and must not be averaged away. The fixture must not be used as a universal predictor of game-image quality.

## Memory and thermal

- Vulkan reported **553,984 bytes** for the three image allocation requirements at this resolution (input + EASU intermediate + output).
- The two staging buffers required **288,000 bytes** of host-visible Vulkan allocation (57,600-byte upload and 230,400-byte output readback, including any allocator requirement size).
- Process peak RSS was 44.6–45.7 MB for this one-shot Termux runner; this includes runtime/loader and CPU reference memory and is not app incremental overhead.
- Thermal data could not be read in the current shell due to Android permissions; see the capability report. Thermal response and sustained throttling are unmeasured.

## Interpretation limits

These results verify that the two FSR passes execute on this Redmi A5 GPU and provide real pass timings. They do not establish a user-visible frame-rate gain, real-time suitability, or game compatibility. The harness starts a new process, creates pipelines, uploads a CPU PPM, and reads output back; the setup/submit/readback interval measured 140–379 ms across runs. App integration must keep GPU resources/pipelines alive and avoid CPU readback in the frame path. No comparison to an unoptimized game frame is possible at this stage.
