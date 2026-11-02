#!/usr/bin/env python3
"""
Ramdac's palettes against the siblings that already hold the same colours.

The brief: where ramdac and a sibling share a palette, they share its values.
This reads the siblings' own source and says, palette by palette, whether
they agree -- or, where they knowingly do not, says so and what differs.

    teletext    source/Shaders.cpp   the eight colours as bits: red 0, green 1, blue 2
    copperlist  source/chip/Chipset.h  Expand12: a 12-bit $RGB to 8 bits a gun is x17
    nesolume    source/Consoles.cpp  kStore: Game Boy [0..3], NES [4..67],
                                     ZX Spectrum [68..82], Commodore 64 [83..98]

The Commodore 64 is the one knowing disagreement: ramdac takes Pepto's 2001
measurement (pepto.de/projects/colorvic/2001), nesolume a later rendering of
it. It is printed, not failed; AGENTS.md says why.

A sibling that is not checked out is skipped, not failed.

    python3 tools/siblings.py [--projects ~/Projects/resolume]
"""

import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent


def ramdac_tables():
    text = (REPO / "source" / "Palettes.cpp").read_text()
    tables = {}
    for m in re.finditer(r"const uint32_t (k\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S):
        tables[m.group(1)] = [(int(h, 16) >> 16, (int(h, 16) >> 8) & 255, int(h, 16) & 255)
                              for h in re.findall(r"0x([0-9A-Fa-f]{6})", m.group(2))]
    return tables


def nesolume_store(path):
    text = path.read_text()
    body = re.search(r"kStore\[\]\[\s*3\s*\]\s*=\s*\{(.*?)\n\};", text, re.S).group(1)
    # Comments carry the section names and indices; drop them before reading triples.
    triples = re.findall(r"\{\s*(\d+),\s*(\d+),\s*(\d+)\s*\}", re.sub(r"//[^\n]*", "", body))
    return [tuple(int(v) for v in t) for t in triples]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--projects", type=pathlib.Path, default=pathlib.Path.home() / "Projects" / "resolume")
    args = parser.parse_args()
    ours = ramdac_tables()
    failures = 0

    def check(ok, message):
        nonlocal failures
        print(f"   {'ok  ' if ok else 'FAIL'} {message}")
        failures += 0 if ok else 1

    teletext = args.projects / "teletext" / "source" / "Shaders.cpp"
    if teletext.exists():
        rule = "vec3( float( c & 1 ), float( ( c >> 1 ) & 1 ), float( ( c >> 2 ) & 1 ) )" in teletext.read_text()
        bits = [(255 * (c & 1), 255 * ((c >> 1) & 1), 255 * ((c >> 2) & 1)) for c in range(8)]
        check(rule and ours["kTeletext"] == bits,
              "teletext: the eight colours are its bits (red 0, green 1, blue 2), in its code order")
    else:
        print(f"   skipped: no {teletext}")

    chipset = args.projects / "copperlist" / "source" / "chip" / "Chipset.h"
    if chipset.exists():
        text = chipset.read_text()
        check("( r * 17u ) << 16 | ( g * 17u ) << 8 | ( b * 17u )" in text,
              "copperlist: Expand12 is x17 a gun, ramdac's 12-bit DAC (rmtest --dac measures the plugin's)")
    else:
        print(f"   skipped: no {chipset}")

    consoles = args.projects / "nesolume" / "source" / "Consoles.cpp"
    if consoles.exists():
        store = nesolume_store(consoles)
        check(len(store) == 99, f"nesolume: kStore read as {len(store)} colours (99: 4 + 64 + 15 + 16)")
        gameboy, nes, spectrum, c64 = store[0:4], store[4:68], store[68:83], store[83:99]
        check(ours["kGameBoy"] == gameboy, "nesolume: Game Boy, all four, the same values in the same order")
        missing = [c for c in ours["kNes"] if c not in nes]
        check(not missing, f"nesolume: NES, ramdac's {len(ours['kNes'])} are all in its 2C02 table"
              + (f" -- not: {missing}" if missing else ""))
        check(ours["kSpectrum"] == spectrum, "nesolume: ZX Spectrum, all fifteen, the same values in the same order")
        differ = sum(a != b for a, b in zip(ours["kC64"], c64))
        same = 16 - differ
        print(f"   note Commodore 64: {same} of 16 the same as nesolume's, {differ} differ -- ramdac is Pepto 2001, "
              f"nesolume a later rendering (AGENTS.md)")
    else:
        print(f"   skipped: no {consoles}")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
