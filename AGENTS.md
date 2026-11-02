# AGENTS.md: Ramdac

The why. `CLAUDE.md` has the commands; `README.md` the claims and the numbers;
`~/Projects/resolume/specs/SPEC-ramdac.md` the spec it was built to.

## The idea

Asked for on 2026-11-02, relayed from Allan's session: an effect that forces a clip or a
comp into a limited palette of 8 or 16 colours and lets you swap palettes. It was to have:

- real published palettes, a user palette and an adaptive one;
- OKLab distance;
- dithers that don't crawl on a still frame;
- palette swap, the payoff: hard swap, crossfade by index, and Mark Ferrari / Deluxe Paint
  colour cycling.

The one mechanism is an **indexed framebuffer and its RAMDAC**. The clip is averaged
into cells, each cell is quantised once to a palette index, and the picture is a
16-entry table looked up by index. `Clut.h` rebuilds the table on the CPU every frame,
in double. Swap, fade, ripple, cycle and the DAC are all writes to it, and the index
buffer never hears about them. Everything the README lists falls out of keeping the
index and the colour apart: every pixel a member, a swap that keeps the structure, a
fade with 16 colours at every instant, cycling on a still buffer, and Freeze.

The passes (`Shaders.h`):

1. **grid**: the clip's linear-light mean per Pixel Size cell, alpha-weighted,
   RGBA16F.
2. **analysis** (Adaptive only): one sample per block of the grid, at most 96×54,
   read back. See below for why it isn't the block's mean.
3. **quantise**: None, the Bayers and Blue Noise, all in one pass.
4. **diffuse / settle / gather**: Dot Diffusion, 64 classes × (diffuse + settle),
   then a gather that unpacks the class blocks into the index texture. That's 128
   draws and a gather.
5. **display**: the table looked up by index, then alpha and Mix.
6. **resample**: under Freeze, a new raster or Pixel Size takes the old index under
   each new cell's centre, by an integer law.

## Decisions taken without asking (the brief says decide and write it down)

- **Name and ID.**
  - `ramdac` is the chip that held the colour table and turned an index into a
    voltage. It names the mechanism, as copperlist, genlock and teletext do. No folder
    under `~/Projects/*/` has the name.
  - Display name `SW Ramdac`, ID `RM01`. RD01, the obvious one, is resodoom's. RM01 is
    in no `*/source` in the fleet, and PG01 and HF01 (the sibling sessions') aren't
    used.
  - Bundle `com.stoatworks.ffgl.ramdac`, harness `rmtest`.
- **Where it lives.** Built in `~/dev/ramdac` (the worktree guard blocks a new repo
  under `~/Projects` before it exists), then moved to `~/Projects/resolume/ramdac` as
  its own git repo on `main`, as slitscan was. Public at
  [stoatworks-labs/ramdac](https://github.com/stoatworks-labs/ramdac) since
  2026-11-02, when Allan gave the go-ahead to release it.
- **The release plumbing is slitscan's, from its origin/main.**
  - `.github/workflows/ci.yml` and `release.yml` were copied after slitscan's
    installer fix (08b5214) and renamed; their history comments were rewritten for
    ramdac. `scripts/release-lib.sh` is the backend's current copy, which has
    `rl_nsis --ffgl`.
  - The workflows exist because the repo is public: the fleet rule is no workflows
    on a private repo.
  - `source/StoatworksAbout.h` and `ATTRIBUTIONS.md` started as hand copies. The
    backend's `sync-about.py` and `sync-attributions.py --adopt` generate them once
    ramdac is registered; the backend's lists were seeded from the hand file.
- **What it doesn't redo.**
  - **nesolume** has console rasters, attribute clash, HAM and broken VRAM. Ramdac has
    no attribute cells and no machine raster; Pixel Size is a plain grid.
  - **teletext** has the serial-attribute encoder. Ramdac's Teletext palette is its
    eight colours, per pixel, with no attribute rules.
  - **copperlist** writes registers per line. Ramdac writes the table once a frame.
  - **receipt and fax** are 1-bit printers with Floyd–Steinberg on the CPU. Ramdac's
    error diffusion is Knuth's dot diffusion, on the GPU.
- **Shared colours.** Where a sibling holds a palette, the values are the sibling's:
  teletext's bits, copperlist's ×17, and nesolume's Game Boy, NES and Spectrum.
  `tools/siblings.py` reads their source and checks it in `verify.sh`.
- **The C64 is not nesolume's.** nesolume labels its table "Pepto's measured VIC-II
  palette", but its values (red 136,57,50) aren't on Pepto's 2001 page (red 104,55,43);
  only 2 of 16 match. Ramdac uses the page's. `siblings.py` prints the difference rather
  than failing. nesolume probably took a later rendering of Pepto's palette (VICE ships
  several). Which one to standardise on is Allan's call.
- **Palette sources, settled on 2026-11-02:**
  - **PICO-8:** a tutorial site gives colour 11 as #00E756. The PICO-8 wiki's Palette
    page and Lospec both give #00E436, and Lexaloffle's manual names colours but gives
    no hex. Ramdac uses #00E436.
  - **Workbench 1.3 and 2.0:** checked against AmigaFFH's `amiga_palettes` data (the
    `.rda` file itself, decompressed).
  - **Amstrad CPC inks:** checked against SOFT968 Appendix V. MAME gives the three gun
    levels.
