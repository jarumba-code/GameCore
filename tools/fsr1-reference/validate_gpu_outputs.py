#!/usr/bin/env python3
"""Quality floor for actual AMD-header Vulkan outputs on deterministic fixtures."""
from pathlib import Path
import math
import sys


def ppm(path: Path):
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6" or parts[2] != b"255":
        raise ValueError(f"invalid P6 PPM: {path}")
    width, height = map(int, parts[1].split())
    pixels = parts[3]
    if len(pixels) != width * height * 3:
        raise ValueError(f"wrong payload size: {path}")
    return width, height, pixels


root, outdir = Path(sys.argv[1]), Path(sys.argv[2])
floors = {"diagonal": 32.0, "checker": 15.0, "shape": 25.0}
for name, floor in floors.items():
    w, h, actual = ppm(outdir / f"{name}_amd_fsr1_gpu.ppm")
    rw, rh, expected = ppm(root / f"testdata/{name}_reference_{w}x{h}.ppm")
    if (w, h) != (rw, rh):
        raise AssertionError(f"{name}: output dimensions {w}x{h}, expected {rw}x{rh}")
    mse = sum((a - b) ** 2 for a, b in zip(actual, expected)) / len(actual)
    psnr = 99.0 if mse == 0 else 10.0 * math.log10(255.0**2 / mse)
    if not math.isfinite(psnr) or psnr < floor:
        raise AssertionError(f"{name}: AMD Vulkan output PSNR {psnr:.4f} dB below {floor:.1f} dB floor")
    print(f"AMD-header Vulkan quality: {name} PSNR={psnr:.4f} dB floor={floor:.1f} dB")
