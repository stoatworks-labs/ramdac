# Attributions

Ramdac is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The effect's shape, the GL state guard, the pass buffers, the log and the tooling — Stoatworks slitscan

<https://github.com/stoatworks-labs/slitscan>  
Licence: MIT  
Copyright: Stoatworks Labs

Taken from slitscan and adapted: the plugin class's shape and its table of controls; source/GLState.h, source/PassBuffer.{h,cpp} and source/Diag.{h,cpp}; the host-clock unit vote in RamdacPlugin::UpdateClock (rosette's, by way of millpond and slitscan); the harness's rig, PNG writer, --pipe/--film, --negative with its Perturb, --state, --names and --offline; tools/verify.sh, tools/mutate.sh, tools/glslc.sh and tools/sweep.py; tools/wincheck.ps1 (isohypse's); and the CI and release workflows.

### lowbias32 — Chris Wellons, hash-prospector

<https://github.com/skeeto/hash-prospector>  
Licence: Unlicense (public domain)  
Copyright: Chris Wellons

The analysis pass in source/Shaders.cpp uses this 32-bit integer hash, its five lines and two constants as published, to pick which cell of each block the adaptive palette samples. The pick is fixed per block, so a still clip gives the same samples every frame.

### Teletext's eight colours — Stoatworks teletext

<https://github.com/stoatworks-labs/teletext>  
Licence: MIT  
Copyright: Stoatworks Labs

Ramdac's Teletext palette is teletext's eight colours in its code order. tools/siblings.py reads teletext's source and checks that the two agree.

### The Amiga's 12-bit colour registers — Stoatworks copperlist

<https://github.com/stoatworks-labs/copperlist>  
Licence: MIT  
Copyright: Stoatworks Labs

Workbench 1.3 and 2.0 are held as 12-bit $RGB registers and expanded to 8 bits a gun by x17, as copperlist's chip::Expand12 does. tools/siblings.py checks the two agree.

### The ZX Spectrum, NES and Game Boy tables — Stoatworks nesolume

<https://github.com/stoatworks-labs/nesolume>  
Licence: MIT  
Copyright: Stoatworks Labs

The ZX Spectrum (basic at 0xD7, bright at 0xFF), 16 of the 2C02 NES entries and the DMG Game Boy's four greens are nesolume's values. nesolume's Commodore 64 table is not used (see Pepto's page under the colour values); tools/siblings.py checks the shared tables agree and prints the C64 difference instead of failing on it.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Data this project ships

Measurements made by other people and carried inside the product. Each is credited the way its publisher asks, and where a licence requires its attribution to travel with the data, that text is reproduced here.

### Calculating the color palette of the VIC II (2001) — Philip "Pepto" Timmermann

<https://www.pepto.de/projects/colorvic/2001/>

The Commodore 64 palette: the VIC-II's sixteen as Pepto measured and calculated them. nesolume's table is labelled with the same source but holds different values (its red is 136,57,50 where the page has 104,55,43, and only 2 of its 16 entries match); ramdac uses the page's. rmtest --palettes checks the plugin's tables against the harness's own transcription.

### The IBM Color/Graphics Monitor Adapter's sixteen colours — IBM

<https://en.wikipedia.org/wiki/Color_Graphics_Adapter>

CGA 16 and EGA's default palette: the RGBI colours as the IBM 5153 showed them, with colour 6's green halved to make brown. CGA Mode 4 is palette 1 with the intensity bit set.

### CPC 464/664/6128 Firmware (SOFT968), Appendix V — Amstrad

<https://archive.org/details/SOFT968TheAmstrad6128FirmwareManual>

The Amstrad CPC palette is the power-on inks 0-13, firmware colours 1, 24, 20, 6, 26, 0, 2, 8, 10, 12, 14, 16, 18 and 22; inks 14 and 15 flash at power-on and are left out. Each colour number is 9G + 3R + B with a gun at 0, 1 or 2, and the three levels, 0x00, 0x60 and 0xFF, are the ones MAME's CPC driver gives (src/mame/amstrad/amstrad_m.cpp). Only the numbers are used.

### PICO-8's palette — Lexaloffle Games; the PICO-8 wiki; Lospec

<https://pico-8.fandom.com/wiki/Palette>

