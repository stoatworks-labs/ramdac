# ramdac

Indexed colour for Resolume Arena/Avenue, as an FFGL effect. The clip becomes an index
buffer and a 16-entry colour table. Each cell is quantised once to a palette entry's index
in OKLab, and the picture is the table looked up by index. Swaps, fades and Deluxe Paint
colour cycling are writes to the table, so the indices never move. C++/GLSL, CMake MODULE →
universal `.bundle` (macOS). MIT. ID `RM01`, display name `SW Ramdac`.

Read `AGENTS.md` before touching the quantiser, the dithers, the adaptive palette, the
table's swap/cycle arithmetic, or any tolerance in the harness.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (writes into Arena's Extra Effects; don't run it from an agent)
- Render offline: `./build/rmtest --out /tmp/frame.png` (the look card, 60 frames in)
- List parameters: `./build/rmtest --list`
- Set anything by name: `./build/rmtest --set "Palette=Commodore 64" --set "Dither=Blue Noise"`
- Press something later: `./build/rmtest --at 30 "Freeze=1"`
- A cycling strip: `./build/rmtest --strip 8 --out /tmp/cycle.png`
- A clip through it: `ffmpeg -i in.mov -vf fps=60 -f rawvideo -pix_fmt rgba - | ./build/rmtest --pipe --size WxH | ffmpeg …`
- The README's figures: `python3 tools/figures.py` (Arena's bundled `IntoTheGlow_02`, ffmpeg, Pillow)

## Verify
- Everything: `tools/verify.sh` takes about eight minutes. It runs:
  - greps for GLSL reserved words, trig and anything WebGL2 lacks;
  - `glslc` on all 9 shaders;
  - the FFGL pin, then a fresh universal build;
  - lipo, plist, version, and an ad-hoc signature;
  - `oxbow probe` and `selftest`;
  - every check at 1280x720 and 320x180, and again on Apple's software renderer;
  - the offline checks and the blue-noise regeneration;
  - `tools/siblings.py`;
  - the negative controls, 35 mutants, the sweep and the bench.

  `OXBOW=` points at an oxbow binary if `~/Projects/resolume/oxbow/build/oxbow` isn't built.
- **The quantiser**:
  - `--members`: every pixel is a table entry;
  - `--nearest`: the OKLab-nearest index, by the harness's own route;
  - `--tone`: Knoll's counts and Bayer's order;
  - `--diffusion`: Knuth's recurrence, cell by cell;
  - `--still`: frames bit for bit.
- **The table**:
  - `--swap`: Cut keeps every index, Fade is per index on the OKLab segment, Ripple runs in order;
  - `--cycle`: each step a rotation, a period returns, beat and bar;
  - `--clock`: a 499-million-ms host clock;
  - `--adaptive`: bounded settling, 0.002 in OKLab a frame under noise, never worse than median cut, thin white slats kept;
  - `--dac`: the nearest level of each grid.
- **The rest**: `--freeze`, `--resize`, `--alpha`, `--mix`, `--state`.
- **No GL**: `--palettes`, `--custom`, `--oklab`, `--bluenoise`, `--names`; `--offline` runs all five.
- The checks can fail: `--negative` (11 wrong models), `tools/mutate.sh` (35 mutants; `tools/mutate.sh PATTERN` runs only the ones whose description matches).
- No dead controls: `python3 tools/sweep.py` (21 swept, all live; `--bare` lists the 12 that only act in a context).
- Shared colours: `python3 tools/siblings.py` (teletext, copperlist and nesolume, when they're checked out).
- Render cost: `./build/rmtest --bench`.
- No CI and no release workflow: the repo is local-only, with no remote (AGENTS.md says why). `tools/wincheck.ps1` loads a built DLL as a host does. It checks the name, ID, type and parameter count; `-Params 25` is the 21 controls plus the About text and three buttons. Change it whenever the control table changes.

## Notes
- **The table is built on the CPU every frame** (`Clut.h`), in double, from the palette, the swap, the cycle and the DAC. The GPU only ever sees 16 RGB entries and an index texture.
- **Nothing in a dither depends on time.** Every rank is a pure function of the cell's position, so a still input gives a still picture. `kHookMovingDither` is the negative.
- **Apple's GPU truncates when it writes half floats**, so every half-float bound in the harness is one ulp (`halfUlp`), not half of one.
- **GLSL division is allowed 2.5 ulp of error**, so the freeze resample uses integer arithmetic; a float `floor()` at a 2:1 edge picks either cell.
- **Near-ties are measured, not guessed**: `nearTie()` perturbs the input by the error it could carry and asks whether the decision can flip.
- **A reference built from the plugin's own function can't fail.** The harness has its own Knuth matrix, its own OKLab route, its own τ and its own DAC rounding.
- All host parameters are 0..1 and mapped in `Controls.cpp`; options hold the element index. The option lists are append-only from v0.1.0.
- GLSL reserved words (`patch sample input output filter common active half layout flat packed`, `noise1`–`noise4`) must not be identifiers; `verify.sh` greps for them. `near` and `far` are MSVC macros; it greps for those too.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About line, or no host can instantiate the plugin. Custom is a real text control, parsed on the render thread.
- `ramdac_core` is an OBJECT library: the plugin registers itself from a file-scope constructor that nothing references.
- The macOS build must be universal. Check with `lipo`, never the build log.
- Hand-written for now, until ramdac is registered in stoatworks-backend: `source/StoatworksAbout.h` (provisional: page and repo links that don't exist yet, no guide) and `ATTRIBUTIONS.md`. Once it's registered, the backend's sync scripts generate both. `StoatworksAboutLinks.h` and `StoatworksAboutParams.h` are the fleet's hand-written FFGL half.

## Not done yet
- Never loaded into Resolume, on either platform. Never built for Windows.
- No browser demo, presets or user guide.

## Diagnostics

`source/Diag.{h,cpp}`: log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/ramdac/ramdac.YYYY-MM-DD.log
    %LOCALAPPDATA%\ramdac\logs\ramdac.YYYY-MM-DD.log

`RAMDAC_LOG_DIR` overrides the folder.
