/**
 * Ramdac — browser demo.
 *
 * Indexed colour, palette swap and Deluxe Paint colour cycling. The one idea,
 * from `AGENTS.md`: **the picture becomes an index buffer and a colour table**,
 * as an indexed display's was. The clip is averaged into cells, each cell is
 * quantised once to a palette entry's index in OKLab, and what is shown is a
 * 16-entry table looked up by index. Swap, fade, ripple, cycle and the DAC are
 * writes to the table; the indices never hear about them.
 *
 * Two halves, and they are not equally faithful here:
 *
 *   **The GPU half is the plugin's own GLSL.** `shaders.js` is the eleven
 *   `R"( ... )"` pieces of `source/Shaders.cpp` plus kVersion, spliced by
 *   `demo/tools/check_shaders.py --write` and never typed; the same script
 *   compares them character for character, and checks they are joined as
 *   `Assemble()` and InitGL join them, and `tools/verify.sh` runs it. They run
 *   as the plugin's passes -- grid, analysis (Adaptive only, read back),
 *   quantise or Dot Diffusion's 64 diffuse + settle draws and a gather, then
 *   display, and resample under Freeze -- on the plugin's RGBA16F and RGBA32F,
 *   which need EXT_color_buffer_float. `tables.js` is the plugin's data, spliced
 *   and checked the same way: the blue-noise mask, Knuth's class matrix, the
 *   eleven published palettes and the Custom default.
 *
 *   **The CPU half is a port.** Between the PORT markers below: Controls.cpp
 *   (names, groups, order, defaults, ranges and both square laws), the
 *   functions of Palettes.cpp (sRGB, OKLab both ways, the DAC, the Custom
 *   parser, Lightness order), Clut.cpp (swap partner, Cut, Fade, Ripple, the
 *   cycle and its clock), Adaptive.cpp (median cut, warm-started k-means, the
 *   one-pole) and Dither.cpp's plan for Knuth's dot diffusion, in double.
 *   Between the FRAME markers: the CPU half of Ramdac.cpp -- ProcessOpenGL,
 *   ensureIndices, uploadPalette, drawGrid, runAdaptive, drawQuantise,
 *   drawDiffusion and cycleUnits -- which turns the controls and the clock
 *   into each pass's uniforms and the table. `demo/tools/crosscheck/` renders
 *   the plugin's own harness (`rmtest --pipe`) and this page over the same
 *   frames and compares the pictures; it needs Chrome and a GPU, so it is run
 *   by hand (AGENTS.md).
 *
 * The trap this file is written around: the kit's `Program.set()` sends
 * glUniform1f, and on a GLSL `int` or `ivec2` that is an error the shader
 * never hears about -- the control is simply dead. Ramdac declares 24 int and
 * ivec2 uniforms and 12 samplers. Every uniform here goes through the typed
 * setters below, which read each program’s active uniforms back from WebGL
 * and THROW if a setter does not match the declared type.
 *
 * What the page cannot do as the plugin does, all said on the page: take its
 * clock and tempo from a host (the page's clock is seconds from frame deltas,
 * and Cycle Sync Beat and Bar count the plugin's own 120 BPM, which is what it
 * does in a host that sends none).
 */

import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import { mountDemo } from './vendor/demo.js';
import * as S from './shaders.js';
import * as T from './tables.js';

const f32 = Math.fround;
const clampTo = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

/** std::lround: half away from zero. */
const lround = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));

// ==== BEGIN PORT ===========================================================
//
// Controls.h / Controls.cpp. Every STANDARD host parameter is 0..1; the four
// counts are FF_TYPE_INTEGER and hold real integers in their own ranges;
// options hold the element index; Freeze is a boolean; Custom is text.

const PT = {
  PALETTE: 0, COLOURS: 1, ADAPT: 2, CUSTOM: 3, ORDER: 4, DAC: 5,
  DITHER: 6, DITHER_AMOUNT: 7, PIXEL_SIZE: 8,
  SWAP_TO: 9, SWAP: 10, SWAP_STYLE: 11,
  CYCLE: 12, CYCLE_RATE: 13, CYCLE_SYNC: 14, CYCLE_START: 15, CYCLE_LENGTH: 16, CYCLE_BLEND: 17,
  FREEZE: 18, ALPHA: 19, MIX: 20,
  ABOUT_FIRST: 21,
};

const Kind = { Standard: 0, Option: 1, Integer: 2, Boolean: 3, Text: 4 };

const PaletteId = {
  Adaptive: 0, Custom: 1, Teletext: 2, CgaMode4: 3, Cga16: 4, C64: 5, Spectrum: 6, Cpc: 7, Nes: 8, GameBoy: 9,
  Pico8: 10, Workbench13: 11, Workbench20: 12, Count: 13,
};
const Order = { Lightness: 0, Native: 1, Count: 2 };
const Dac = { Bits8: 0, Bits6: 1, Bits4: 2, Bits3: 3, Count: 4 };
const Dither = { None: 0, Bayer2: 1, Bayer4: 2, Bayer8: 3, BlueNoise: 4, DotDiffusion: 5, Count: 6 };
const SwapStyle = { Cut: 0, Fade: 1, Ripple: 2, Count: 3 };
const CycleMode = { Off: 0, Forward: 1, Reverse: 2, PingPong: 3, Count: 4 };
const CycleSync = { Free: 0, Beat: 1, Bar: 2, Count: 3 };
const CycleBlend = { Step: 0, Smooth: 1, Count: 2 };
const AlphaMode = { Keep: 0, Opaque: 1, Count: 2 };

const kMaxEntries = 16;

const kPaletteNames = ['Adaptive', 'Custom', 'Teletext', 'CGA Mode 4', 'CGA 16', 'Commodore 64', 'ZX Spectrum',
  'Amstrad CPC', 'NES', 'Game Boy', 'PICO-8', 'Workbench 1.3', 'Workbench 2.0'];
const kOrderNames = ['Lightness', 'Native'];
const kDacNames = ['24-bit', '18-bit', '12-bit', '9-bit'];
const kDitherNames = ['None', 'Bayer 2', 'Bayer 4', 'Bayer 8', 'Blue Noise', 'Dot Diffusion'];
const kSwapStyleNames = ['Cut', 'Fade', 'Ripple'];
const kCycleNames = ['Off', 'Forward', 'Reverse', 'Ping-Pong'];
const kCycleSyncNames = ['Free', 'Beat', 'Bar'];
const kCycleBlendNames = ['Step', 'Smooth'];
const kAlphaNames = ['Keep', 'Opaque'];
if (kPaletteNames.length !== PaletteId.Count) throw new Error('a name per palette');

const clamp01 = (v) => clampTo(v, 0.0, 1.0);

const kAdaptMost = 4.0;
const kRateMost = 60.0;

function AdaptFromParam(value) {
  const v = clamp01(value);
  return kAdaptMost * v * v;
}
const ParamFromAdapt = (seconds) => f32(Math.sqrt(clampTo(seconds / kAdaptMost, 0.0, 1.0)));

function CycleRateFromParam(value) {
  const v = clamp01(value);
  return kRateMost * v * v;
}
const ParamFromCycleRate = (steps) => f32(Math.sqrt(clampTo(steps / kRateMost, 0.0, 1.0)));

const standard = (id, name, group, value) => ({ id, name, group, kind: Kind.Standard, defaultValue: f32(value), options: null, lo: 0, hi: 0, text: null });
const option = (id, name, group, value, names) => ({ id, name, group, kind: Kind.Option, defaultValue: f32(value), options: names, lo: 0, hi: 0, text: null });
const integer = (id, name, group, value, lo, hi) => ({ id, name, group, kind: Kind.Integer, defaultValue: f32(value), options: null, lo, hi, text: null });
const boolean = (id, name, group, value) => ({ id, name, group, kind: Kind.Boolean, defaultValue: value ? 1.0 : 0.0, options: null, lo: 0, hi: 0, text: null });
const text = (id, name, group, value) => ({ id, name, group, kind: Kind.Text, defaultValue: 0.0, options: null, lo: 0, hi: 0, text: value });

// The defaults: PICO-8 through Bayer 4 at two pixels a cell; the swap waiting
// on the Commodore 64 with a fade; cycling off, at eight steps a second over
// the whole table when it is turned on. The Custom default is the plugin's
// own eight colours (tables.js, from Palettes.cpp, which check_shaders.py
// holds equal to Controls.cpp's).
const kTable = [
  option(PT.PALETTE, 'Palette', 'Palette', PaletteId.Pico8, kPaletteNames),
  integer(PT.COLOURS, 'Colours', 'Palette', 16, 2, kMaxEntries),
  standard(PT.ADAPT, 'Adapt', 'Palette', ParamFromAdapt(0.3)),
  text(PT.CUSTOM, 'Custom', 'Palette', T.kDefaultCustom),
  option(PT.ORDER, 'Order', 'Palette', Order.Lightness, kOrderNames),
  option(PT.DAC, 'DAC', 'Palette', Dac.Bits8, kDacNames),

  option(PT.DITHER, 'Dither', 'Dither', Dither.Bayer4, kDitherNames),
  standard(PT.DITHER_AMOUNT, 'Dither Amount', 'Dither', 1.0),
  integer(PT.PIXEL_SIZE, 'Pixel Size', 'Dither', 2, 1, 16),

  option(PT.SWAP_TO, 'Swap To', 'Swap', PaletteId.C64, kPaletteNames),
  standard(PT.SWAP, 'Swap', 'Swap', 0.0),
  option(PT.SWAP_STYLE, 'Swap Style', 'Swap', SwapStyle.Fade, kSwapStyleNames),

  option(PT.CYCLE, 'Cycle', 'Cycle', CycleMode.Off, kCycleNames),
  standard(PT.CYCLE_RATE, 'Cycle Rate', 'Cycle', ParamFromCycleRate(8.0)),
  option(PT.CYCLE_SYNC, 'Cycle Sync', 'Cycle', CycleSync.Free, kCycleSyncNames),
  integer(PT.CYCLE_START, 'Cycle Start', 'Cycle', 0, 0, kMaxEntries - 1),
  integer(PT.CYCLE_LENGTH, 'Cycle Length', 'Cycle', kMaxEntries, 2, kMaxEntries),
  option(PT.CYCLE_BLEND, 'Cycle Blend', 'Cycle', CycleBlend.Step, kCycleBlendNames),

  boolean(PT.FREEZE, 'Freeze', 'Output', false),
  option(PT.ALPHA, 'Alpha', 'Output', AlphaMode.Keep, kAlphaNames),
  standard(PT.MIX, 'Mix', 'Output', 1.0),
];
if (kTable.length !== PT.ABOUT_FIRST) throw new Error('every control needs a row in kTable');

