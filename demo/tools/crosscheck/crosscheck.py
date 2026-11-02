"""The browser demo against the plugin: the same frames, two renderers, one picture.

    python3 demo/tools/crosscheck/crosscheck.py [--rmtest build/rmtest] [--size 640x360] [--keep DIR]

Each case below is rendered twice, over the same 60 frames a sixtieth of a
second apart:

  - by the PLUGIN: `rmtest --pipe`, the real plugin class in a GL 4.1 context
    (build it first: CLAUDE.md's `cmake -B build`, or tools/verify.sh's
    build-verify/), with no beat sent (--bpm 0), so Cycle Sync's beat runs on
    the plugin's own 120 BPM, as the page's does;
  - by the PAGE: demo/ served from here and loaded in headless Chrome, through
    window.ramdacDemo.renderCase, which makes a fresh instance and runs the
    same frames on the same clock.

The frames are the harness's own moving card, taken from the plugin at Mix 0
(which hands its clip back to the bit), and fed to both. The `alpha` cases get
the same card with coverage cut into it here (a band at 128/255, premultiplied,
and a band at 0), because Alpha does nothing on an opaque clip. Both pictures
are the last frame, RGBA8, and they are compared pixel by pixel.

The page's GPU half is the plugin's GLSL (check_shaders.py), so what this
mostly tests is the PORT: every mapping, the palettes' arithmetic, the DAC,
the table's swap and cycle, the Adaptive palette, the dot-diffusion plan and
every uniform's type. A difference is a port bug, or a difference between GL
4.1 on this machine and WebGL2 through ANGLE on the same GPU: a cell whose two
nearest entries are a rounding apart can go either way, and is then a whole
palette entry off. So the figures are the share of pixels exactly equal and
within one level, and the worst. A negative control -- a cycling case one
frame short -- shows the comparison can see a sixtieth of a second.

Not run by tools/verify.sh: it needs a GPU and Chrome. Exit 1 if a case falls
under its threshold, the page asked for a uniform no program has, or the
negative control is not caught.
"""
import argparse
import functools
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.abspath(os.path.join(HERE, "..", ".."))
REPO = os.path.abspath(os.path.join(DEMO, ".."))
FRAMES, FPS = 60, 60
sys.dont_write_bytecode = True

# name, clip, settings as `rmtest --set` takes them (an option by its element's
# name, a count by its number, Custom by its text).
CASES = [
    ("defaults", "card", {}),
    ("dither-none", "card", {"Dither": "None"}),
    ("bayer-2-cga16", "card", {"Dither": "Bayer 2", "Palette": "CGA 16"}),
    ("bayer-8-amount", "card", {"Dither": "Bayer 8", "Dither Amount": "0.6", "Pixel Size": "1"}),
    ("blue-noise-c64", "card", {"Dither": "Blue Noise", "Palette": "Commodore 64"}),
    ("dot-diffusion", "card", {"Dither": "Dot Diffusion"}),
    ("dot-diffusion-gameboy-p3", "card", {"Dither": "Dot Diffusion", "Palette": "Game Boy", "Pixel Size": "3"}),
    ("fade-mid", "card", {"Swap": "0.5"}),
    ("fade-dac12", "card", {"Swap": "0.37", "DAC": "12-bit", "Swap To": "Amstrad CPC"}),
    ("cut-cga16", "card", {"Swap Style": "Cut", "Swap": "0.7", "Swap To": "CGA 16"}),
    ("ripple-native", "card", {"Swap Style": "Ripple", "Swap": "0.45", "Swap To": "ZX Spectrum", "Order": "Native"}),
    ("cycle-forward", "card", {"Cycle": "Forward"}),
    ("cycle-range-smooth", "card", {"Cycle": "Forward", "Cycle Rate": "0.5", "Cycle Start": "4", "Cycle Length": "8",
                                    "Cycle Blend": "Smooth"}),
    ("cycle-pingpong-beat", "card", {"Cycle": "Ping-Pong", "Cycle Sync": "Beat", "Cycle Rate": "0.31", "Cycle Blend": "Smooth",
                                     "DAC": "18-bit"}),
    ("cycle-reverse-bar", "card", {"Cycle": "Reverse", "Cycle Sync": "Bar", "Cycle Rate": "0.8", "Palette": "NES"}),
    ("adaptive", "card", {"Palette": "Adaptive"}),
    ("adaptive-8-instant", "card", {"Palette": "Adaptive", "Colours": "8", "Adapt": "0", "Dither": "Blue Noise"}),
    ("adaptive-swap-to", "card", {"Swap To": "Adaptive", "Swap": "0.6", "Colours": "5", "Dither": "None"}),
    ("custom", "card", {"Palette": "Custom", "Custom": "#102030, $F80; 0x00ff00 abc nonsense #FFFFFF", "Dither": "Bayer 8"}),
    ("workbench-pixel-7", "card", {"Palette": "Workbench 1.3", "Pixel Size": "7", "DAC": "9-bit"}),
    ("freeze-cycling", "card", {"Freeze": "1", "Cycle": "Forward", "Cycle Rate": "0.4"}),
    ("mix-half", "card", {"Mix": "0.5", "Palette": "Teletext"}),
    ("alpha-keep", "alpha", {}),
    ("alpha-opaque-diffusion", "alpha", {"Alpha": "Opaque", "Dither": "Dot Diffusion"}),
]
# The negative control: a cycling case's page render one frame short.
NEGATIVE = ("cycle-range-smooth", "late")


