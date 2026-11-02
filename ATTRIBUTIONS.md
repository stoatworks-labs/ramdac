# Attributions

Ramdac is built on other people's work. This file lists what that work is, who did it,
and what it is doing here.

It is hand-written for now. Ramdac isn't registered in the `stoatworks-backend` repo yet.
Once it is, the master lists there will generate this file through
`scripts/sync-attributions.py`, as they do for the rest of the fleet, and those lists
should start from this file.

## Code we derived from other people's work

Someone else solved this first, and this project wouldn't exist in its current form without their work.

### The effect's shape, the GL state guard, the pass buffers, the log and the tooling — Stoatworks slitscan

<https://github.com/stoatworks-labs/slitscan>  
Licence: MIT  
Copyright: Stoatworks Labs

Taken from slitscan and adapted:

- the plugin class's shape and its table of controls;
- `source/GLState.h`, `source/PassBuffer.{h,cpp}` and `source/Diag.{h,cpp}`;
- the host-clock unit vote in `RamdacPlugin::UpdateClock` (rosette's, by way of millpond and slitscan);
- the harness's rig, PNG writer, `--pipe`/`--film`, `--negative` with its Perturb, `--state`, `--names` and `--offline`;
- `tools/verify.sh`, `tools/mutate.sh`, `tools/glslc.sh` and `tools/sweep.py`;
- `scripts/release-lib.sh`.

### lowbias32 — Chris Wellons, hash-prospector

<https://github.com/skeeto/hash-prospector>  
Licence: Unlicense (public domain)  
Copyright: Chris Wellons

The analysis pass in source/Shaders.cpp uses this 32-bit integer hash, with its five lines and two constants as published, to pick which cell of each block the adaptive palette samples. Fixed per block, so a still clip gives the same samples every frame.

### Palette values shared with Stoatworks teletext, copperlist and nesolume

<https://github.com/stoatworks-labs/teletext>, <https://github.com/stoatworks-labs/copperlist>, <https://github.com/stoatworks-labs/nesolume>  
Licence: MIT  
Copyright: Stoatworks Labs

Where a sibling already holds a palette, ramdac uses the same values. `tools/siblings.py` reads the siblings' source and checks that they agree:

- **Teletext** is teletext's eight colours, in its code order.
- **Workbench 1.3 and 2.0** expand 12-bit `$RGB` registers to 8 bits by ×17, as copperlist's `chip::Expand12` does.
- **ZX Spectrum, NES and Game Boy** use nesolume's tables.

The Commodore 64 is the exception, for the reason given under Pepto below.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

This is the plugin ABI itself. An FFGL effect or source is defined by this SDK's headers, and nothing else will load in Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Comes inside the FFGL submodule at external/ffgl/deps/glew-2.1.0 and isn't fetched separately.

It resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Comes inside the FFGL submodule, under the SDK's CustomThumbnail sample.

The plugin doesn't call it. It's listed because it's in the checkout.

## Published colour values

The palettes are the machines' own published or measured colours. Ramdac uses the numbers and nothing else: no code, images or text from these sources. `rmtest --palettes` checks the plugin's tables against the harness's own transcription of them.

### Commodore 64 — Philip "Pepto" Timmermann, *Calculating the color palette of the VIC II* (2001)

<https://www.pepto.de/projects/colorvic/2001/>

The VIC-II's sixteen as Pepto measured and calculated them. nesolume's table is labelled with the same source but holds different values: its red is 136,57,50 where the page has 104,55,43, and only 2 of its 16 entries match. Ramdac uses the page's values, and `tools/siblings.py` prints the difference instead of failing on it.

### CGA and EGA — IBM Color/Graphics Monitor Adapter

<https://en.wikipedia.org/wiki/Color_Graphics_Adapter>

The sixteen RGBI colours as the IBM 5153 showed them, with colour 6's green halved to make brown. CGA Mode 4 is palette 1 with the intensity bit set. EGA's default palette is the same sixteen.

### Amstrad CPC — *CPC 464/664/6128 Firmware* (Amstrad, SOFT968), Appendix V; MAME

<https://archive.org/details/SOFT968TheAmstrad6128FirmwareManual>  
<https://github.com/mamedev/mame> (`src/mame/amstrad/amstrad_m.cpp`)

The firmware manual gives the power-on ink colours. Inks 0–13 are firmware colours 1, 24, 20, 6, 26, 0, 2, 8, 10, 12, 14, 16, 18 and 22. Inks 14 and 15 flash at power-on, so they're left out. Each colour number is 9G + 3R + B, a gun at 0, 1 or 2. MAME's CPC driver gives the three levels as 0x00, 0x60 and 0xFF.

### PICO-8 — Lexaloffle Games

<https://www.lexaloffle.com/pico-8.php>  
<https://pico-8.fandom.com/wiki/Palette>  
<https://lospec.com/palette-list/pico-8>

PICO-8's sixteen in its own colour order. Lexaloffle's manual names the colours but gives no hex. The values come from the PICO-8 wiki's Palette page, and Lospec's list agrees with every one. One tutorial site gives colour 11 as #00E756; the wiki and Lospec both have #00E436, and that's what ramdac uses.

### Amiga Workbench 1.3 and 2.0 — Pepijn de Vries, AmigaFFH

<https://github.com/pepijn-devries/AmigaFFH> (`data/amiga_palettes.rda`)

AmigaFFH is an R package that reads Amiga file formats. Its `amiga_palettes` data has Workbench 1.x's four colour registers (`wb.os1`: #0055AA, #FFFFFF, #000022, #FF8800) and Workbench 2.x's (the first four of `wb.os2`: #AAAAAA, #000000, #FFFFFF, #6688BB). Ramdac holds them as 12-bit `$RGB` registers ($05A $FFF $002 $F80 and $AAA $000 $FFF $68B), which expand ×17 to exactly those values.

### ZX Spectrum, NES and Game Boy

These are nesolume's tables (see above). The Spectrum is basic at 0xD7 and bright at 0xFF. The NES subset is 16 of nesolume's 2C02 entries. The Game Boy is the DMG's four greens.

### Teletext — ETS 300 706, *Enhanced Teletext specification*

The eight colours are the corners of the RGB cube, in the order the spacing attributes number them, which is teletext's.

## Work we checked ourselves against

No code was taken from these, but they're how we knew we had it right, and that's worth saying out loud.

### OKLab — Björn Ottosson, *A perceptual color space for image processing* (2020)

<https://bottosson.github.io/posts/oklab/>

Every nearest-colour decision is made in OKLab, using the post's matrices as revised on 2021-01-25. `rmtest --oklab` reproduces the post's XYZ→OKLab test table by the harness's own route and checks that the plugin's direct linear-sRGB matrices agree with it.

### sRGB — IEC 61966-2-1:1999; Bruce Lindbloom's RGB/XYZ matrices

<http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html>

The transfer function the plugin linearises with and encodes with. The harness's independent route to OKLab goes sRGB → XYZ by Lindbloom's matrix, then Ottosson's M1 and M2.

### Pattern dithering — Thomas Knoll, US 6,606,166 (expired); Joel Yliluoma

<https://patents.google.com/patent/US6606166B1>  
<https://bisqwit.iki.fi/story/howto/dither/jy/>

Ordered dither for an arbitrary palette: N candidates per cell, each the nearest entry to the target plus the error so far. The patent expired in 2019. Yliluoma's article is a clear account of the algorithm. Ramdac accumulates the error in linear light and picks candidates in OKLab, and is otherwise the patent's method.

### Bayer matrices — B. E. Bayer, *An optimum method for two-level rendition of continuous-tone pictures* (IEEE ICC, 1973)

The 2×2, 4×4 and 8×8 index matrices. `rmtest --tone` builds the standard matrix independently and checks that cells turn white in its order.

### Blue noise — Robert Ulichney, *The void-and-cluster method for dither array generation* (Proc. SPIE 1913, 1993)

`tools/bluenoise.py` generates the 64×64 mask by this method, and `verify.sh` regenerates it and compares the bytes.

### Dot diffusion — Donald E. Knuth, *Digital halftones by dot diffusion* (ACM TOG 6(4), 1987)

<https://patents.google.com/patent/US8077355B2>

Error diffusion done in parallel: an 8×8 class matrix, and each pixel's error passed to its neighbours of higher class, orthogonal neighbours at weight 2 and diagonal at 1. The class matrix was checked against US 8,077,355 B2, which reproduces the paper's as its Table 2. The harness holds its own transcription.

### Median cut — Paul Heckbert, *Color image quantization for frame buffer display* (SIGGRAPH 1982); k-means — Stuart Lloyd, *Least squares quantization in PCM* (IEEE Trans. Inf. Theory 28(2), 1982)

The adaptive palette: median cut seeds it, then Lloyd's iterations refine it, warm-started from the last frame.

### Colour cycling — Electronic Arts, IFF ILBM's `CRNG` chunk (Deluxe Paint, 1985)

<https://en.wikipedia.org/wiki/ILBM>

A range of colour registers rotated at a rate where 16384 is 60 steps a second. Flag bit 1 clear cycles upwards. Ping-Pong isn't CRNG's.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined; the debt is to the idea.

### Mark Ferrari's colour-cycling art

Ferrari's Deluxe Paint scenes, where water runs, rain falls and fire flickers while not one pixel changes, only the colour registers. This is the reason the plugin keeps the index and the colour apart.

### The palette DAC

VGA's RAMDAC, the Amiga's colour registers in Denise and every indexed display's colour lookup table: the hardware that made a palette swap a write of a few bytes. The plugin is named after it.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you'd rather not be listed, open an issue and it will be fixed.