const InfoOf = (id) => kTable[Math.min(id, PT.ABOUT_FIRST - 1)];

/** An option's index from the value the host stored. */
const OptionIndex = (value, count) => clampTo(lround(value), 0, count - 1);

/** An Integer control's value, rounded and clamped to its range. */
function IntegerOf(id, value) {
  const info = InfoOf(id);
  const v = Number.isFinite(value) ? value : info.defaultValue;
  return clampTo(lround(v), info.lo, info.hi);
}

// Palettes.h / Palettes.cpp. A palette is up to sixteen 8-bit sRGB colours in
// the machine's own order; the tables themselves are spliced into tables.js.

const rgb = (r, g, b) => ({ r, g, b });
const sameRgb = (x, y) => x.r === y.r && x.g === y.g && x.b === y.b;
const lab = (L, a, b) => ({ L, a, b });
const copyLab = (c) => lab(c.L, c.a, c.b);

function emptyPalette() {
  const entries = [];
  for (let i = 0; i < kMaxEntries; i += 1) entries.push(rgb(0, 0, 0));
  return { count: 0, entries };
}
function copyPalette(p) {
  return { count: p.count, entries: p.entries.map((c) => rgb(c.r, c.g, c.b)) };
}

/** static_cast< uint8_t >( v >> 16 ) and the rest. */
const hex = (v) => rgb((v >>> 16) & 255, (v >>> 8) & 255, v & 255);

/** A 12-bit $RGB register, ×17 a gun. */
const reg12 = (v) => rgb(((v >>> 8) & 15) * 17, ((v >>> 4) & 15) * 17, (v & 15) * 17);

function fromHex(values) {
  if (values.length > kMaxEntries) throw new Error('a palette is at most sixteen colours');
  const p = emptyPalette();
  p.count = values.length;
  values.forEach((v, i) => { p.entries[i] = hex(v); });
  return p;
}
function fromRegisters(values) {
  const p = emptyPalette();
  p.count = values.length;
  values.forEach((v, i) => { p.entries[i] = reg12(v); });
  return p;
}

function LinearOfCode(code) {
  return code <= 0.04045 ? code / 12.92 : Math.pow((code + 0.055) / 1.055, 2.4);
}
function CodeOfLinear(light) {
  return light <= 0.0031308 ? light * 12.92 : 1.055 * Math.pow(light, 1.0 / 2.4) - 0.055;
}

function OklabOfLinear(r, g, b) {
  const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return lab(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
    0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s);
}

function LinearOfOklab(c) {
  const l_ = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
  const m_ = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
  const s_ = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
  const l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
  return [
    4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
    -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
  ];
}

const OklabOf = (c) => OklabOfLinear(LinearOfCode(c.r / 255.0), LinearOfCode(c.g / 255.0), LinearOfCode(c.b / 255.0));

function Rgb8OfOklab(c) {
  const v = LinearOfOklab(c);
  const gun = (x) => lround(clampTo(CodeOfLinear(clampTo(x, 0.0, 1.0)), 0.0, 1.0) * 255.0);
  return rgb(gun(v[0]), gun(v[1]), gun(v[2]));
}

function DistanceSquared(x, y) {
  const dL = x.L - y.L, da = x.a - y.a, db = x.b - y.b;
  return dL * dL + da * da + db * db;
}

function DacBits(dac) {
  switch (dac) {
    case Dac.Bits6: return 6;
    case Dac.Bits4: return 4;
    case Dac.Bits3: return 3;
    default: return 8;
  }
}

function DacExpand(level, bits) {
  if (bits >= 8) return clampTo(level, 0, 255);
  // The level's bits written out again and again until eight are filled.
  let v = 0, filled = 0;
  while (filled < 8) {
    v = (v << bits) | level;
    filled += bits;
  }
  return v >> (filled - 8);
}

function DacRound(code, bits) {
  code = clampTo(code, 0, 255);
  if (bits >= 8) return code;
  let best = 0, bestError = 256;
  for (let level = 0; level < (1 << bits); level += 1) {
    const error = Math.abs(DacExpand(level, bits) - code);
    // A tie goes to the lower level.
    if (error < bestError) {
      bestError = error;
      best = level;
    }
  }
  return DacExpand(best, bits);
}
const DacRoundRgb = (c, bits) => rgb(DacRound(c.r, bits), DacRound(c.g, bits), DacRound(c.b, bits));

const kLibrary = new Map([
  [PaletteId.Teletext, fromHex(T.kTeletext)],
  [PaletteId.CgaMode4, fromHex(T.kCgaMode4)],
  [PaletteId.Cga16, fromHex(T.kCga16)],
  [PaletteId.C64, fromHex(T.kC64)],
  [PaletteId.Spectrum, fromHex(T.kSpectrum)],
  [PaletteId.Cpc, fromHex(T.kCpc)],
  [PaletteId.Nes, fromHex(T.kNes)],
  [PaletteId.GameBoy, fromHex(T.kGameBoy)],
  [PaletteId.Pico8, fromHex(T.kPico8)],
  [PaletteId.Workbench13, fromRegisters(T.kWorkbench13)],
  [PaletteId.Workbench20, fromRegisters(T.kWorkbench20)],
]);
const kEmpty = emptyPalette();

/** A fixed palette in its machine's order; Adaptive and Custom have none. */
const LibraryPalette = (id) => kLibrary.get(id) ?? kEmpty;

function hexDigit(c) {
  if (c >= '0' && c <= '9') return c.charCodeAt(0) - 48;
  if (c >= 'a' && c <= 'f') return c.charCodeAt(0) - 97 + 10;
  if (c >= 'A' && c <= 'F') return c.charCodeAt(0) - 65 + 10;
  return -1;
}

function parseColour(token) {
  if (token.length > 0 && (token[0] === '#' || token[0] === '$')) token = token.slice(1);
  else if (token.length > 2 && token[0] === '0' && (token[1] === 'x' || token[1] === 'X')) token = token.slice(2);
  if (token.length !== 6 && token.length !== 3) return null;
  let v = 0;
  for (const c of token) {
    const d = hexDigit(c);
    if (d < 0) return null;
    v = ((v << 4) | d) >>> 0;
  }
  return token.length === 6 ? hex(v) : reg12(v);
}

/** std::isspace in the C locale. */
const isSpace = (ch) => ch === ' ' || ch === '\t' || ch === '\n' || ch === '\v' || ch === '\f' || ch === '\r';

/**
 * Custom's text: up to sixteen colours, `#RRGGBB`, `RRGGBB`, `0xRRGGBB`, `$RGB`
 * or `#RGB`, separated by spaces, commas or semicolons. Fewer than two is not a
 * palette and gives the default. The C++ works on bytes; a non-ASCII character
 * here is a UTF-16 unit, which can only ever make a token not a colour, as its
 * bytes would.
 */
function ParseCustom(input) {
  const p = emptyPalette();
  let bad = 0;
  let token = '';
  const flush = () => {
    if (token.length === 0) return;
    const c = parseColour(token);
    if (c && p.count < kMaxEntries) p.entries[p.count++] = c;
    else bad += 1;
    token = '';
  };
  for (const ch of input) {
    if (isSpace(ch) || ch === ',' || ch === ';') flush();
    else token += ch;
  }
  flush();
  const fallback = p.count < 2;
  if (fallback) return { palette: ParseCustom(T.kDefaultCustom).palette, rejected: bad, usedDefault: true };
  return { palette: p, rejected: bad, usedDefault: false };
}

/** A permutation of 0..count-1, darkest first by OKLab L; equal L keeps the machine's order. */
function LightnessOrder(p) {
  const order = [];
  const lightness = [];
  for (let i = 0; i < kMaxEntries; i += 1) {
    order.push(i);
    lightness.push(i < p.count ? OklabOf(p.entries[i]).L : 0.0);
  }
  // Array.prototype.sort is stable (ES2019), as std::stable_sort is.
  const head = order.slice(0, p.count).sort((x, y) => (lightness[x] < lightness[y] ? -1 : lightness[y] < lightness[x] ? 1 : 0));
  return head.concat(order.slice(p.count));
}

function Ordered(p, order) {
  if (order === Order.Native) return p;
  const permutation = LightnessOrder(p);
  const out = emptyPalette();
  out.count = p.count;
  for (let i = 0; i < p.count; i += 1) out.entries[i] = p.entries[permutation[i]];
  return out;
}