- **CPC is 14 colours.** Inks 14 and 15 flash at power-on (1/24 and 11/16), and a
    flashing ink isn't a colour.
- **Order defaults to Lightness**, so a Ripple runs dark to light and a cycle range
  is a run of neighbouring lightnesses. Native keeps the machine's own register order,
  which is what a real CRNG range meant.
- **Swap's partner** for different sizes is j = ⌊(i + ½)·M/N⌋: identity when M = N,
  spread evenly otherwise.
- **Cycle.**
  - **Range:** one range, not DPaint's several, each at its own rate.
  - **Ping-Pong** is offered, but isn't CRNG's and says so.
  - **Rate:** 0–60 steps a second, CRNG's whole range (16384 = 60/s), or a step a beat
    or a bar.
  - **Step count:** `floor( phase + 1e-6 )`, so a phase that is a whole number in exact
    arithmetic doesn't land a step short in double.
- **The DAC rounds to the nearest level**, with a tie going to the lower level, and
  widens by bit replication, so 4 bits is copperlist's ×17 exactly.
- **The defaults** are PICO-8 through Bayer 4 at Pixel Size 2, which shows what the
  plugin is the moment it's dropped on a clip. Swap waits on the C64 with a Fade, and
  cycling is off. The Custom default is the plugin's own eight colours, not anybody's
  published palette.
- **The option lists are append-only from v0.1.0**, because a saved composition holds
  the index.

## Adaptive

- **The palette.** Median cut seeds it on the first frame or a change of Colours. Then
  Lloyd's k-means refines it each frame, warm-started from the last frame's centroids,
  so an entry stays the same entry. An empty cluster is re-seeded at the sample
  farthest from its centroid. If the warm result ends worse than this frame's median
  cut, the median cut is refined instead and matched to the old entries.
- **What's shown.** It follows the target through a one-pole of Adapt seconds,
  α = 1 − exp(−dt/τ). It snaps to the target once within 1e-3 in OKLab, or once both
  round to the same DAC code, so a still clip reaches a still palette in a bounded
  number of frames.
- **The samples** are colours the cells actually have. For each block of the grid the
  analysis pass:
  1. picks one cell, by a fixed hash of the block (Wellons's lowbias32);
  2. averages the block's cells within 0.04 in OKLab of it, two just-noticeable
     differences.

  The first build averaged the whole block. On a card of white slats a pixel wide, one
  column in eight over navy, that gave a palette whose lightest entry was #717483, and
  not one slat pixel was white. A block's mean is a colour no cell has. The picked
  cell keeps the slats white. The like-mean is there so grain is still averaged where
  the block is one colour. That's the reasoning; it wasn't measured. A plain hashed
  pick kept the slats white too, and under the noise check both stepped the same
  saturated green by 2 codes. The hash means a still clip gives the same samples every
  frame, and a regular pattern can't alias against a regular stride (a plain stride
  over 1280 columns never lands on a slat one column in eight).
- **How the noise check changed.** The noise check used to say "no entry moves more than
  a code a frame". With the new samples, one saturated green at 320×180 stepped 2 codes
  of red on an OKLab move of 0.0016. Near the gamut edge a tiny move is two codes of
  one gun, so a 1-code bound had only held by luck. The check is now in OKLab, where the
  one-pole works: a tenth of a JND (0.002) a frame, measured 0.0014 and 0.0016. **The
  bound was set after seeing those numbers.** The negative it has to catch (a fresh
  median cut every frame) moves 0.07–0.12, which it does.
