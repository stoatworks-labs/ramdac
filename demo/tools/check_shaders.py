"""The demo's shaders and tables must be the plugin's, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js and demo/tables.js

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

`demo/shaders.js` holds the GLSL pieces of `source/Shaders.cpp` (kQuadVertex,
the kCommon and kPaletteCommon libraries, and the grid, analysis, quantise,
diffuse, settle, gather, display and resample fragments) plus kVersion. That
is two copies of the same text, and two copies drift -- quietly, because a
dither drawn by a *plausible* shader looks exactly like one drawn by the right
one. The whole claim of the page is that it runs the plugin's own shaders, so
the claim needs something enforcing it. `rmtest` drives the real plugin class
and has never heard of this page, and `tools/glslc.sh` compiles the C++ copies
and never looks at the JS one.

`demo/tables.js` holds the plugin's DATA that the page's port runs on: the
4096 ranks of the blue-noise mask (source/BlueNoise.h), Knuth's class matrix
(source/Dither.cpp), the eleven published palettes and the Custom default
(source/Palettes.cpp), and the analysis grid's size (source/Shaders.h). They
are numbers, not text, so they are compared as numbers: every value, in order,
and nothing more or less.

------------------------------------------------------------------- what it does

Pulls each piece out of `source/Shaders.cpp` and the matching backtick literal
out of `demo/shaders.js`, and compares them exactly -- no whitespace
normalisation, no comment stripping: a comment updated on one side only is
exactly the drift worth catching. `kVersion`, a plain string, is compared too.

A piece may be several adjacent raw strings (MSVC refuses one literal over
about 16 KB, C2026); this joins them as the compiler does, with nothing
between. None of ramdac's pieces needs that today.

It also fails when Shaders.cpp grows a piece this script does not list (a new
pass the page would silently lack), or Palettes.cpp a table, and it checks the
JOIN as well as the pieces: `Assemble()` in Shaders.cpp is kVersion + kCommon,
then kPaletteCommon for the analysis, quantise and diffuse passes only, then
the pass's fragment; InitGL in Ramdac.cpp builds the vertex stage as kVersion +
kQuadVertex and each of its eight fragment stages with Assemble(). plugin.js
must assemble them the same way. A change to either side's assembly fails here
and needs a reader.

The one transformation on the GLSL is a decode, not a normalisation: a
backtick cannot appear raw inside a template literal, so --write would escape
one as \\`. Ramdac's GLSL has none. This undoes that escape and REJECTS any
other backslash or any `${` on the JS side; there are none in the C++ bodies,
so either could only be somebody hiding a difference.

`--write` produces both files from the C++, so the copies are spliced by this
script and never typed.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORTED half: Controls.cpp (the names, defaults and
mappings), the functions of Palettes.cpp (OKLab, the DAC, the Custom parser,
Lightness order), Clut.cpp (swap, fade, ripple, cycle), Adaptive.cpp (median
cut, k-means, the one-pole), Dither.cpp's plan, and the frame logic of
Ramdac.cpp's ProcessOpenGL in demo/plugin.js are a hand translation.
demo/tools/crosscheck/ renders rmtest and the page over the same frames and
compares the pictures; it needs Chrome and a GPU, so it is run by hand, not by
verify.sh. When you change one of those, change the port too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

# JS constant, C++ symbol (all in source/Shaders.cpp), in the order they are
# declared there.
SHADERS = [
    ("QUAD_VERTEX", "kQuadVertex"),
    ("COMMON", "kCommon"),
    ("PALETTE_COMMON", "kPaletteCommon"),
    ("GRID_FRAGMENT", "kGridFragment"),
    ("ANALYSIS_FRAGMENT", "kAnalysisFragment"),
    ("QUANTISE_FRAGMENT", "kQuantiseFragment"),
    ("DIFFUSE_FRAGMENT", "kDiffuseFragment"),
    ("SETTLE_FRAGMENT", "kSettleFragment"),
    ("GATHER_FRAGMENT", "kGatherFragment"),
    ("DISPLAY_FRAGMENT", "kDisplayFragment"),
    ("RESAMPLE_FRAGMENT", "kResampleFragment"),
]

# The published palettes in source/Palettes.cpp, in the order they are
# declared there. tables.js keeps the C++ names.
PALETTES = ["kTeletext", "kCgaMode4", "kCga16", "kC64", "kSpectrum", "kCpc", "kNes", "kGameBoy", "kPico8",
            "kWorkbench13", "kWorkbench20"]

# How the pieces are joined, on each side. Each entry is (file, text that must
# appear in it verbatim). The C++ lines are the plugin's assembly; the JS lines
# are plugin.js's copy of it. A change to either is a change to what runs.
ASSEMBLY = [
    ("source/Shaders.cpp",
     "std::string Assemble( Pass pass )\n{\n"
     "\tstd::string out = kVersion;\n"
     "\tout += kCommon;\n"
     "\tswitch( pass )\n\t{\n"
     "\tcase Pass::Grid: out += kGridFragment; break;\n"
     "\tcase Pass::Analysis:\n\t\tout += kPaletteCommon;\n\t\tout += kAnalysisFragment;\n\t\tbreak;\n"
     "\tcase Pass::Quantise:\n\t\tout += kPaletteCommon;\n\t\tout += kQuantiseFragment;\n\t\tbreak;\n"
     "\tcase Pass::Diffuse:\n\t\tout += kPaletteCommon;\n\t\tout += kDiffuseFragment;\n\t\tbreak;\n"
     "\tcase Pass::Settle: out += kSettleFragment; break;\n"
     "\tcase Pass::Gather: out += kGatherFragment; break;\n"
     "\tcase Pass::Display: out += kDisplayFragment; break;\n"
     "\tcase Pass::Resample: out += kResampleFragment; break;\n"
     "\t}\n\treturn out;\n}"),
    ("source/Ramdac.cpp", "const std::string vertex = std::string( shaders::kVersion ) + shaders::kQuadVertex;"),
    ("source/Ramdac.cpp", '{ &gridShader, shaders::Pass::Grid, "grid" },'),
    ("source/Ramdac.cpp", '{ &analysisShader, shaders::Pass::Analysis, "analysis" },'),
    ("source/Ramdac.cpp", '{ &quantiseShader, shaders::Pass::Quantise, "quantise" },'),
    ("source/Ramdac.cpp", '{ &diffuseShader, shaders::Pass::Diffuse, "diffuse" },'),
    ("source/Ramdac.cpp", '{ &settleShader, shaders::Pass::Settle, "settle" },'),
    ("source/Ramdac.cpp", '{ &gatherShader, shaders::Pass::Gather, "gather" },'),
    ("source/Ramdac.cpp", '{ &displayShader, shaders::Pass::Display, "display" },'),
    ("source/Ramdac.cpp", '{ &resampleShader, shaders::Pass::Resample, "resample" },'),
    ("source/Ramdac.cpp", "const std::string fragment = shaders::Assemble( stage.pass );"),
    ("source/Ramdac.cpp", "if( stage.shader->Compile( vertex.c_str(), fragment.c_str() ) )"),
    ("demo/plugin.js", "const vertex = S.VERSION + S.QUAD_VERTEX;"),
    ("demo/plugin.js", "const assemble = (...pieces) => S.VERSION + S.COMMON + pieces.join('');"),
    ("demo/plugin.js", "grid: new Program(gl, vertex, assemble(S.GRID_FRAGMENT), 'grid'),"),
    ("demo/plugin.js", "analysis: new Program(gl, vertex, assemble(S.PALETTE_COMMON, S.ANALYSIS_FRAGMENT), 'analysis'),"),
    ("demo/plugin.js", "quantise: new Program(gl, vertex, assemble(S.PALETTE_COMMON, S.QUANTISE_FRAGMENT), 'quantise'),"),
    ("demo/plugin.js", "diffuse: new Program(gl, vertex, assemble(S.PALETTE_COMMON, S.DIFFUSE_FRAGMENT), 'diffuse'),"),
    ("demo/plugin.js", "settle: new Program(gl, vertex, assemble(S.SETTLE_FRAGMENT), 'settle'),"),
    ("demo/plugin.js", "gather: new Program(gl, vertex, assemble(S.GATHER_FRAGMENT), 'gather'),"),
    ("demo/plugin.js", "display: new Program(gl, vertex, assemble(S.DISPLAY_FRAGMENT), 'display'),"),
    ("demo/plugin.js", "resample: new Program(gl, vertex, assemble(S.RESAMPLE_FRAGMENT), 'resample'),"),
]

SHADERS_HEADER = """// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \\` for a backtick inside a comment.
"""

TABLES_HEADER = """// GENERATED from source/BlueNoise.h, source/Dither.cpp, source/Palettes.cpp
// and source/Shaders.h by demo/tools/check_shaders.py --write. Do not edit:
// tools/verify.sh fails if a value of this differs from the plugin's.
"""


def read(*parts):
    with open(os.path.join(REPO, *parts)) as handle:
        return handle.read()


def cpp_version(source):
    match = re.search(r'const char\* const kVersion = "(.*?)";', source)
    return None if match is None else match.group(1)


def from_cpp(source, symbol):
    """The piece's text: its raw strings, joined as the compiler joins them.

    Returns (text, count of raw strings), or (None, 0) if the symbol is not
    there or what follows its `=` is not a run of adjacent raw strings ending
    in `;`.
    """
    match = re.search(r'const char\* const ' + symbol + r' = ((?:R"\(.*?\)"\s*)+);', source, re.S)
    if match is None:
        return None, 0
    parts = re.findall(r'R"\((.*?)\)"', match.group(1), re.S)
    rebuilt = re.sub(r'R"\(.*?\)"', "", match.group(1), flags=re.S)
    if rebuilt.strip() != "":
        return None, 0
    return "".join(parts), len(parts)


def cpp_pieces(source):
    """Every raw-string piece Shaders.cpp defines, by symbol."""
    return re.findall(r'const char\* const (k\w+) = R"\(', source)


def from_js(source, name):
    match = re.search(r'^export const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    if "${" in body:
        return None, "template substitution"
    return body.replace("\\`", "`"), None


# ---------------------------------------------------------------- the tables

def integers(text):
    """Every integer literal in a brace or bracket body, decimal or 0x hex, in order."""
    if re.search(r"[^0-9A-Fa-fx,\s{}\[\]]", text):
        return None
    return [int(token, 0) for token in re.findall(r"0[xX][0-9A-Fa-f]+|\d+", text)]


def cpp_tables():
    """Every table the page needs, from the C++, as {js name: value}, or a complaint."""
    noise = read("source", "BlueNoise.h")
    dither = read("source", "Dither.cpp")
    palettes = read("source", "Palettes.cpp")
    shaders_h = read("source", "Shaders.h")
    controls = read("source", "Controls.cpp")
    out = {}

    def scalar(source, pattern, name, where):
        match = re.search(pattern, source)
        if match is None:
            raise LookupError(f"{name} not found in {where}")
        out[name] = int(match.group(1))

    def array(source, pattern, name, where, length=None):
        match = re.search(pattern, source, re.S)
        if match is None:
            raise LookupError(f"{name} not found in {where}")
        values = integers(match.group(1))
        if values is None:
            raise LookupError(f"{name} in {where} holds something that is not an integer")
        if length is not None and len(values) != length:
            raise LookupError(f"{name} in {where} has {len(values)} values, not {length}")
        out[name] = values

    scalar(noise, r"constexpr int kBlueNoiseSize = (\d+);", "kBlueNoiseSize", "source/BlueNoise.h")
    array(noise, r"constexpr uint16_t kBlueNoise\[ 4096 \] = \{(.*?)\};", "kBlueNoise", "source/BlueNoise.h", 4096)
    if sorted(out["kBlueNoise"]) != list(range(4096)):
        raise LookupError("kBlueNoise in source/BlueNoise.h is not every rank 0..4095 once")
    array(dither, r"const int kKnuthClass\[ 8 \]\[ 8 \] = \{(.*?)\};", "kKnuthClass", "source/Dither.cpp", 64)
    if sorted(out["kKnuthClass"]) != list(range(64)):
        raise LookupError("kKnuthClass in source/Dither.cpp is not every class 0..63 once")
    scalar(shaders_h, r"constexpr int kAnalysisWidth\s*= (\d+);", "kAnalysisWidth", "source/Shaders.h")
    scalar(shaders_h, r"constexpr int kAnalysisHeight\s*= (\d+);", "kAnalysisHeight", "source/Shaders.h")

    declared = re.findall(r"const uint32_t (k\w+)\[\] = \{", palettes)
    if declared != PALETTES:
        raise LookupError(f"source/Palettes.cpp declares the tables {declared}, not the {PALETTES} this check lists -- "
                          "add a new one here (and to the page)")
    for symbol in PALETTES:
        array(palettes, r"const uint32_t " + symbol + r"\[\] = \{(.*?)\};", symbol, "source/Palettes.cpp")

    custom = re.search(r'const char\* const kDefaultCustom = "([^"\\]*)";', palettes)
    if custom is None:
        raise LookupError("kDefaultCustom not found in source/Palettes.cpp")
    if f'text( PT_CUSTOM, "Custom", "Palette", "{custom.group(1)}" )' not in controls:
        raise LookupError("Controls.cpp's Custom default is not Palettes.cpp's kDefaultCustom")
    out["kDefaultCustom"] = custom.group(1)
    return out


def js_tables(source):
    out = {}
    for name, body in re.findall(r"^export const (k\w+) = \[(.*?)\];$", source, re.S | re.M):
        out[name] = integers(body)
    for name, value in re.findall(r"^export const (k\w+) = (\d+);$", source, re.M):
        out[name] = int(value)
    for name, value in re.findall(r"^export const (k\w+) = '([^'\\]*)';$", source, re.M):
        out[name] = value
    return out


def write_tables(tables):
    out = [TABLES_HEADER]
    out.append("\n// source/Shaders.h: the adaptive palette's analysis grid, at most.\n")
    out.append(f"export const kAnalysisWidth = {tables['kAnalysisWidth']};\n")
    out.append(f"export const kAnalysisHeight = {tables['kAnalysisHeight']};\n")
    out.append("\n// source/BlueNoise.h: a 64x64 void-and-cluster mask, every rank 0..4095 once, row 0 first.\n")
    out.append(f"export const kBlueNoiseSize = {tables['kBlueNoiseSize']};\n")
    noise = tables["kBlueNoise"]
    rows = ",\n".join("  " + ", ".join(str(v) for v in noise[r * 64:(r + 1) * 64]) for r in range(64))
    out.append(f"export const kBlueNoise = [\n{rows},\n];\n")
    out.append("\n// source/Dither.cpp: Knuth's class matrix, [y][x] flattened as y * 8 + x, row 0 the bottom.\n")
    knuth = tables["kKnuthClass"]
    rows = ",\n".join("  " + ", ".join(str(v) for v in knuth[r * 8:(r + 1) * 8]) for r in range(8))
    out.append(f"export const kKnuthClass = [\n{rows},\n];\n")
    out.append("\n// source/Palettes.cpp: the published tables, in each machine's own order.\n")
    for symbol in PALETTES:
        values = tables[symbol]
        digits = 3 if symbol.startswith("kWorkbench") else 6
        body = ", ".join(f"0x{v:0{digits}X}" for v in values)
        out.append(f"export const {symbol} = [{body}];\n")
    out.append("\n// source/Palettes.cpp: the Custom control's default, Controls.cpp's too.\n")
    out.append(f"export const kDefaultCustom = '{tables['kDefaultCustom']}';\n")
    with open(os.path.join(REPO, "demo", "tables.js"), "w") as handle:
        handle.write("".join(out))
    print("wrote demo/tables.js")


def write(cpp):
    out = [SHADERS_HEADER]
    version = cpp_version(cpp)
    if version is None:
        print("FAIL  kVersion not found in source/Shaders.cpp")
        return 1
    # kVersion is "#version 410 core\n" in the C++, the \n an escape there.
    out.append(f"export const VERSION = '{version}';\n")
    for name, symbol in SHADERS:
        body, count = from_cpp(cpp, symbol)
        if body is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp (or not a run of adjacent raw strings)")
            return 1
        if "\\" in body or "${" in body:
            print(f"FAIL  {symbol} holds a backslash or ${{; the escape scheme cannot carry it")
            return 1
        joined = f", {count} adjacent raw strings joined" if count > 1 else ""
        out.append(f"\n// {symbol}, source/Shaders.cpp{joined}\nexport const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    try:
        tables = cpp_tables()
    except LookupError as error:
        print(f"FAIL  {error}")
        return 1
    with open(os.path.join(REPO, "demo", "shaders.js"), "w") as handle:
        handle.write("".join(out))
    print("wrote demo/shaders.js")
    write_tables(tables)
    return 0


def first_difference(a, b):
    left, right = a.splitlines(), b.splitlines()
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            return i + 1, x, y
    return None


def check(cpp, js, tables_js):
    problems = 0
    version_cpp = cpp_version(cpp)
    version_js = re.search(r"^export const VERSION = '(.*?)';$", js, re.M)
    if version_cpp is None or version_js is None or version_cpp != version_js.group(1):
        print("FAIL  VERSION does not match kVersion")
        problems += 1
    else:
        print(f"ok    {'VERSION':<20} matches kVersion")

    listed = {symbol for _, symbol in SHADERS}
    for symbol in cpp_pieces(cpp):
        if symbol not in listed:
            print(f"FAIL  source/Shaders.cpp defines {symbol}, which this check does not list -- "
                  "add it to SHADERS (and to the page, or say on the page why not)")
            problems += 1

    for name, symbol in SHADERS:
        cpp_text, count = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp (or not a run of adjacent raw strings)")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif cpp_text == js_text:
            joined = f", {count} raw strings joined" if count > 1 else ""
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars{joined})")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(cpp_text, js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    try:
        want = cpp_tables()
    except LookupError as error:
        print(f"FAIL  {error}")
        want = None
        problems += 1
    if want is not None:
        if tables_js is None:
            print("FAIL  demo/tables.js is missing -- run demo/tools/check_shaders.py --write")
            problems += 1
        else:
            got = js_tables(tables_js)
            for name, value in want.items():
                if name not in got:
                    print(f"FAIL  {name} not found in demo/tables.js")
                    problems += 1
                elif got[name] != value:
                    print(f"FAIL  {name} in demo/tables.js has drifted from the C++")
                    problems += 1
                else:
                    size = f"{len(value)} values" if isinstance(value, list) else repr(value)
                    print(f"ok    {name:<20} matches the C++ ({size})")
            for name in got:
                if name not in want:
                    print(f"FAIL  demo/tables.js has {name}, which the C++ does not")
                    problems += 1

    for path, text in ASSEMBLY:
        try:
            body = read(*path.split("/"))
        except FileNotFoundError:
            print(f"FAIL  {path} is missing")
            problems += 1
            continue
        if text in body:
            print(f"ok    assembly: {path} still says {text.splitlines()[0][:60]}")
        else:
            print(f"FAIL  assembly: {path} no longer says: {text.splitlines()[0]}")
            problems += 1
    return problems


def main(argv):
    cpp = read("source", "Shaders.cpp")
    if "--write" in argv:
        return write(cpp)
    try:
        js = read("demo", "shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    try:
        tables_js = read("demo", "tables.js")
    except FileNotFoundError:
        tables_js = None
    problems = check(cpp, js, tables_js)
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write for a drifted copy "
              "(never edit shaders.js or tables.js by hand); an assembly or unlisted-piece failure needs a reader")
        return 1
    print(f"all {len(SHADERS) + 1} shader pieces, their assembly and the {len(PALETTES) + 6} tables are the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