function ThroughDac(p, bits) {
  const out = copyPalette(p);
  for (let i = 0; i < p.count; i += 1) out.entries[i] = DacRoundRgb(p.entries[i], bits);
  return out;
}

// Clut.h / Clut.cpp: the colour table, rebuilt every frame in double.

function fade(from, to, t, dacBits) {
  if (t <= 0.0) return from;
  if (t >= 1.0) return to;
  const x = OklabOf(from), y = OklabOf(to);
  const m = lab(x.L + t * (y.L - x.L), x.a + t * (y.a - x.a), x.b + t * (y.b - x.b));
  return DacRoundRgb(Rgb8OfOklab(m), dacBits);
}

/** C++'s %, which keeps the sign of v, made positive. */
function wrap(v, n) {
  const r = v % n;
  return r < 0 ? r + n : r;
}

/** ⌊(i + ½)·M/N⌋ in integers: ⌊(2i + 1)·M / 2N⌋. */
function SwapPartner(i, n, m) {
  if (n <= 0 || m <= 0) return 0;
  return clampTo(Math.trunc(((2 * i + 1) * m) / (2 * n)), 0, m - 1);
}

function CycleOffset(mode, step, length) {
  if (length < 2) return 0;
  switch (mode) {
    case CycleMode.Forward: return wrap(step, length);
    case CycleMode.Reverse: return wrap(-step, length);
    case CycleMode.PingPong: {
      const period = 2 * (length - 1);
      const p = wrap(step, period);
      return p <= length - 1 ? p : period - p;
    }
    default: return 0;
  }
}

const kPhaseSnap = 1e-6;
const WholeSteps = (phase) => Math.floor(phase + kPhaseSnap);

function BuildTable(input) {
  const out = { count: 0, entries: emptyPalette().entries, step: 0, fraction: 0.0, lo: 0, hi: 0 };
  const n = clampTo(input.a.count, 0, kMaxEntries);
  out.count = n;
  if (n === 0) return out;

  // Swap, index by index.
  const base = emptyPalette().entries;
  const s = clampTo(input.swap, 0.0, 1.0);
  for (let i = 0; i < n; i += 1) {
    const a = input.a.entries[i];
    if (input.b.count <= 0 || s <= 0.0) {
      base[i] = a;
      continue;
    }
    const b = input.b.entries[SwapPartner(i, n, input.b.count)];
    let c = a;
    switch (input.style) {
      case SwapStyle.Cut: c = s >= 0.5 ? b : a; break;
      case SwapStyle.Fade: c = fade(a, b, s, input.dacBits); break;
      case SwapStyle.Ripple: c = s * (n + 1) >= i + 1 ? b : a; break;
      default: break;
    }
    base[i] = c;
  }

  // Cycle, register by register.
  out.entries = base.slice();
  out.lo = clampTo(input.start, 0, n);
  out.hi = clampTo(input.start + input.length, out.lo, n);
  const length = out.hi - out.lo;
  if (input.cycle === CycleMode.Off || length < 2) return out;

  const mode = input.cycle;
  out.step = WholeSteps(input.phase);
  out.fraction = clampTo(input.phase - out.step, 0.0, 1.0);
  const now = CycleOffset(mode, out.step, length);
  const next = CycleOffset(mode, out.step + 1, length);
  const smooth = input.blend === CycleBlend.Smooth && out.fraction > 0.0;
  for (let k = 0; k < length; k += 1) {
    const here = base[out.lo + wrap(k - now, length)];
    let shown = here;
    if (smooth) shown = fade(here, base[out.lo + wrap(k - next, length)], out.fraction, input.dacBits);
    out.entries[out.lo + k] = shown;
  }
  return out;
}

/** The cycle's phase in steps, kept in double. */
class CycleClock {
  constructor() { this.phase = 0.0; }
  Advance(rate, units) { this.phase += rate * units; }
  Phase() { return this.phase; }
  Reset() { this.phase = 0.0; }
}

// Adaptive.h / Adaptive.cpp: the palette chosen from the clip, in double.

const axisOf = (c, axis) => (axis === 0 ? c.L : axis === 1 ? c.a : c.b);
const byAxis = (va, vb) => (va < vb ? -1 : vb < va ? 1 : 0);

function measure(samples, box) {
  let w = 0.0;
  const m = [0.0, 0.0, 0.0];
  const lo = [1e30, 1e30, 1e30], hi = [-1e30, -1e30, -1e30];
  for (const i of box.members) {
    const s = samples[i];
    w += s.weight;
    for (let a = 0; a < 3; a += 1) {
      const v = axisOf(s.lab, a);
      m[a] += s.weight * v;
      lo[a] = Math.min(lo[a], v);
      hi[a] = Math.max(hi[a], v);
    }
  }
  box.sse = 0.0;
  box.splittable = false;
  if (w <= 0.0) return;
  for (let a = 0; a < 3; a += 1) m[a] /= w;
  for (const i of box.members) {
    const s = samples[i];
    for (let a = 0; a < 3; a += 1) {
      const d = axisOf(s.lab, a) - m[a];
      box.sse += s.weight * d * d;
    }
  }
  box.axis = 0;
  for (let a = 1; a < 3; a += 1) if (hi[a] - lo[a] > hi[box.axis] - lo[box.axis]) box.axis = a;
  box.splittable = box.members.length >= 2 && hi[box.axis] > lo[box.axis];
}

function meanOf(samples, members) {
  let w = 0.0;
  const m = lab(0.0, 0.0, 0.0);
  for (const i of members) {
    const s = samples[i];
    w += s.weight;
    m.L += s.weight * s.lab.L;
    m.a += s.weight * s.lab.a;
    m.b += s.weight * s.lab.b;
  }
  if (w > 0.0) {
    m.L /= w;
    m.a /= w;
    m.b /= w;
  }
  return m;
}

/** The nearest centroid and its squared distance; a tie goes to the lower entry. */
function nearestOf(c, centroids) {
  let best = 0;
  let bestValue = Infinity;
  for (let j = 0; j < centroids.length; j += 1) {
    const d = DistanceSquared(c, centroids[j]);
    if (d < bestValue) {
      bestValue = d;
      best = j;
    }
  }
  return { index: best, distance: bestValue };
}

const sortDarkestFirst = (c) => c.sort((x, y) => byAxis(x.L, y.L));

/** `fresh` renumbered so each of `old`'s entries keeps the nearest fresh one not yet taken, closest pairs first. */
function matchTo(old, fresh) {
  const n = fresh.length;
  const out = [];
  for (let i = 0; i < n; i += 1) out.push(lab(0.0, 0.0, 0.0));
  const oldTaken = new Array(n).fill(false), freshTaken = new Array(n).fill(false);
  for (let round = 0; round < n; round += 1) {
    let best = Infinity;
    let bo = 0, bf = 0;
    for (let o = 0; o < n && o < old.length; o += 1) {
      if (oldTaken[o]) continue;
      for (let f = 0; f < n; f += 1) {
        if (freshTaken[f]) continue;
        const d = DistanceSquared(old[o], fresh[f]);
        if (d < best) {
          best = d;
          bo = o;
          bf = f;
        }
      }
    }
    if (best === Infinity) break;
    oldTaken[bo] = true;
    freshTaken[bf] = true;
    out[bo] = copyLab(fresh[bf]);
  }
  return out;
}

function KMeansCost(samples, centroids) {
  if (centroids.length === 0) return 0.0;
  let total = 0.0;
  for (const s of samples) total += s.weight * nearestOf(s.lab, centroids).distance;
  return total;
}

function MedianCut(samples, k) {
  const all = { members: [], sse: 0.0, axis: 0, splittable: false };
  for (let i = 0; i < samples.length; i += 1) if (samples[i].weight > 0.0) all.members.push(i);
  if (all.members.length === 0 || k < 1) return [];
  measure(samples, all);
  const boxes = [all];
  while (boxes.length < k) {
    let pick = -1;
    for (let b = 0; b < boxes.length; b += 1) {
      if (boxes[b].splittable && (pick < 0 || boxes[b].sse > boxes[pick].sse)) pick = b;
    }
    if (pick < 0) break;
    const box = boxes[pick];
    const axis = box.axis;
    box.members.sort((x, y) => byAxis(axisOf(samples[x].lab, axis), axisOf(samples[y].lab, axis)));
    let total = 0.0;
    for (const i of box.members) total += samples[i].weight;
    // The weighted median: the first sample at which half the weight is
    // behind. Never a split that leaves a side empty, and never one between
    // two equal values.
    let cut = 1;
    let run = 0.0;
    for (let m = 0; m + 1 < box.members.length; m += 1) {
      run += samples[box.members[m]].weight;
      cut = m + 1;
      if (run >= 0.5 * total) break;
    }
    const valueAt = (m) => axisOf(samples[box.members[m]].lab, axis);
    while (cut < box.members.length && valueAt(cut) === valueAt(cut - 1)) cut += 1;
    if (cut >= box.members.length) {
      cut = box.members.length - 1;
      while (cut > 0 && valueAt(cut) === valueAt(cut - 1)) cut -= 1;
    }
    const upper = { members: box.members.slice(cut), sse: 0.0, axis: 0, splittable: false };
    box.members.length = cut;
    measure(samples, box);
    measure(samples, upper);
    boxes.push(upper);
  }
  const out = boxes.map((box) => meanOf(samples, box.members));
  sortDarkestFirst(out);
  return out;
}

