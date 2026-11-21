#!/usr/bin/env python3
"""Generate deterministic low-resolution inputs and their 2x reference images."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WIDTH = 320
HEIGHT = 180

def color(case, x, y):
    if case == "diagonal":
        return (235, 235, 235) if x * HEIGHT >= y * WIDTH else (18, 28, 54)
    if case == "checker":
        return (235, 235, 235) if ((x // 12 + y // 12) & 1) else (20, 28, 38)
    # Thin high-contrast shape with a one-pixel outline and a two-pixel stem.
    left, right = int(WIDTH*0.2), int(WIDTH*0.8)
    top, bottom = int(HEIGHT*0.2), int(HEIGHT*0.7)
    on = (left <= x <= right and (y in (top, bottom) or x in (left, right))) or (WIDTH//2-2 <= x <= WIDTH//2+2 and bottom <= y < HEIGHT-4)
    return (245, 190, 38) if on else (18, 25, 38)

def ppm(path, width, height, pixels):
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + bytes(c for p in pixels for c in p))

for case in ("diagonal", "checker", "shape"):
    ref = [color(case, x, y) for y in range(HEIGHT) for x in range(WIDTH)]
    source = []
    for y in range(HEIGHT // 2):
        for x in range(WIDTH // 2):
            block = [ref[(2*y+dy)*WIDTH + 2*x+dx] for dy in range(2) for dx in range(2)]
            source.append(tuple(sum(pixel[c] for pixel in block) // 4 for c in range(3)))
    ppm(ROOT / f"{case}_input_{WIDTH//2}x{HEIGHT//2}.ppm", WIDTH // 2, HEIGHT // 2, source)
    ppm(ROOT / f"{case}_reference_{WIDTH}x{HEIGHT}.ppm", WIDTH, HEIGHT, ref)
