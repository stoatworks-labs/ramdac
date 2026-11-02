#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders, or built its references from the plugin's own functions, would
# pass every mutant here.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK is
# symlinked, not copied), built arm64-only, with only the named check run at
# CI's raster, under a time limit (a mutant can hang: honeydew's lesson; a
# timeout counts as caught).
#
#     tools/mutate.sh            every mutant
#     tools/mutate.sh PATTERN    only the mutants whose description matches
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
# The fields are split on '|', so neither a target nor a mutant may contain one
# (conway's trap).
MUTANTS=(
	"source/Shaders.cpp|pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) )|pow( ( c + 0.055 ) / 1.055, vec3( 2.5 ) )|--nearest|GLSL: the clip decoded at gamma 2.5, 4 -> 5"
	"source/Shaders.cpp|dot( vec3( 0.4122214708, 0.5363325363, 0.0514459929 ), lin )|dot( vec3( 0.4922214708, 0.5363325363, 0.0514459929 ), lin )|--nearest|GLSL: OKLab's L cone a fifth too red, 1 -> 9"
	"source/Shaders.cpp|vec3( 1.0 / 3.0 ) );|vec3( 1.0 / 2.0 ) );|--nearest|GLSL: a square root for OKLab's cube root, 3 -> 2"
	"source/Shaders.cpp|dot( vec3( 1.9779984951, -2.4285922050, 0.4505937099 ), lms )|dot( vec3( 1.9779984951, -2.4885922050, 0.4505937099 ), lms )|--nearest|GLSL: OKLab's a axis off green, 2 -> 8"
	"source/Shaders.cpp|vec3 d   = lab - PalLab[ i ];|vec3 d   = lab + PalLab[ i ];|--nearest|GLSL: the distance to an entry a sum, - -> +"
	"source/Shaders.cpp|( 2 * k - 1 - 2 * bit )|( 2 * k - 2 - 2 * bit )|--tone|GLSL: the Bayer rank's xor bit on the y bit, 1 -> 2"
	"source/Shaders.cpp|texelFetch( Mask, p & 63, 0 ).r ) >> 6;|texelFetch( Mask, p & 63, 0 ).r ) >> 5;|--tone|GLSL: blue noise's rank to 128 of 64, 6 -> 5"
	"source/Shaders.cpp|n    = 1 << ( 2 * DitherIndex );|n    = 1 << ( 3 * DitherIndex );|--tone|GLSL: Bayer's candidates 2^3k, 2 -> 3"
	"source/Shaders.cpp|error += t - PalLin[ e ];|error += t + PalLin[ e ];|--tone|GLSL: Knoll's error the sum of the cell and its candidate, - -> +"
	"source/Shaders.cpp|e = nearestEntry( clamp( t + error * Amount, 0.0, 1.0 ) );|e = nearestEntry( clamp( t - error * Amount, 0.0, 1.0 ) );|--tone|GLSL: Knoll's attempt against its error, + -> -"
	"source/Shaders.cpp|if( rank < seen )|if( rank > seen )|--tone|GLSL: the candidate at the rank from the wrong side, < -> >"
	"source/Shaders.cpp|DonorTexel[ i ] + tile, 0 ).rgb;|DonorTexel[ i ] - tile, 0 ).rgb;|--diffusion|GLSL: a donor's error from the mirrored tile, + -> -"
	"source/Shaders.cpp|fragColor    = vec4( attempt - PalLin[ e ], float( e ) );|fragColor    = vec4( attempt + PalLin[ e ], float( e ) );|--diffusion|GLSL: the error kept a sum, - -> +"
	"source/Shaders.cpp|ClassOf[ ( cell.y & 7 ) * 8 + ( cell.x & 7 ) ];|ClassOf[ ( cell.y & 7 ) * 9 + ( cell.x & 7 ) ];|--diffusion|GLSL: the gather's class read a row late, 8 -> 9"
	"source/Shaders.cpp|light += toLinear( clamp( c.rgb / a, 0.0, 1.0 ) ) * a;|light += toLinear( clamp( c.rgb / a, 0.0, 1.0 ) ) / a;|--alpha|GLSL: a cell's light over its coverage, not times, * -> /"
	"source/Shaders.cpp|vec4( colour * cell.a, cell.a )|vec4( colour / cell.a, cell.a )|--alpha|GLSL: the entry not premultiplied by its cell's coverage, * -> /"
	"source/Shaders.cpp|a >= 1.0 ? y : mix( x, y, a );|a >= 1.0 ? x : mix( x, y, a );|--mix|GLSL: Mix 1 the clip, y -> x"
	"source/Shaders.cpp|( ( 2 * cell + 1 ) * OldSize )|( ( 2 * cell + 2 ) * OldSize )|--resize|GLSL: a frozen index resampled from a half cell on, 1 -> 2"
	"source/Shaders.cpp|const float kLike = 0.04;|const float kLike = 4.04;|--adaptive|GLSL: every cell of a block like the picked one, the block's mean again, 0 -> 4"
	"source/Clut.cpp|( 2 * i + 1 ) * m / ( 2 * n )|( 2 * i + 1 ) * m / ( 3 * n )|--swap|C++: the swap partner over 3N, 2 -> 3"
	"source/Clut.cpp|x.L + t * ( y.L - x.L )|x.L + t * ( y.L + x.L )|--swap|C++: Fade's lightness toward the sum, - -> +"
	"source/Clut.cpp|c = s * ( n + 1 ) >= i + 1 ? b : a;|c = s * ( n + 2 ) >= i + 1 ? b : a;|--swap|C++: Ripple over N + 2 steps, 1 -> 2"
	"source/Clut.cpp|out.lo + wrap( k - now, length )|out.lo + wrap( k + now, length )|--cycle|C++: the registers rotated against the offset, - -> +"
	"source/Clut.cpp|return p <= length - 1 ? p : period - p;|return p <= length - 1 ? p : period + p;|--cycle|C++: Ping-Pong's way back a way on, - -> +"
	"source/Clut.cpp|std::floor( phase + kPhaseSnap )|std::floor( phase - kPhaseSnap )|--cycle|C++: a whole step counted a frame late, + -> -"
	"source/Ramdac.cpp|beats = 4.0 * std::max( d, 0.0 );|beats = 5.0 * std::max( d, 0.0 );|--cycle|C++: five beats to the host's bar, 4 -> 5"
	"source/Ramdac.cpp|case CycleSync::Bar: return beats / 4.0;|case CycleSync::Bar: return beats / 3.0;|--cycle|C++: a bar of three beats, 4 -> 3"
	"source/Controls.cpp|return kRateMost * v * v;|return kRateMost * v / v;|--cycle|C++: Cycle Rate always its most, * -> /"
	"source/Adaptive.cpp|-std::max( dt, 0.0 ) / tau )|-std::max( dt, 0.0 ) * tau )|--adaptive|C++: the one-pole's step dt tau, / -> *"
	"source/Dither.cpp|( orthogonal ? 2 : 1 )|( orthogonal ? 3 : 1 )|--diffusion|C++: Knuth's orthogonal weight 3, 2 -> 3"
	"source/Dither.cpp|{ 34, 48, 40, 32, 29, 15, 23, 31 },|{ 34, 48, 40, 32, 29, 15, 23, 30 },|--diffusion|C++: Knuth's class 31 made a second 30, 1 -> 0"
	"source/Palettes.cpp|0.4122214708 * r|0.4922214708 * r|--oklab|C++: the palette's OKLab L cone a fifth too red, 1 -> 9"
	"source/Palettes.cpp|std::pow( ( code + 0.055 ) / 1.055, 2.4 )|std::pow( ( code + 0.055 ) / 1.055, 2.5 )|--nearest|C++: the palette's entries decoded at gamma 2.5, 4 -> 5"
	"source/Palettes.cpp|return v >> ( filled - 8 );|return v >> ( filled - 7 );|--dac|C++: the DAC's levels a bit short, 8 -> 7"
	"source/Palettes.cpp|if( error < bestError )|if( error > bestError )|--dac|C++: the DAC's level the farthest, < -> >"
)

limit() {
	# macOS has no timeout(1): perl's alarm is enough.
	perl -e 'alarm shift; exec @ARGV' "$@"
}

caught=0
ran=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	[[ -z "${1:-}" || "$what" == *"$1"* ]] || continue
	ran=$(( ran + 1 ))
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DRAMDAC_BUILD_TOOLS=ON >/dev/null
	cmake --build "$tree/build" --target rmtest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	status=0
	extra=()
	# The checks that need no GL context take no raster.
	case "$check" in --palettes|--custom|--oklab|--bluenoise|--names) ;; *) extra=( --size 320x180 ) ;; esac
	limit 300 "$tree/build/rmtest" "$check" ${extra[@]+"${extra[@]}"} >"$WORK/log" 2>&1 || status=$?
	if [[ "$status" -eq 0 ]]; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant (exit %d):\n' "$check" "$status"
		# A mutant that timed out printed no FAIL: grep finding none must not end the run.
		{ grep -E '^  FAIL' "$WORK/log" || true; } | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "$ran" "$caught"
[[ "$ran" -gt 0 && "$caught" -eq "$ran" ]]