function RefineKMeans(samples, centroids, maxRounds) {
  const k = centroids.length;
  if (k === 0 || samples.length === 0) return 0;
  const owner = new Array(samples.length).fill(-1);
  let round = 0;
  for (; round < maxRounds; round += 1) {
    let changed = false;
    for (let i = 0; i < samples.length; i += 1) {
      const j = samples[i].weight > 0.0 ? nearestOf(samples[i].lab, centroids).index : -1;
      changed = changed || j !== owner[i];
      owner[i] = j;
    }
    if (!changed && round > 0) break;
    const w = new Array(k).fill(0.0);
    const sum = [];
    for (let j = 0; j < k; j += 1) sum.push(lab(0.0, 0.0, 0.0));
    for (let i = 0; i < samples.length; i += 1) {
      if (owner[i] < 0) continue;
      const j = owner[i];
      w[j] += samples[i].weight;
      sum[j].L += samples[i].weight * samples[i].lab.L;
      sum[j].a += samples[i].weight * samples[i].lab.a;
      sum[j].b += samples[i].weight * samples[i].lab.b;
    }
    const used = new Array(samples.length).fill(false);
    for (let j = 0; j < k; j += 1) {
      if (w[j] > 0.0) {
        centroids[j] = lab(sum[j].L / w[j], sum[j].a / w[j], sum[j].b / w[j]);
        continue;
      }
      // Empty: re-seed at the sample farthest from its own centroid.
      let farDistance = -1.0;
      let farthest = 0;
      for (let i = 0; i < samples.length; i += 1) {
        if (owner[i] < 0 || used[i]) continue;
        const d = DistanceSquared(samples[i].lab, centroids[owner[i]]);
        if (d > farDistance) {
          farDistance = d;
          farthest = i;
        }
      }
      if (farDistance >= 0.0) {
        used[farthest] = true;
        centroids[j] = copyLab(samples[farthest].lab);
      }
    }
  }
  return round;
}

class Adaptive {
  static kSnap = 1e-3;
  static kMaxRounds = 16;

  constructor() {
    this.samples = [];
    this.target = [];
    this.shown = [];
    this.shown8 = emptyPalette();
    this.k = 0;
    this.cost = 0.0;
    this.medianCost = 0.0;
    this.rounds = 0;
    this.reseeded = false;
  }

  /** One frame. `rgba` is `count` cells of linear RGB and coverage. */
  Update(rgba, count, colours, tau, dt, dacBits) {
    const samples = [];
    for (let i = 0; i < count; i += 1) {
      const c3 = rgba[i * 4 + 3];
      const w = Number.isFinite(c3) ? clampTo(c3, 0.0, 1.0) : 0.0;
      if (w <= 0.0) continue;
      const r = clampTo(rgba[i * 4], 0.0, 1.0);
      const g = clampTo(rgba[i * 4 + 1], 0.0, 1.0);
      const b = clampTo(rgba[i * 4 + 2], 0.0, 1.0);
      samples.push({ lab: OklabOfLinear(r, g, b), weight: w });
    }
    this.samples = samples;
    colours = clampTo(colours, 2, kMaxEntries);

    let median = MedianCut(samples, colours);
    if (median.length === 0) median = [lab(0.0, 0.0, 0.0)]; // nothing covered: black
    // Fewer distinct colours than entries: the darkest repeated.
    while (median.length < colours) median.unshift(copyLab(median[0]));
    this.medianCost = KMeansCost(samples, median);
    this.reseeded = false;

    const fresh = this.k !== colours || this.target.length !== colours;
    if (fresh) {
      this.target = median.map(copyLab);
      this.rounds = RefineKMeans(samples, this.target, Adaptive.kMaxRounds);
      this.shown = this.target.map(copyLab);
      this.k = colours;
      this.reseeded = true;
    } else {
      let warm = this.target.map(copyLab);
      this.rounds = RefineKMeans(samples, warm, Adaptive.kMaxRounds);
      if (KMeansCost(samples, warm) > this.medianCost) {
        const refined = median.map(copyLab);
        this.rounds += RefineKMeans(samples, refined, Adaptive.kMaxRounds);
        warm = matchTo(this.target, refined);
        this.reseeded = true;
      }
      this.target = warm;

      // What is shown follows. Adapt 0 is at once.
      const alpha = tau > 0.0 ? 1.0 - Math.exp(-Math.max(dt, 0.0) / tau) : 1.0;
      for (let j = 0; j < this.target.length; j += 1) {
        const s = this.shown[j];
        const t = this.target[j];
        s.L += alpha * (t.L - s.L);
        s.a += alpha * (t.a - s.a);
        s.b += alpha * (t.b - s.b);
        if (DistanceSquared(s, t) <= Adaptive.kSnap * Adaptive.kSnap
          || sameRgb(DacRoundRgb(Rgb8OfOklab(s), dacBits), DacRoundRgb(Rgb8OfOklab(t), dacBits))) {
          s.L = t.L;
          s.a = t.a;
          s.b = t.b;
        }
      }
    }
    this.cost = KMeansCost(samples, this.target);

    this.shown8 = emptyPalette();
    this.shown8.count = this.shown.length;
    for (let j = 0; j < this.shown.length; j += 1) this.shown8.entries[j] = DacRoundRgb(Rgb8OfOklab(this.shown[j]), dacBits);
  }

  Shown() { return this.shown8; }
}

// Dither.h / Dither.cpp: the plan for Knuth's dot diffusion. The class matrix
// is spliced into tables.js, [y][x] flattened, row 0 the bottom.

const kClasses = 64;
const kKnuthClass = (x, y) => T.kKnuthClass[y * 8 + x];

const classAt = (x, y) => kKnuthClass(((x % 8) + 8) % 8, ((y % 8) + 8) % 8);

function weightOf(dx, dy) {
  const orthogonal = dx === 0 || dy === 0;
  return orthogonal ? 2 : 1;
}

/** A cell's weights over its neighbours of higher class. */
function outgoingWeight(x, y) {
  let total = 0;
  for (let dy = -1; dy <= 1; dy += 1) {
    for (let dx = -1; dx <= 1; dx += 1) {
      if ((dx || dy) && classAt(x + dx, y + dy) > classAt(x, y)) total += weightOf(dx, dy);
    }
  }
  return total;
}

const floorDiv8 = (v) => (v >= 0 ? Math.trunc(v / 8) : -Math.trunc((-v + 7) / 8));

function BuildPlan() {
  const plan = [];
  for (let c = 0; c < kClasses; c += 1) plan.push({ x: 0, y: 0, donorCount: 0, donors: [] });
  for (let y = 0; y < 8; y += 1) {
    for (let x = 0; x < 8; x += 1) {
      const p = plan[kKnuthClass(x, y)];
      p.x = x;
      p.y = y;
      for (let dy = -1; dy <= 1; dy += 1) {
        for (let dx = -1; dx <= 1; dx += 1) {
          if (!dx && !dy) continue;
          const nx = x + dx, ny = y + dy;
          const donor = classAt(nx, ny);
          if (donor >= kKnuthClass(x, y)) continue;
          p.donors.push({
            dx, dy, donorClass: donor, tileDx: floorDiv8(nx), tileDy: floorDiv8(ny),
            // The donor is lower, so this cell is one of its higher
            // neighbours and its total is never 0.
            share: weightOf(dx, dy) / outgoingWeight(nx, ny),
          });
          p.donorCount += 1;
        }
      }
    }
  }
  return plan;
}

// ==== END PORT =============================================================

// ==== BEGIN FRAME ==========================================================
//
// Ramdac.cpp, the CPU half of a frame. Each pass binds and draws as the
// plugin's does; `input` is the clip as the kit hands it over, the size of the
// composition, which is the host's viewport here (ViewOrigin 0, 0).

/** Seconds of clock a single frame may advance by. */
const kMaxFrameDelta = 0.25;

/**
 * The uniforms each program actually declares, read back from WebGL, so a
 * setter of the wrong type fails loudly instead of being a dead control. A
 * name the program does not have (or the compiler dropped) is a no-op, as
 * glUniform( -1 ) is, and is counted in `program.missing`.
 */
function uniformTypes(program) {
  if (!program.types) {
    const gl = program.gl;
    program.types = new Map();
    const count = gl.getProgramParameter(program.program, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < count; i += 1) {
      const info = gl.getActiveUniform(program.program, i);
      program.types.set(info.name.replace(/\[0\]$/, ''), info.type);
    }
  }
  return program.types;
}

function located(program, name, want) {
  const type = uniformTypes(program).get(name);
  if (type === undefined) {
    program.missing.add(name);
    return null;
  }
  if (!want.includes(type)) {
    throw new Error(`${program.label}: uniform ${name} is GL type 0x${type.toString(16)}, not what its setter sends`);
  }
  return program.location(name) ?? program.location(`${name}[0]`);
}

function makeSetters(gl) {
  const INT = [gl.INT];
  const IVEC2 = [gl.INT_VEC2];
  const FLOAT = [gl.FLOAT];
  const VEC3 = [gl.FLOAT_VEC3];
  const SAMPLER = [gl.SAMPLER_2D];
  return {
    int(p, name, v) { const l = located(p, name, INT); if (l) gl.uniform1i(l, v | 0); },
    int2(p, name, x, y) { const l = located(p, name, IVEC2); if (l) gl.uniform2i(l, x | 0, y | 0); },
    ints(p, name, values) { const l = located(p, name, INT); if (l) gl.uniform1iv(l, values); },
    int2s(p, name, values) { const l = located(p, name, IVEC2); if (l) gl.uniform2iv(l, values); },
    float(p, name, v) { const l = located(p, name, FLOAT); if (l) gl.uniform1f(l, v); },
    floats(p, name, values) { const l = located(p, name, FLOAT); if (l) gl.uniform1fv(l, values); },
    vec3s(p, name, values) { const l = located(p, name, VEC3); if (l) gl.uniform3fv(l, values); },
    sampler(p, name, unit) { const l = located(p, name, SAMPLER); if (l) gl.uniform1i(l, unit); },
  };
}