- **Not the sampling.** In the README figure the adaptive panel's highlights were grey at
  the default Adapt. That was the one-pole, not the sampling: the clip strobes. Its white
  panels are on from about 1.45 s to 1.75 s, and the figure's frame is 0.2 s into the
  flash. At Adapt 0 the palette has #FBFDFE. The figure shows Adapt 0 and says so.
- **The read-back is synchronous**, as nesolume's and receipt's are.

## The traps actually hit

Ordered by how much they would have cost.

- **Apple's GPU truncates when it writes half floats.** A diffusion error stored as
  −0.851020 comes back as −1742/2048. Every half-float bound in the harness is
  therefore one ulp (`halfUlp`), not the half-ulp that rounding would give. Cell means
  measured exactly one ulp off before this was understood.
- **GLSL division is allowed 2.5 ulp of error.** The Freeze resample did
  `floor( ( x + 0.5 ) * old / new )`, so at a 2:1 edge it picked either cell. The
  harness had skipped every cell "too near an edge to call", all 14 400 of them, so
  the check was vacuous. The shader now uses the exact integer law
  `( ( 2x + 1 ) * old ) / ( 2 * new )`. The harness uses the same law, counts the
  on-edge cells and judges them, and gets 0 wrong.
- **Near-ties are measured, not guessed.** A fixed "too close to call" margin skipped
  5% of cells on the C64 and 80% under Bayer 8. `nearTie()` perturbs the input by the
  error it can carry (`kFloatSum`, `kMatrixRoute`) and asks whether the decision can
  flip. Skips are now 0.3% for `--nearest` and at most 17.5% for Bayer 8 tone on
  colour palettes, where 64 candidates are each a chance to tie.