PICO-8's sixteen in its own colour order. Lexaloffle's manual names the colours but gives no hex, so the values are the PICO-8 wiki's, and Lospec's list (lospec.com/palette-list/pico-8) agrees with every one. One tutorial site gives colour 11 as #00E756; the wiki and Lospec both have #00E436, which is what ramdac uses.

### Workbench 1.x and 2.x colour registers, AmigaFFH's amiga_palettes — Pepijn de Vries

<https://github.com/pepijn-devries/AmigaFFH>

The values Workbench 1.3 and 2.0 were checked against: wb.os1's four registers (#0055AA, #FFFFFF, #000022, #FF8800) and the first four of wb.os2 (#AAAAAA, #000000, #FFFFFF, #6688BB), from data/amiga_palettes.rda. Only the numbers are used.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### A perceptual color space for image processing (2020) — Björn Ottosson

<https://bottosson.github.io/posts/oklab/>

OKLab, in which every nearest-colour decision is made, with the post's matrices as revised on 2021-01-25. rmtest --oklab reproduces the post's XYZ to OKLab test table by the harness's own route and checks the plugin's direct linear-sRGB matrices against it.

### RGB/XYZ matrices — Bruce Lindbloom

<http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html>

The harness's independent route to OKLab goes sRGB to XYZ by Lindbloom's matrix, then through Ottosson's M1 and M2, so the plugin's own matrices are never their own reference.

### Pattern dithering, US 6,606,166 (expired 2019) — Thomas Knoll; with Joel Yliluoma's account

<https://patents.google.com/patent/US6606166B1>

Ordered dither for an arbitrary palette: N candidates per cell, each the nearest entry to the target plus the error so far, chosen by the cell's rank. Yliluoma's article (bisqwit.iki.fi/story/howto/dither/jy/) is a clear account of it. Ramdac accumulates the error in linear light and picks candidates in OKLab, and is otherwise the patent's method; rmtest --tone checks every cell against the harness's own Knoll candidates.

### An optimum method for two-level rendition of continuous-tone pictures (IEEE ICC, 1973) — B. E. Bayer

The 2x2, 4x4 and 8x8 index matrices. rmtest --tone builds the standard matrix independently and checks that cells turn white in its order.

### The void-and-cluster method for dither array generation (Proc. SPIE 1913, 1993) — Robert Ulichney

tools/bluenoise.py generates the 64x64 blue-noise mask by this method, and tools/verify.sh regenerates it and compares the bytes.

### Digital halftones by dot diffusion (ACM TOG 6(4), 1987) — Donald E. Knuth

<https://patents.google.com/patent/US8077355B2>

Error diffusion done in parallel: an 8x8 class matrix, each pixel's error passed to its neighbours of higher class, orthogonal at weight 2 and diagonal at 1. The class matrix was checked against US 8,077,355 B2, which reproduces the paper's as its Table 2, and the harness holds its own transcription; rmtest --diffusion checks every cell against Knuth's recurrence.

### Color image quantization for frame buffer display (SIGGRAPH 1982) — Paul Heckbert

Median cut, which seeds the adaptive palette.

### Least squares quantization in PCM (IEEE Trans. Inf. Theory 28(2), 1982) — Stuart Lloyd

The k-means iterations that refine the adaptive palette, warm-started from the last frame. rmtest --adaptive checks they never end worse than median cut.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Mark Ferrari's colour-cycling art

Ferrari's Deluxe Paint scenes, where water runs, rain falls and fire flickers while not one pixel changes, only the colour registers. This is the reason the plugin keeps the index and the colour apart. No artwork of his is used.

### The palette DAC

VGA's RAMDAC, the Amiga's colour registers and every indexed display's colour lookup table: the hardware that made a palette swap a write of a few bytes. The plugin is named after it, and its DAC control rounds the table to those grids.

## Standards and published specifications

What the implementation is measured against.

- **IEC 61966-2-1 (sRGB)** — the transfer function the plugin linearises with before quantising and encodes with after; the dither's error is accumulated in linear light.
- **ETSI ETS 300 706, "Enhanced Teletext specification"** — the Teletext palette's eight colours, the corners of the RGB cube, in the order the spacing attributes number them.
- **EA IFF 85 ILBM, the CRNG chunk (Deluxe Paint, 1985)** — colour cycling: a range of registers rotated at a rate where 16384 is 60 steps a second, flag bit 1 clear cycling upwards. rmtest --cycle checks each step is a rotation; Ping-Pong is ramdac's, not CRNG's.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
