#!/usr/bin/env python3
"""Create the temporary unsigned-literal-only ESSL compatibility include."""
from pathlib import Path
import sys
source, out = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
(out / "ffx_a.h").write_bytes((source / "ffx_a.h").read_bytes())
text = (source / "ffx_fsr1.h").read_text()
replacements = {"con3[2]=con3[3]=0;": "con3[2]=con3[3]=0u;", "con[2]=0;": "con[2]=0u;", "con[3]=0;": "con[3]=0u;"}
for old, new in replacements.items():
    if old not in text:
        raise SystemExit(f"AMD header changed; expected text missing: {old}")
    text = text.replace(old, new, 1)
(out / "ffx_fsr1.h").write_text(text)