class RamdacInstance {
  constructor(gl, quad, programs, shared) {
    this.gl = gl;
    this.quad = quad;
    this.programs = programs;
    this.u = shared.setters;
    this.maskTexture = shared.mask;  // the blue-noise mask, R32F 64x64
    this.dummy = shared.dummy;       // a 1x1 texture for every unit a pass does not read

    this.params = new Float32Array(PT.ABOUT_FIRST);
    for (let id = 0; id < PT.ABOUT_FIRST; id += 1) this.params[id] = InfoOf(id).defaultValue;

    const nearest = { filter: 'nearest' };
    this.grid = new PassBuffer(gl, nearest);                                     // RGBA16F, a texel a cell
    this.analysis = new PassBuffer(gl, nearest);                                 // RGBA32F, at most 96x54
    this.indices = [new PassBuffer(gl, nearest), new PassBuffer(gl, nearest)];   // RGBA16F, the index and coverage
    this.classes = [new PassBuffer(gl, nearest), new PassBuffer(gl, nearest)];   // RGBA16F, Dot Diffusion's blocks
    this.currentIndex = 0;
    this.indicesValid = false;
    this.gridWidth = 0;
    this.gridHeight = 0;
    this.readback = new Float32Array(0);

    this.adaptive = new Adaptive();
    this.cycle = new CycleClock();
    this.lastTable = null;
    this.lastQuantised = emptyPalette();
    this.plan = BuildPlan();

    // Custom: the text the host last set, parsed on the render thread.
    this.customText = InfoOf(PT.CUSTOM).text;
    this.customDirty = true;
    this.customPalette = emptyPalette();
    this.customRejected = 0;
    this.customUsedDefault = false;

    // Time: the page's own clock, in seconds. No vote on the unit.
    this.now = 0.0;
    this.lastNow = -1.0;
    this.frameCount = 0;

    // The beat, as SetBeatInfo last said. No host here ever calls it.
    this.bpm = 120.0;
    this.barPhase = 0.0;
    this.lastBarPhase = -1.0;
    this.hostBeatSeen = false;
    this.cycleBar = -1.0;

    this.hooks = 0;
  }

  SetFloatParameter(index, value) {
    if (index >= PT.ABOUT_FIRST || index === PT.CUSTOM) return false;
    this.params[index] = value;
    return true;
  }

  SetTextParameter(index, value) {
    if (index !== PT.CUSTOM) return false;
    const incoming = value ?? '';
    if (incoming !== this.customText) {
      this.customText = incoming;
      this.customDirty = true;
    }
    return true;
  }

  /** Every unit a pass used, back to the dummy: WebGL2 refuses a draw whose
   * target is also bound to a sampler, and the next pass's target may be this
   * one's input. */
  unbindTextureUnits(count) {
    for (let unit = count - 1; unit >= 0; unit -= 1) bindTexture(this.gl, unit, this.dummy);
  }

  refreshCustom() {
    if (!this.customDirty) return;
    const parsed = ParseCustom(this.customText);
    this.customPalette = parsed.palette;
    this.customRejected = parsed.rejected;
    this.customUsedDefault = parsed.usedDefault;
    this.customDirty = false;
  }

  resolve(id, dacBits, order) {
    let p;
    if (id === PaletteId.Adaptive) p = this.adaptive.Shown();
    else if (id === PaletteId.Custom) p = this.customPalette;
    else p = LibraryPalette(id);
    if (p.count <= 0) {
      // Adaptive before it has seen a frame: one black entry, not nothing.
      p = emptyPalette();
      p.count = 1;
    }
    return Ordered(ThroughDac(p, dacBits), order);
  }

  ensureIndices(gw, gh, frozen) {
    const gl = this.gl;
    const current = this.indices[this.currentIndex];
    if (current.texture && gw === this.gridWidth && gh === this.gridHeight) return true;

    if (frozen && current.texture) {
      // A new grid under Freeze keeps the frozen picture, each new cell the
      // index of the old cell its centre falls in.
      const next = this.indices[1 - this.currentIndex];
      next.ensure(gw, gh, gl.RGBA16F);
      gl.bindFramebuffer(gl.FRAMEBUFFER, next.fbo);
      gl.viewport(0, 0, gw, gh);
      const resample = this.programs.resample;
      resample.use();
      bindTexture(gl, 0, current.texture);
      this.u.sampler(resample, 'Frozen', 0);
      this.u.int2(resample, 'OldSize', this.gridWidth, this.gridHeight);
      this.u.int2(resample, 'NewSize', gw, gh);
      this.quad.draw();
      this.unbindTextureUnits(1);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      current.dispose();
      this.currentIndex = 1 - this.currentIndex;
    } else {
      current.dispose();
      current.ensure(gw, gh, gl.RGBA16F);
      this.indicesValid = this.indicesValid && frozen;
    }
    this.gridWidth = gw;
    this.gridHeight = gh;
    return true;
  }

  uploadPalette(program, p) {
    const lin = new Float32Array(kMaxEntries * 3), labs = new Float32Array(kMaxEntries * 3), code = new Float32Array(kMaxEntries * 3);
    const order = new Int32Array(kMaxEntries);
    const lightness = LightnessOrder(p);
    for (let i = 0; i < kMaxEntries; i += 1) {
      order[i] = lightness[i];
      if (i >= p.count) continue;
      const c = p.entries[i];
      const cv = [c.r / 255.0, c.g / 255.0, c.b / 255.0];
      const l = OklabOf(c);
      const lv = [l.L, l.a, l.b];
      for (let k = 0; k < 3; k += 1) {
        lin[i * 3 + k] = LinearOfCode(cv[k]);
        code[i * 3 + k] = cv[k];
        labs[i * 3 + k] = lv[k];
      }
    }
    this.u.int(program, 'Count', Math.max(p.count, 1));
    this.u.vec3s(program, 'PalLin', lin);
    this.u.vec3s(program, 'PalLab', labs);
    this.u.vec3s(program, 'PalCode', code);
    this.u.ints(program, 'LightOrder', order);
    this.u.int(program, 'Hooks', this.hooks);
  }

