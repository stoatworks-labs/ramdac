#!/usr/bin/env python3
"""
The README's figures, rendered by rmtest from a real clip -- not captured from
Resolume.

    python3 tools/figures.py [--build build] [--clip PATH] [--out docs]

The clip is Resolume's own IntoTheGlow_02 (Arena's bundled media, the fleet's
usual), 1.17 to 1.67 s in at its 30 fps through `rmtest --pipe` at 640x360,
and each panel is the last of those 15 frames. The clip strobes: its white
panels come on about 1.45 s in and are gone by 1.8 s, so Adaptive's panel is at
Adapt 0, the palette the frame gets, rather than the default 0.3 s one-pole
still a fifth of a second behind the flash. Needs ffmpeg and Pillow.

  palettes.png    the frame through twelve palette and dither settings
  swap.png        C64 quantised; swapped to PICO-8 (a Cut); PICO-8 quantised
                  afresh; half way through a Fade
  cycle.png       a ring card, each ring one PICO-8 index, cycling Forward:
                  eight frames a step apart (rmtest --strip)
  cycle-clip.png  the frame frozen and its middle eight entries cycling: six
                  frames a step apart, the indices never moving
"""

import argparse
import pathlib
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

REPO = pathlib.Path(__file__).resolve().parent.parent
W, H = 640, 360
CLIP = pathlib.Path("/Applications/Resolume Arena/media/Shop74/IntoTheGlow_02.mov")


def frames(clip, start=1.17, seconds=0.5):
    """The clip as raw RGBA at W x H, its own 30 fps."""
    return subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-ss", str(start), "-i", str(clip), "-t", str(seconds),
         "-vf", f"scale={W}:{H}:flags=area", "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
        check=True, capture_output=True).stdout


def through(rmtest, raw, settings, script=None):
    """Every frame of `raw` through the plugin; the frames back, as images."""
    args = [str(rmtest), "--pipe", "--size", f"{W}x{H}", "--fps", "30"]
    for s in settings:
        args += ["--set", s]
    if script:
        args += ["--script", str(script)]
    out = subprocess.run(args, input=raw, check=True, capture_output=True).stdout
    size = W * H * 4
    return [Image.frombytes("RGBA", (W, H), out[i:i + size]).convert("RGB") for i in range(0, len(out), size)]


def label(img, text):
    font = ImageFont.load_default(size=20)
    band = Image.new("RGB", (img.width, 34), (16, 16, 16))
    ImageDraw.Draw(band).text((10, 6), text, fill=(235, 235, 235), font=font)
    out = Image.new("RGB", (img.width, img.height + band.height))
    out.paste(img, (0, 0))
    out.paste(band, (0, img.height))
    return out


def grid(panels, columns, gap=6):
    rows = (len(panels) + columns - 1) // columns
    pw, ph = panels[0].size
    sheet = Image.new("RGB", (columns * pw + (columns - 1) * gap, rows * ph + (rows - 1) * gap), (0, 0, 0))
    for i, p in enumerate(panels):
        sheet.paste(p, ((i % columns) * (pw + gap), (i // columns) * (ph + gap)))
    return sheet


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--clip", type=pathlib.Path, default=CLIP)
    parser.add_argument("--out", type=pathlib.Path, default=REPO / "docs")
    args = parser.parse_args()
    rmtest = args.build / "rmtest"
    if not args.clip.exists():
        print(f"no clip at {args.clip} (--clip PATH)", file=sys.stderr)
        return 1
    args.out.mkdir(parents=True, exist_ok=True)
    raw = frames(args.clip)
    source = Image.frombytes("RGBA", (W, H), raw[-W * H * 4:]).convert("RGB")

    # One frame, many palettes and dithers. Pixel Size 2 throughout.
    looks = [
        ("PICO-8, no dither", ["Palette=PICO-8", "Dither=None"]),
        ("PICO-8, Bayer 4", ["Palette=PICO-8", "Dither=Bayer 4"]),
        ("PICO-8, Blue Noise", ["Palette=PICO-8", "Dither=Blue Noise"]),
        ("PICO-8, Dot Diffusion", ["Palette=PICO-8", "Dither=Dot Diffusion"]),
        ("Commodore 64, Bayer 4", ["Palette=Commodore 64", "Dither=Bayer 4"]),
        ("CGA 16, Bayer 8", ["Palette=CGA 16", "Dither=Bayer 8"]),
        ("Amstrad CPC, Bayer 2", ["Palette=Amstrad CPC", "Dither=Bayer 2"]),
        ("Game Boy, Bayer 4", ["Palette=Game Boy", "Dither=Bayer 4"]),
        ("Teletext, Blue Noise", ["Palette=Teletext", "Dither=Blue Noise"]),
        ("Workbench 1.3, Blue Noise", ["Palette=Workbench 1.3", "Dither=Blue Noise"]),
        ("Adaptive 16 at Adapt 0, Dot Diffusion", ["Palette=Adaptive", "Adapt=0", "Dither=Dot Diffusion"]),
    ]
    panels = [label(source, "the clip")]
    for name, settings in looks:
        panels.append(label(through(rmtest, raw, ["Pixel Size=2"] + settings)[-1], name))
    grid(panels, 4).save(args.out / "palettes.png", optimize=True)

    # The one idea: a swap keeps every index.
    base = ["Palette=Commodore 64", "Dither=Bayer 4", "Pixel Size=2", "Swap To=PICO-8"]
    swap = [
        label(through(rmtest, raw, base)[-1], "Commodore 64, Bayer 4"),
        label(through(rmtest, raw, base + ["Swap Style=Cut", "Swap=1"])[-1], "swapped to PICO-8: the same indices"),
        label(through(rmtest, raw, ["Palette=PICO-8", "Dither=Bayer 4", "Pixel Size=2"])[-1], "PICO-8 quantised afresh"),
        label(through(rmtest, raw, base + ["Swap Style=Fade", "Swap=0.5"])[-1], "half way through a Fade"),
    ]
    grid(swap, 2).save(args.out / "swap.png", optimize=True)

    # Colour cycling on a ring card, a step a panel.
    subprocess.run([str(rmtest), "--strip", "8", "--out", str(args.out / "cycle.png")], check=True,
                   capture_output=True)

    # Colour cycling on the clip, frozen: 15 steps a second at 30 fps is a
    # step every other frame. Freeze from the first frame, so every panel has
    # the same indices.
    held = through(rmtest, raw, ["Palette=PICO-8", "Dither=Bayer 4", "Pixel Size=2", "Freeze=1", "Cycle=Forward",
                                 "Cycle Rate=0.5", "Cycle Start=4", "Cycle Length=8"])
    strip = [label(held[k], f"step {k // 2}") for k in range(0, 12, 2)]
    grid(strip, 3).save(args.out / "cycle-clip.png", optimize=True)
    for name in ("palettes.png", "swap.png", "cycle.png", "cycle-clip.png"):
        print(f"wrote {args.out / name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