def parse_size(text):
    w, h = text.lower().split("x")
    return int(w), int(h)


def run_rmtest(rmtest, args, stdin=None):
    done = subprocess.run([rmtest, *args], input=stdin, capture_output=True, check=False)
    if done.returncode != 0:
        sys.exit(f"rmtest {' '.join(args)} failed: {done.stderr.decode(errors='replace').strip()}")
    return done.stdout


def with_alpha(cards, width, height):
    """The card with a band at coverage 128/255 (premultiplied) and a band at 0."""
    frames = np.frombuffer(cards, np.uint8).reshape(-1, height, width, 4).copy()
    half = slice(width // 3, width // 2)
    none = slice(5 * width // 6, width)
    rgb = frames[:, :, half, :3].astype(np.uint32)
    frames[:, :, half, :3] = ((rgb * 128 + 127) // 255).astype(np.uint8)
    frames[:, :, half, 3] = 128
    frames[:, :, none, :] = 0
    return frames.tobytes()


class Handler(http.server.SimpleHTTPRequestHandler):
    """demo/ as it is, plus /xc/ for the job and the page's answers."""

    def __init__(self, *args, state, **kwargs):
        self.state = state
        super().__init__(*args, directory=DEMO, **kwargs)

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/xc/job.json":
            return self.reply(json.dumps(self.state["job"]).encode(), "application/json")
        if self.path.startswith("/xc/clip/"):
            return self.reply(self.state["clips"][self.path[len("/xc/clip/"):]], "application/octet-stream")
        return super().do_GET()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path.startswith("/xc/out/"):
            self.state["page"][self.path[len("/xc/out/"):]] = body
        elif self.path == "/xc/missing":
            self.state["missing"] = json.loads(body)
        elif self.path == "/xc/error":
            self.state["error"] = body.decode(errors="replace")
            self.state["finished"].set()
        elif self.path == "/xc/done":
            self.state["finished"].set()
        self.reply(b"ok", "text/plain")

    def reply(self, body, kind):
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def compare(a, b, width, height):
    x = np.frombuffer(a, np.uint8).reshape(height, width, 4).astype(np.int16)
    y = np.frombuffer(b, np.uint8).reshape(height, width, 4).astype(np.int16)
    worst = np.abs(x - y).max(axis=2)
    return {
        "exact": float((worst == 0).mean() * 100.0),
        "within1": float((worst <= 1).mean() * 100.0),
        "over1": int((worst > 1).sum()),
        "max": int(worst.max()),
        "mean": float(np.abs(x - y)[..., :3].mean()),
    }


def default_rmtest():
    for build in ("build-verify", "build"):
        path = os.path.join(REPO, build, "rmtest")
        if os.access(path, os.X_OK):
            return path
    return os.path.join(REPO, "build", "rmtest")


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--rmtest", default=default_rmtest())
    parser.add_argument("--size", default="640x360")
    parser.add_argument("--keep", help="write each case's two pictures here as PNGs")
    parser.add_argument("--threshold", type=float, default=99.9,
                        help="share of pixels, %%, that must be within one level of 255 (default 99.9)")
    parser.add_argument("--chrome", default=os.environ.get(
        "CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"))
    parser.add_argument("--port", type=int, default=0, help="the local server's port (default: any free one)")
    parser.add_argument("--only", help="run only the cases whose name contains this")
    args = parser.parse_args(argv)
    width, height = parse_size(args.size)
    if not os.access(args.rmtest, os.X_OK):
        sys.exit(f"no rmtest at {args.rmtest}: build it (cmake -B build && cmake --build build --target rmtest)")
    cases = [c for c in CASES if not args.only or args.only in c[0]]

    size = f"{width}x{height}"
    common = ["--size", size, "--fps", str(FPS), "--bpm", "0"]
    cards = run_rmtest(args.rmtest, ["--film", str(FRAMES), *common, "--set", "Mix=0"])
    frame_bytes = width * height * 4
    if len(cards) != FRAMES * frame_bytes:
        sys.exit(f"rmtest --film gave {len(cards)} bytes, not {FRAMES} frames")
    clips = {"card": cards, "alpha": with_alpha(cards, width, height)}

    plugin = {}
    for name, clip, settings in cases:
        sets = [arg for key, value in settings.items() for arg in ("--set", f"{key}={value}")]
        out = run_rmtest(args.rmtest, ["--pipe", *common, *sets], stdin=clips[clip])
        if len(out) != FRAMES * frame_bytes:
            sys.exit(f"rmtest --pipe for {name} gave {len(out)} bytes, not {FRAMES} frames")
        plugin[name] = out[-frame_bytes:]

    job_cases = [{"name": name, "clip": clip, "settings": settings, "count": FRAMES} for name, clip, settings in cases]
    negative = next((c for c in cases if c[0] == NEGATIVE[0]), None)
    if negative:
        job_cases.append({"name": NEGATIVE[1], "clip": negative[1], "settings": negative[2], "count": FRAMES - 1})
    state = {"job": {"width": width, "height": height, "fps": FPS, "cases": job_cases},
             "clips": clips, "page": {}, "missing": None, "error": None, "finished": threading.Event()}

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), functools.partial(Handler, state=state))
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = tempfile.mkdtemp(prefix="ramdac-chrome-")
    chrome = subprocess.Popen([
        args.chrome, "--headless=new", "--no-first-run", "--no-default-browser-check",
        f"--user-data-dir={profile}", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist",
        "--window-size=1600,1200", f"http://127.0.0.1:{port}/tools/crosscheck/",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not state["finished"].wait(300):
            sys.exit("the page never finished (300 s)")
    finally:
        chrome.terminate()
        try:
            chrome.wait(10)
        except subprocess.TimeoutExpired:
            chrome.kill()
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)
    if state["error"]:
        sys.exit(f"the page failed:\n{state['error']}")

    if args.keep:
        from PIL import Image
        os.makedirs(args.keep, exist_ok=True)

    failures = 0
    print(f"{size}, {FRAMES} frames at {FPS} fps, the last compared\n")
    print(f"{'case':<26} {'exact':>9} {'<=1':>9} {'>1 px':>7} {'max':>4} {'mean':>7}")
    for name, _, _ in cases:
        got = state["page"].get(name)
        if got is None or len(got) != frame_bytes:
            print(f"{name:<26} no picture from the page")
            failures += 1
            continue
        s = compare(plugin[name], got, width, height)
        ok = s["within1"] >= args.threshold
        failures += 0 if ok else 1
        print(f"{name:<26} {s['exact']:8.3f}% {s['within1']:8.3f}% {s['over1']:>7} {s['max']:>4} {s['mean']:7.4f}"
              f"  {'ok' if ok else 'FAIL'}")
        if args.keep:
            for label, data in (("plugin", plugin[name]), ("page", got)):
                Image.frombytes("RGBA", (width, height), data).save(os.path.join(args.keep, f"{name}-{label}.png"))

    # The analysis program has kPaletteCommon but is given no palette, and the
    # diffuse pass never reads LightOrder: names the plugin's own FindUniform
    # also gets -1 for. Anything else asked for and absent is a dead uniform.
    expected = {"diffuse": ["LightOrder"]}
    missing = state["missing"] or {}
    unexpected = {k: sorted(set(v) - set(expected.get(k, []))) for k, v in missing.items()}
    unexpected = {k: v for k, v in unexpected.items() if v}
    print(f"\nuniforms the page set that a program does not have: {json.dumps(missing)}"
          + ("" if not unexpected else f"  -- UNEXPECTED: {json.dumps(unexpected)}"))
    failures += 1 if unexpected else 0

    if negative:
        late = state["page"].get(NEGATIVE[1])
        s = compare(plugin[NEGATIVE[0]], late, width, height) if late else None
        caught = s is not None and s["within1"] < args.threshold
        failures += 0 if caught else 1
        print(f"negative control, {NEGATIVE[0]} one frame short: "
              + (f"{s['within1']:.3f}% within one level, {'caught' if caught else 'NOT CAUGHT'}" if s else "no picture"))
    print(f"\n{'all cases within threshold' if not failures else f'{failures} problem(s)'} ({args.threshold}% within one level)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