  drawGrid(source, cell) {
    const gl = this.gl;
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.grid.fbo);
    gl.viewport(0, 0, this.gridWidth, this.gridHeight);
    const grid = this.programs.grid;
    grid.use();
    bindTexture(gl, 0, source.texture);
    this.u.sampler(grid, 'InputTexture', 0);
    this.u.int2(grid, 'Size', source.width, source.height);
    this.u.int(grid, 'Cell', cell);
    this.quad.draw();
    this.unbindTextureUnits(1);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  }

  runAdaptive(colours, dt, dacBits) {
    const gl = this.gl;
    const aw = this.analysis.width, ah = this.analysis.height;
    if (this.readback.length !== aw * ah * 4) this.readback = new Float32Array(aw * ah * 4);
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.analysis.fbo);
    gl.viewport(0, 0, aw, ah);
    const analysis = this.programs.analysis;
    analysis.use();
    bindTexture(gl, 0, this.grid.texture);
    this.u.sampler(analysis, 'Grid', 0);
    this.u.int(analysis, 'Hooks', this.hooks);
    this.u.int2(analysis, 'GridSize', this.gridWidth, this.gridHeight);
    this.u.int2(analysis, 'AnalysisSize', aw, ah);
    this.quad.draw();
    this.unbindTextureUnits(1);
    // Synchronous, as the plugin's is: RGBA32F read back as floats.
    gl.pixelStorei(gl.PACK_ALIGNMENT, 4);
    gl.readPixels(0, 0, aw, ah, gl.RGBA, gl.FLOAT, this.readback);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    this.adaptive.Update(this.readback, aw * ah, colours, AdaptFromParam(this.params[PT.ADAPT]), dt, dacBits);
    return true;
  }

  drawQuantise(p, dither, amount) {
    const gl = this.gl;
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.indices[this.currentIndex].fbo);
    gl.viewport(0, 0, this.gridWidth, this.gridHeight);
    const quantise = this.programs.quantise;
    quantise.use();
    bindTexture(gl, 0, this.grid.texture);
    bindTexture(gl, 1, this.maskTexture);
    this.u.sampler(quantise, 'Grid', 0);
    this.u.sampler(quantise, 'Mask', 1);
    this.u.int(quantise, 'DitherIndex', dither);
    this.u.float(quantise, 'Amount', amount);
    this.u.int(quantise, 'Frame', this.frameCount & 0xFFFF);
    this.uploadPalette(quantise, p);
    this.quad.draw();
    this.unbindTextureUnits(2);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  }

  drawDiffusion(p, amount) {
    const gl = this.gl;
    const tw = this.TilesWide(), th = this.TilesHigh();
    const blockOf = (cls) => [(cls & 7) * tw, (cls >> 3) * th];
    const diffuse = this.programs.diffuse;
    const settle = this.programs.settle;

    for (let cls = 0; cls < kClasses; cls += 1) {
      const c = this.plan[cls];
      const block = blockOf(cls);
      {
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.classes[1].fbo);
        gl.viewport(block[0], block[1], tw, th);
        diffuse.use();
        bindTexture(gl, 0, this.grid.texture);
        bindTexture(gl, 1, this.classes[0].texture);
        this.u.sampler(diffuse, 'Grid', 0);
        this.u.sampler(diffuse, 'Classes', 1);
        this.u.int2(diffuse, 'GridSize', this.gridWidth, this.gridHeight);
        this.u.int2(diffuse, 'ClassPos', c.x, c.y);
        this.u.int2(diffuse, 'BlockOrigin', block[0], block[1]);
        this.u.float(diffuse, 'Amount', amount);
        const offsets = new Int32Array(16), texels = new Int32Array(16);
        const shares = new Float32Array(8);
        for (let i = 0; i < c.donorCount; i += 1) {
          const d = c.donors[i];
          const from = blockOf(d.donorClass);
          offsets[i * 2] = d.dx;
          offsets[i * 2 + 1] = d.dy;
          texels[i * 2] = from[0] + d.tileDx;
          texels[i * 2 + 1] = from[1] + d.tileDy;
          shares[i] = d.share;
        }
        this.u.int(diffuse, 'DonorCount', c.donorCount);
        this.u.int2s(diffuse, 'DonorOffset', offsets);
        this.u.int2s(diffuse, 'DonorTexel', texels);
        this.u.floats(diffuse, 'DonorShare', shares);
        this.uploadPalette(diffuse, p);
        this.quad.draw();
        this.unbindTextureUnits(2);
      }
      {
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.classes[0].fbo);
        gl.viewport(block[0], block[1], tw, th);
        settle.use();
        bindTexture(gl, 0, this.classes[1].texture);
        this.u.sampler(settle, 'Fresh', 0);
        this.quad.draw();
        this.unbindTextureUnits(1);
      }
    }

    const classOf = new Int32Array(64);
    for (let y = 0; y < 8; y += 1) for (let x = 0; x < 8; x += 1) classOf[y * 8 + x] = kKnuthClass(x, y);
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.indices[this.currentIndex].fbo);
    gl.viewport(0, 0, this.gridWidth, this.gridHeight);
    const gather = this.programs.gather;
    gather.use();
    bindTexture(gl, 0, this.classes[0].texture);
    bindTexture(gl, 1, this.grid.texture);
    this.u.sampler(gather, 'Classes', 0);
    this.u.sampler(gather, 'Grid', 1);
    this.u.int2(gather, 'Tiles', tw, th);
    this.u.ints(gather, 'ClassOf', classOf);
    this.quad.draw();
    this.unbindTextureUnits(2);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  }

  TilesWide() { return Math.trunc((this.gridWidth + 7) / 8); }
  TilesHigh() { return Math.trunc((this.gridHeight + 7) / 8); }

  cycleUnits(sync, dt) {
    // Beats from the host's own bar when it sends one; otherwise its tempo on
    // our clock -- which, with no host to send one, is the plugin's 120 BPM.
    let beats = dt * Math.max(this.bpm, 1.0) / 60.0;
    if (this.hostBeatSeen) {
      let d = this.cycleBar >= 0.0 ? this.barPhase - this.cycleBar : 0.0;
      if (d < -0.5) d += 1.0;
      beats = 4.0 * Math.max(d, 0.0);
    }
    this.cycleBar = this.barPhase;
    switch (sync) {
      case CycleSync.Beat: return beats;
      case CycleSync.Bar: return beats / 4.0;
      default: return dt;
    }
  }

  /** ProcessOpenGL, with the page's clock in place of UpdateClock's vote. */
  ProcessOpenGL(source, time) {
    if (!source || source.width === 0 || source.height === 0) return;
    const gl = this.gl;
    const p = this.params;
    gl.disable(gl.BLEND);

    // Time and the beat.
    this.now = time;
    const dt = this.lastNow >= 0.0 ? clampTo(this.now - this.lastNow, 0.0, kMaxFrameDelta) : 0.0;
    this.lastNow = this.now;
    this.frameCount += 1;

    if (this.barPhase !== this.lastBarPhase && this.lastBarPhase >= 0.0) this.hostBeatSeen = true;
    this.lastBarPhase = this.barPhase;

    // The controls.
    this.refreshCustom();
    const cell = IntegerOf(PT.PIXEL_SIZE, p[PT.PIXEL_SIZE]);
    const { width, height } = source;
    const gw = Math.trunc((width + cell - 1) / cell);
    const gh = Math.trunc((height + cell - 1) / cell);
    const paletteA = OptionIndex(p[PT.PALETTE], PaletteId.Count);
    const paletteB = OptionIndex(p[PT.SWAP_TO], PaletteId.Count);
    const ditherMode = OptionIndex(p[PT.DITHER], Dither.Count);
    const order = OptionIndex(p[PT.ORDER], Order.Count);
    const dacBits = DacBits(OptionIndex(p[PT.DAC], Dac.Count));
    const amount = clampTo(p[PT.DITHER_AMOUNT], 0.0, 1.0);
    const swap = clampTo(p[PT.SWAP], 0.0, 1.0);
    const style = OptionIndex(p[PT.SWAP_STYLE], SwapStyle.Count);
    const frozen = p[PT.FREEZE] >= 0.5 && this.indicesValid;
    const adaptiveRun = !frozen && (paletteA === PaletteId.Adaptive || paletteB === PaletteId.Adaptive);

    // Every buffer before anything is drawn.
    this.ensureIndices(gw, gh, frozen);
    if (!frozen) this.grid.ensure(gw, gh, gl.RGBA16F);
    if (adaptiveRun) this.analysis.ensure(Math.min(gw, T.kAnalysisWidth), Math.min(gh, T.kAnalysisHeight), gl.RGBA32F);
    if (!frozen && ditherMode === Dither.DotDiffusion) {
      this.classes[0].ensure(8 * this.TilesWide(), 8 * this.TilesHigh(), gl.RGBA16F);
      this.classes[1].ensure(8 * this.TilesWide(), 8 * this.TilesHigh(), gl.RGBA16F);
    }

    // 1-2. The grid, and from it the adaptive palette.
    if (!frozen) this.drawGrid(source, cell);
    if (adaptiveRun) this.runAdaptive(IntegerOf(PT.COLOURS, p[PT.COLOURS]), dt, dacBits);

    const a = this.resolve(paletteA, dacBits, order);
    const b = this.resolve(paletteB, dacBits, order);

    // 3. The indices.
    if (!frozen) {
      this.lastQuantised = a;
      if (ditherMode === Dither.DotDiffusion) this.drawDiffusion(this.lastQuantised, amount);
      else this.drawQuantise(this.lastQuantised, ditherMode, amount);
      this.indicesValid = true;
    }

    // 4. The table, in double.
    const sync = OptionIndex(p[PT.CYCLE_SYNC], CycleSync.Count);
    const rate = CycleRateFromParam(p[PT.CYCLE_RATE]);
    this.cycle.Advance(rate, this.cycleUnits(sync, dt));

    this.lastTable = BuildTable({
      a,
      b,
      swap,
      style,
      cycle: OptionIndex(p[PT.CYCLE], CycleMode.Count),
      start: IntegerOf(PT.CYCLE_START, p[PT.CYCLE_START]),
      length: IntegerOf(PT.CYCLE_LENGTH, p[PT.CYCLE_LENGTH]),
      blend: OptionIndex(p[PT.CYCLE_BLEND], CycleBlend.Count),
      phase: this.cycle.Phase(),
      dacBits,
    });

    // 5. The picture, into the host's framebuffer and viewport.
    const table = new Float32Array(kMaxEntries * 3);
    for (let i = 0; i < this.lastTable.count; i += 1) {
      const c = this.lastTable.entries[i];
      table[i * 3] = c.r / 255.0;
      table[i * 3 + 1] = c.g / 255.0;
      table[i * 3 + 2] = c.b / 255.0;
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);
    const display = this.programs.display;
    display.use();
    bindTexture(gl, 0, this.indices[this.currentIndex].texture);
    bindTexture(gl, 1, source.texture);
    this.u.sampler(display, 'Indices', 0);
    this.u.sampler(display, 'InputTexture', 1);
    this.u.int2(display, 'Size', width, height);
    this.u.int2(display, 'ViewOrigin', 0, 0);
    this.u.int(display, 'Cell', cell);
    this.u.int(display, 'TableCount', Math.max(this.lastTable.count, 1));
    this.u.vec3s(display, 'Table', table);
    this.u.int(display, 'AlphaIndex', OptionIndex(p[PT.ALPHA], AlphaMode.Count));
    this.u.float(display, 'MixAmount', clampTo(p[PT.MIX], 0.0, 1.0));
    this.quad.draw();
    this.unbindTextureUnits(2);
    gl.activeTexture(gl.TEXTURE0);
  }

  dispose() {
    for (const b of [this.grid, this.analysis, ...this.indices, ...this.classes]) b.dispose();
    this.indicesValid = false;
    this.gridWidth = this.gridHeight = 0;
  }
}

// ==== END FRAME ============================================================