- **A reference built from the plugin's own function can't fail.** The harness first
  took Knuth's class matrix from `Dither.h` and τ from `AdaptFromParam`, so a mutant
  in either passed everything. It now has its own transcription of the matrix (from
  US 8,077,355 B2's Table 2) and its own τ = 0.3, plus a check that the plugin's
  matrix equals it. It also has its own OKLab route through XYZ, its own Bayer matrix,
  its own Knoll candidates and its own DAC rounding.
- **"Settled" isn't two equal frames.** `--still` on Adaptive took two equal pictures
  as settled while the one-pole was still moving entries under a code. It now waits
  until the plugin's own shown palette equals its target and the target stopped
  moving.
- **Mutants that survived.**
  - Ripple at (i + 2)/(N + 2) passed a three-point check, so Ripple is now judged at
    nine points.
  - A DAC rounding tie the wrong way passed a grid check, so `--dac` now also requires
    PICO-8's sixteen to land on the nearest level.
- **The opaque look card hid Alpha.** Alpha did nothing measurable on an opaque card,
  so the sweep called it dead. `--card alpha` has a half-covered third and an uncovered
  sixth.
- **`near` and `far` are MSVC macros**, and `verify.sh` greps for them. BSD sed has no
  `\b`, so renames went through perl.

## Verified, and assumed

Verified on this Mac (numbers in README Status): everything `tools/verify.sh` runs.

Assumed, not measured:

- **Resolume's input arrives sRGB-encoded and premultiplied.** The plugin linearises
  with IEC 61966-2-1 and un-premultiplies per cell. A host that hands over linear or
  straight alpha would be read wrongly.
- **The host's beat.** Cycle Sync reads `SetBeatInfo`'s bar phase and BPM, as the
  fleet's other plugins do. It hasn't been measured in Arena.
- **Dot Diffusion's tone runs light.** On a two-colour palette over flat dark greys it
  drifts: 0.03 → 0.12 and 0.18 → 0.25 in linear light. `--diffusion` accounts for
  every bit of it: the barons' error, what goes off the edges, and what the clamp cut.
  It's the clamp. The decision is in OKLab, whose black–white midpoint is 0.125 in
  linear light, so a dark cell that tips white leaves −0.875 of error, and the next
  cell's clamp to [0, 1] throws most of it away. On real palettes it's small: on the
  clip, PICO-8's and the C64's mean light is within 2% of the clip's for every dither.
  Carrying the error unclamped (OKLab's cube root already takes negative light) is the
  obvious fix. It wasn't made, because it changes the recurrence the harness checks
  and needs its own bounds.
- **OKLab on the GPU** is `sign( x ) * pow( |x|, 1/3 )` in float. Against the harness's
  route through XYZ in double it's within 8.7e-5 over 4096 colours, and the near-tie
  margins carry that (`kMatrixRoute` = 1e-4).

## Would this hold on another rasteriser, at another raster?

Everything below runs at 1280×720 and 320×180, and again on Apple's software renderer
at 320×180, in `verify.sh`.

- **`--members`, `--swap`, `--cycle`, `--freeze`, `--dac`:** compare 8-bit outputs with
  the table the plugin uploaded, or with each other. These are exact and
  raster-independent.
- **`--nearest`, `--tone`, `--diffusion`:** read the GPU's own cell means and errors
  back, and judge each decision against the harness's double-precision model. Decisions
  within the input's rounding of a tie are counted and skipped. The rounding bound is a
  half-float ulp plus float sums, and on a rasteriser that rounds instead of
  truncating it would only be looser than needed.
- **`--still`, `--clock`, `--mix` at 0:** bit for bit.
- **`--resize`:** the integer law, exact anywhere.
- **`--adaptive`:** the plugin's own state, in double, plus the output for the slats.
- **No GL:** `--palettes`, `--custom`, `--oklab`, `--bluenoise` and `--names`.

## The negative controls and mutants

Each check fails against a wrong model. `--negative` has 11:

- nearest by RGB distance;
- Knoll's error kept in OKLab instead of light;
- a dither threshold that moves with the frame;
- Knuth's weights swapped;
- a Cut that re-quantises to B;
- Forward and Reverse turned round;
- the cycle's phase from the host's float time;
- a fresh median cut every frame, shown at once;
- the block mean for Adaptive's samples;
- a resize that clears the frozen indices;
- a white-noise mask.

`tools/mutate.sh` changes one character of the shipped GLSL or C++ 35 times (19 GLSL,
16 C++) and requires the named check to fail. All 35 are caught. They cover:

- **The clip:** decoded at gamma 2.5.
- **OKLab:** its matrices, cube root and distance, on the GPU and the CPU.
- **Pattern dither:** Bayer's rank and candidate count, blue noise's rank shift, and
  Knoll's error, attempt and rank side.
- **Dot Diffusion:** the donor's tile, the error kept, the gather's row, and Knuth's
  weight and class matrix.
- **Output:** Alpha's coverage both ways, Mix, and the resample's half cell.
- **Adaptive:** the like-radius and the one-pole's step.
- **Swap:** its partner, Fade and Ripple.
- **Cycle:** its direction, Ping-Pong's way back, the step count, beats to the bar and
  Cycle Rate's mapping.
- **The DAC:** its widening and its rounding.

**The recorded GLSL mutation** (the brief asks for one written down): in the quantise
pass, Knoll's error `error += t - PalLin[ e ];` becomes `error += t + PalLin[ e ];`.
`--tone` then fails with "320x180 Bayer 2 on black and white: 20160 of 57600 cells
wrong", and every Bayer and Blue Noise case after it fails the same way. Two others,
for the record:

- OKLab's gamma 2.4 → 2.5 fails `--nearest` with "means within 401.55 of a half's ulp".
- The gather's row 8 → 9 fails `--diffusion` with "24061 cells gathered wrongly".

## The sweep's honest list

`tools/sweep.py` sweeps all 21 controls, each in a context where it should act, and
finds all of them live:

| Context | Controls |
| --- | --- |
| Palette=Adaptive | Colours, Adapt |
| Palette=Custom | Custom |
| Swap=1 | Swap To, Swap Style |
| Cycle on | the Cycle group |
| `--card alpha` | Alpha |
| Freeze pressed at frame 75 | Freeze |

`--bare` (no context) lists 12 as dead: Colours, Adapt, Custom, Order, Swap To, Swap
Style, Cycle Rate, Cycle Sync, Cycle Start, Cycle Length, Cycle Blend and Alpha. Each
only acts in its context: Order only matters once something reads the index's order (a
swap or a cycle), and Alpha only on a clip with coverage below 1. That run exits 1 on
purpose.

## Not done

- **Never loaded into Resolume**, on either platform.
  - Never built for Windows, so `tools/wincheck.ps1` is untested.
  - No Arena gate expectation in plugin-bench.
- No browser demo, presets or user guide yet.
- **Not built:**
  - several cycle ranges at once;
  - Floyd–Steinberg proper (serial both ways);
  - a PBO read-back a frame late for Adaptive;
  - unclamped dot diffusion.
- **Open questions for Allan:**
  - whether to fix Dot Diffusion's tone (above);
  - whether the fleet should settle on one C64 table (Pepto 2001 or nesolume's);
  - several CRNG ranges with their own rates;
  - whether Adaptive's default Adapt should be shorter than 0.3 s for strobing clips.
