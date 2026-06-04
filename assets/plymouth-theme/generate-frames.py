#!/usr/bin/env python3
"""Generate Plymouth two-step animation frames for the AirzDowne boot theme.

Produces 30 frames each for animation-XXXX.png and throbber-XXXX.png using
a sinusoidal breathing curve (opacity = 0.72 + 0.28 * sin(t)) over a white
background. Run from this directory or pass an output path as the first argument.

Requires: pillow  (pip install pillow)
"""

import math
import sys
from pathlib import Path
from PIL import Image

FRAMES = 30
SIZE = (500, 500)
LOGO = Path(__file__).parent / "logo.png"


def make_frame(logo_rgba: Image.Image, opacity: float) -> Image.Image:
    bg = Image.new("RGBA", SIZE, (255, 255, 255, 255))
    blended = logo_rgba.copy()
    r, g, b, a = blended.split()
    a = a.point(lambda v: int(v * opacity))
    blended = Image.merge("RGBA", (r, g, b, a))
    bg.paste(blended, (0, 0), blended)
    return bg.convert("RGB")


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent
    out.mkdir(parents=True, exist_ok=True)

    logo = Image.open(LOGO).convert("RGBA").resize(SIZE, Image.LANCZOS)

    for i in range(FRAMES):
        t = 2 * math.pi * i / FRAMES
        opacity = 0.72 + 0.28 * math.sin(t)
        frame = make_frame(logo, opacity)
        frame.save(out / f"animation-{i:04d}.png")
        frame.save(out / f"throbber-{i:04d}.png")
        print(f"  frame {i+1:02d}/{FRAMES}  opacity={opacity:.3f}")

    print(f"Done — {FRAMES * 2} frames written to {out}")


if __name__ == "__main__":
    main()