function createRenderer(gl, quad) {
  // InitGL: the vertex stage is kVersion + kQuadVertex; each fragment stage is
  // Assemble( pass ) = kVersion + kCommon (+ kPaletteCommon for the analysis,
  // quantise and diffuse passes) + the pass's fragment.
  const vertex = S.VERSION + S.QUAD_VERTEX;
  const assemble = (...pieces) => S.VERSION + S.COMMON + pieces.join('');
  const programs = {
    grid: new Program(gl, vertex, assemble(S.GRID_FRAGMENT), 'grid'),
    analysis: new Program(gl, vertex, assemble(S.PALETTE_COMMON, S.ANALYSIS_FRAGMENT), 'analysis'),
    quantise: new Program(gl, vertex, assemble(S.PALETTE_COMMON, S.QUANTISE_FRAGMENT), 'quantise'),
    diffuse: new Program(gl, vertex, assemble(S.PALETTE_COMMON, S.DIFFUSE_FRAGMENT), 'diffuse'),
    settle: new Program(gl, vertex, assemble(S.SETTLE_FRAGMENT), 'settle'),
    gather: new Program(gl, vertex, assemble(S.GATHER_FRAGMENT), 'gather'),
    display: new Program(gl, vertex, assemble(S.DISPLAY_FRAGMENT), 'display'),
    resample: new Program(gl, vertex, assemble(S.RESAMPLE_FRAGMENT), 'resample'),
  };

  // InitGL's blue-noise mask: ranks 0..4095 as floats, exact, R32F, nearest,
  // repeating (texelFetch ignores both, as the plugin's does).
  const mask = gl.createTexture();
  const ranks = new Float32Array(T.kBlueNoise);
  gl.bindTexture(gl.TEXTURE_2D, mask);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, T.kBlueNoiseSize, T.kBlueNoiseSize, 0, gl.RED, gl.FLOAT, ranks);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);

  // Not the plugin's: WebGL2 has no "nothing bound", only a warning, and a
  // feedback loop is an error, so every unit a pass is done with gets this.
  const dummy = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, dummy);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([0, 0, 0, 255]));
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.bindTexture(gl.TEXTURE_2D, null);

  const shared = { setters: makeSetters(gl), mask, dummy };
  const instance = new RamdacInstance(gl, quad, programs, shared);

  /** A setting by the plugin's display name, as `rmtest --set` takes it. */
  function applySetting(run, name, value) {
    const info = kTable.find((row) => row.name === name);
    if (!info) throw new Error(`no parameter called '${name}'`);
    if (info.kind === Kind.Text) {
      run.SetTextParameter(info.id, String(value));
      return;
    }
    const index = info.options ? info.options.indexOf(value) : -1;
    if (index >= 0) {
      run.SetFloatParameter(info.id, f32(index));
      return;
    }
    const n = Number(value);
    if (String(value).trim() === '' || !Number.isFinite(n)) throw new Error(`'${value}' is not a number or an option of ${name}`);
    run.SetFloatParameter(info.id, f32(n));
  }

  // For demo/tools/crosscheck/: a FRESH instance (rmtest's is one too) run over
  // the given frames on the harness's clock, frame / fps seconds, and read back
  // as RGBA8, top row first, as `rmtest --pipe` writes it. It runs in one task,
  // so the page's own frames cannot interleave; the next one puts the canvas
  // back to the composition's size.
  window.ramdacDemo = {
    renderCase({ settings = {}, frames, width, height, fps = 60, count }) {
      const run = new RamdacInstance(gl, quad, programs, shared);
      for (const [name, value] of Object.entries(settings)) applySetting(run, name, value);
      const texture = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, width, height);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      const canvas = gl.canvas;
      canvas.width = width;
      canvas.height = height;
      const row = width * 4;
      const frameBytes = row * height;
      const flipped = new Uint8Array(frameBytes);
      const total = count ?? frames.length / frameBytes;
      for (let f = 0; f < total; f += 1) {
        const frame = frames.subarray(f * frameBytes, (f + 1) * frameBytes);
        for (let y = 0; y < height; y += 1) flipped.set(frame.subarray(y * row, (y + 1) * row), (height - 1 - y) * row);
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, flipped);
        gl.bindTexture(gl.TEXTURE_2D, null);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, width, height);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        run.ProcessOpenGL({ texture, width, height }, f / fps);
      }
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
      gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, flipped);
      const out = new Uint8Array(frameBytes);
      for (let y = 0; y < height; y += 1) out.set(flipped.subarray(y * row, (y + 1) * row), (height - 1 - y) * row);
      run.dispose();
      gl.deleteTexture(texture);
      return out;
    },

    /** Every uniform name some setter asked for that a program does not have. */
    missing() {
      const out = {};
      for (const [name, program] of Object.entries(programs)) if (program.missing.size) out[name] = [...program.missing];
      return out;
    },

    /** The table the display pass was last given, as #RRGGBB. */
    table() {
      const t = instance.lastTable;
      if (!t) return [];
      const h = (v) => v.toString(16).padStart(2, '0');
      return t.entries.slice(0, t.count).map((c) => `#${h(c.r)}${h(c.g)}${h(c.b)}`);
    },
  };

  return {
    gl,
    instance,
    programs,

    render({ input, params, time }) {
      // The host restates every parameter; Custom is text, the counts are
      // FF_TYPE_INTEGER (a dropdown here, its index from the range's low end).
      for (const q of PARAMS) {
        const value = params.get(q.id);
        if (q.kind === Kind.Text) instance.SetTextParameter(q.slot, value);
        else if (q.kind === Kind.Integer) instance.SetFloatParameter(q.slot, f32(InfoOf(q.slot).lo + value));
        else instance.SetFloatParameter(q.slot, f32(value));
      }
      instance.ProcessOpenGL(input, time);
    },
  };
}

//===========================================================================
// The parameters: the constructor's, in kTable's order and groups, with the
// plugin's names, element lists and defaults. Hints from the README and
// Controls.h.
//===========================================================================
const fix = (n, d = 2) => n.toFixed(d);
const V = (v) => f32(v);

/** id, ParamId, display, hint. */
const SPEC = [
  ['palette', PT.PALETTE, null, 'What the indices mean: a machine’s published table in its own colours, a palette you type (Custom), or one chosen from the clip (Adaptive).'],
  ['colours', PT.COLOURS, null, 'Adaptive: how many entries it chooses, 2 to 16. FF_TYPE_INTEGER in the plugin.'],
  ['adapt', PT.ADAPT, (v) => `${fix(AdaptFromParam(V(v)), 2)} s`, 'Adaptive: how fast the shown palette follows what the clip needs, a one-pole of 0 to 4 seconds, quadratically. 0 is at once.'],
  ['custom', PT.CUSTOM, null, 'Custom: up to sixteen colours, #RRGGBB, RRGGBB, 0xRRGGBB, $RGB or #RGB, separated by spaces, commas or semicolons. Fewer than two gives the default.'],
  ['order', PT.ORDER, null, 'What “index i” means to a swap and a cycle range: the i-th lightest entry (OKLab L), or the machine’s own register i.'],
  ['dac', PT.DAC, null, 'The precision the table is shown at: 24-bit, VGA’s 18, the Amiga’s 12, the Atari ST’s 9. Each gun goes to its nearest level.'],

  ['dither', PT.DITHER, null, 'None is nearest in OKLab. Bayer 2, 4 and 8 and Blue Noise are Knoll’s pattern dither for any palette; Dot Diffusion is Knuth’s parallel error diffusion. None of them depends on time.'],
  ['ditherAmount', PT.DITHER_AMOUNT, (v) => fix(V(v)), 'How much of the error the dither carries. 0 is nearest.'],
  ['pixelSize', PT.PIXEL_SIZE, null, 'The cell, 1 to 16 pixels square: the clip is averaged into cells and each cell gets one index. FF_TYPE_INTEGER in the plugin.'],

  ['swapTo', PT.SWAP_TO, null, 'The second table behind the same indices.'],
  ['swap', PT.SWAP, (v) => fix(V(v)), 'From the palette (0) to Swap To (1). The picture is not quantised again: its contours and its dither stay exactly where they are.'],
  ['swapStyle', PT.SWAP_STYLE, null, 'Cut changes the whole table at ½; Fade moves each index along its own straight line in OKLab; Ripple cuts one index at a time, in the working order.'],

  ['cycle', PT.CYCLE, null, 'Deluxe Paint’s colour cycling (CRNG): Forward moves each colour one register up a step, Reverse down, Ping-Pong (not CRNG’s) up the range and back.'],
  ['cycleRate', PT.CYCLE_RATE, (v) => `${fix(CycleRateFromParam(V(v)), 2)} /s`, 'Steps a second (or a beat, or a bar: Cycle Sync), 0 to 60, quadratically. 60 a second is CRNG’s whole range.'],
  ['cycleSync', PT.CYCLE_SYNC, null, 'Free counts seconds; Beat and Bar count the host’s beats or bars. No host sends one here, so they count the plugin’s own 120 BPM (4 beats a bar), as it does in a host that sends none.'],
  ['cycleStart', PT.CYCLE_START, null, 'The first register of the range, 0 to 15, in the working order. FF_TYPE_INTEGER in the plugin.'],
  ['cycleLength', PT.CYCLE_LENGTH, null, 'How many registers the range holds, 2 to 16, clipped to the palette. FF_TYPE_INTEGER in the plugin.'],
  ['cycleBlend', PT.CYCLE_BLEND, null, 'Step shows whole registers, as DPaint did; Smooth blends each register towards the next step in OKLab, through the DAC.'],

  ['freeze', PT.FREEZE, null, 'Keep the indices and let the table carry on: a held frame whose colours still cycle and swap. FF_TYPE_BOOLEAN in the plugin.'],
  ['alpha', PT.ALPHA, null, 'Keep shows each cell at the clip’s own coverage; Opaque fills it. Try the clip with transparency.'],
  ['mix', PT.MIX, (v) => fix(V(v)), 'The whole effect against the clip. At 0 the clip comes back as it went in.'],
];

/** An integer control's dropdown: its range, in order. */
const rangeOf = (info) => Array.from({ length: info.hi - info.lo + 1 }, (_, i) => String(info.lo + i));

const PARAMS = SPEC.map(([id, slot, display, hint]) => {
  const info = InfoOf(slot);
  if (info.id !== slot) throw new Error(`${id}: not a declared parameter`);
  const p = { id, slot, kind: info.kind, name: info.name, group: info.group, hint };
  if (info.kind === Kind.Option) Object.assign(p, { type: 'option', elements: info.options.slice(), default: info.defaultValue });
  else if (info.kind === Kind.Integer) Object.assign(p, { type: 'option', elements: rangeOf(info), default: info.defaultValue - info.lo });
  else if (info.kind === Kind.Boolean) Object.assign(p, { type: 'boolean', default: info.defaultValue });
  else if (info.kind === Kind.Text) Object.assign(p, { type: 'text', default: info.text, placeholder: '#RRGGBB $RGB …' });
  else Object.assign(p, { type: 'standard', display, default: info.defaultValue });
  return p;
});
if (PARAMS.length !== PT.ABOUT_FIRST || PARAMS.some((p, i) => p.slot !== i)) {
  throw new Error('the panel must carry every control, in the plugin’s order');
}

/** A count's dropdown index for the integer n. */
const countIndex = (slot, n) => n - InfoOf(slot).lo;

// Presets: whole states, on top of the constructor's defaults.
const PRESETS = {
  'PICO-8, Bayer 4 (the defaults)': {},
  'Swap half way to the C64 (Fade)': { swap: 0.5 },
  'Ripple to Workbench 1.3': { swapTo: PaletteId.Workbench13, swapStyle: SwapStyle.Ripple, swap: 0.5 },
  'Cycle the whole table': { cycle: CycleMode.Forward },
  'Cycle entries 4–11 at 15 a second (README)': {
    cycle: CycleMode.Forward, cycleRate: ParamFromCycleRate(15.0),
    cycleStart: countIndex(PT.CYCLE_START, 4), cycleLength: countIndex(PT.CYCLE_LENGTH, 8),
  },
  'Ping-Pong on the beat, smooth': { cycle: CycleMode.PingPong, cycleSync: CycleSync.Beat, cycleRate: ParamFromCycleRate(1.0), cycleBlend: CycleBlend.Smooth },
  'Commodore 64, Blue Noise': { palette: PaletteId.C64, dither: Dither.BlueNoise },
  'Game Boy, Dot Diffusion': { palette: PaletteId.GameBoy, dither: Dither.DotDiffusion },
  'Teletext, no dither, fat pixels': { palette: PaletteId.Teletext, dither: Dither.None, pixelSize: countIndex(PT.PIXEL_SIZE, 6) },
  'Workbench 2.0 at 12 bits, Bayer 8': { palette: PaletteId.Workbench20, dither: Dither.Bayer8, dac: Dac.Bits4 },
  'Adaptive, 8 colours': { palette: PaletteId.Adaptive, colours: countIndex(PT.COLOURS, 8) },
  'Custom (the plugin’s own eight), Bayer 8': { palette: PaletteId.Custom, dither: Dither.Bayer8 },
};

mountDemo({
  name: 'Ramdac',
  pluginId: 'RM01',
  tagline:
    'Indexed colour: the clip forced into the 4, 8 or 16 colours of a real machine, a palette you type, or one chosen from the clip, then swapped, faded and colour-cycled the way Deluxe Paint did it. The picture is an index buffer and a 16-entry colour table, so every swap and every cycle is a write to the table and the indices never move.',
  repo: 'https://github.com/stoatworks-labs/ramdac',
  page: 'https://stoatworks-labs.com/software/ramdac/',

  blurb:
    'It is Ramdac’s own GLSL, ported from the repository to WebGL2 unedited, fed by a JavaScript port of the plugin’s CPU half (every control’s mapping, the palettes’ arithmetic, the colour table’s swap and cycle, the adaptive palette and the dot-diffusion plan), running on generated clips or your own in this page — no install.',

  needFloat: true,
  showBackdrop: true,

  // The scene first: a moving picture with a full range, which is what the
  // palettes and the dithers are for. The ramps next, where a dither's
  // pattern and a cycle's bands read plainly; then the bars' flat colours,
  // fine detail for the cell grid, lights on black, and the one clip with
  // real alpha, for the Alpha control.
  sources: ['scene', 'ramp', 'bars', 'grid', 'detail', 'spot', 'alpha'],

  params: PARAMS,
  presets: PRESETS,

  differences: [
    'The CPU half is a PORT, not the plugin’s code: Controls.cpp (every name, group, default, range and mapping), the palettes’ arithmetic (sRGB, OKLab both ways, the DAC’s rounding, the Custom parser, Lightness order), the colour table (the swap partner, Cut, Fade and Ripple, the cycle’s clock, offsets and Smooth blend), the Adaptive palette (median cut, warm-started k-means, the one-pole of Adapt seconds) and Knuth’s dot-diffusion plan, all in double, and the CPU half of Ramdac.cpp that turns them into each pass’s uniforms. Only a reader has checked that code against the plugin’s. What is measured is the picture: demo/tools/crosscheck/ feeds the plugin’s own harness (rmtest) and this page the same 60 frames of the harness’s moving card and compares the last. On 2 November 2026, on the Apple M4 Max the plugin was measured on, 24 cases — the defaults, every dither including Dot Diffusion, Cut, Fade (one through the 12-bit DAC) and Ripple, Forward, Reverse and Ping-Pong cycling (Step and Smooth; Free, Beat and Bar), Adaptive three ways, Custom, Freeze, both Alphas and Mix — came out identical to the plugin’s in every pixel, at 640 × 360 and at 1280 × 720, except Mix at 0.5, where about 22% of pixels are one level apart (the page writes an 8-bit canvas, the harness rounds a float framebuffer). The page one frame short on a cycling case scored 55.3% within a level and fails, as do five deliberate slips in the port, among them an integer uniform sent as a float. A browser on another GPU has not been compared.',
    'The GPU half is not a port: the grid, analysis, quantise, diffuse, settle, gather, display and resample passes are the plugin’s own GLSL, joined as the plugin joins them, on its formats — the grid, the indices and the diffusion classes in RGBA16F and the analysis in RGBA32F (EXT_color_buffer_float, without which the page stops and says so), the analysis read back as floats every frame, as the plugin’s is. The blue-noise mask, Knuth’s class matrix and the eleven published palettes are the plugin’s data, spliced, not retyped. demo/tools/check_shaders.py fails the repository’s verify script if a character of the shaders, a value of those tables, or the way the shaders are joined drifts.',
    'The clock is the page’s: seconds from frame deltas, capped at a tenth of a second a frame by the page (the plugin caps a frame at a quarter). The plugin’s vote on whether its host sends seconds or milliseconds is not ported, because this host’s unit is known. The transport’s Restart jumps the clock back, which the plugin takes as no time passing, so a cycle carries on from where it was. Paused, the cycle and the Adaptive palette stop; Step is a sixtieth of a second.',
    'Cycle Sync has no host beat here: no tempo reaches a page, so Beat and Bar count beats of the plugin’s own 120 BPM in 4/4 on the page’s clock — a Cycle Rate of 1 is two steps a second on Beat and half a step a second on Bar — which is what the plugin does in a host that sends no beat. In Resolume they follow Resolume’s bar, wherever its transport is.',
    'Colours, Pixel Size, Cycle Start and Cycle Length are FF_TYPE_INTEGER in the plugin, which Resolume shows as number fields; here they are dropdowns of the same ranges. Freeze is FF_TYPE_BOOLEAN, a toggle in both. Custom is FF_TYPE_TEXT, a text field in both, parsed by the port. The About block is not on the panel.',
    'The indices live across frames, as the plugin’s do: Freeze holds them while the table goes on cycling and swapping, and a new Composition or Pixel Size under Freeze carries them into the new grid by the plugin’s integer law. Changing the Clip here feeds the same instance, so a frozen picture stays frozen, and the Adaptive palette follows the new clip at its Adapt rate.',
    'The clip is the kit’s generated one or your own file, 8-bit as Resolume hands an effect its input, taken as premultiplied (the plugin’s assumption about Resolume, which AGENTS.md says has not been measured). Alpha Keep shows the clip’s coverage over the Behind backdrop; in Resolume that would be the layers below.',
    'The Presets menu is the page’s: the plugin has none, since Resolume saves parameter presets itself. Each preset is a set of the plugin’s own controls on top of its defaults; the cycle of entries 4–11 is the README’s figure.',
    'The plugin’s negative-control hooks (the Hooks uniform and the test setters its harness flips to prove its checks can fail) are off here, as in a shipped instance.',
    'The crosscheck says the page draws what the plugin draws on this machine; it says nothing about whether either is right. The plugin’s harness, rmtest, shows every pixel a table entry, each undithered cell the OKLab-nearest entry (0 wrong of 918,827), a swap that keeps every index, a cycle that is a rotation of the table, and every dither still on a still clip, bit for bit; 11 wrong models and 35 one-character mutants are caught. The plugin has run in Resolume Arena on Windows, on software rendering, through the fleet’s Arena gate, but has never been loaded into Resolume on macOS. That harness, not this page, is the reason to believe the picture.',
  ],

  createRenderer,
});
