#pragma once

#include <string>

/**
    The passes. The frame is an index buffer and a colour table: the passes
    before the last make the indices, the last looks them up.

    1. **grid** -- the clip into a grid of Pixel Size × Pixel Size cells: each
       cell's mean in linear light, weighted by coverage (the clip is taken as
       premultiplied code values, the fleet's assumption), and its mean
       coverage. RGBA16F.
    2. **analysis** -- only for Adaptive: the grid averaged again into at most
       96×54 cells, RGBA32F, read back for the CPU's median cut and k-means.
    3. **quantise** -- each cell to an index, RGBA16F, r the index and a the
       coverage. Nearest in OKLab (None); Knoll's pattern dither for any
       palette, the candidate picked by the cell's rank in a Bayer matrix or
       the blue-noise mask.
    3'. **diffuse**, **settle** and **gather** -- Dot Diffusion instead of 3.
       The grid is split into its 64 classes, laid out class by class in one
       texture (class c's cells as a block of tiles); a diffuse draw decides
       one class into a second texture, reading every lower class's errors from
       the first, and a settle draw copies that block back. 128 draws, then
       gather puts every cell's index where the grid has it.
    4. **display** -- to the host: every output pixel the table's entry for
       its cell's index, with the cell's coverage (Alpha Keep) or opaque, then
       Mix against the clip, exact at both ends.
    5. **resample** -- only when the grid's size changes under Freeze: the
       frozen indices, nearest, into the new grid.

    Assemble() is the one place the pieces' order is written; tools/glslc.sh
    mirrors it, so the text it compiles is the text the plugin runs.

    No sin, cos, tan, atan or fract( sin ) anywhere; the only pow is the cube
    root and sRGB's. Nothing in any pass reads a time.
*/
namespace ramdac::shaders
{

extern const char* const kVersion;
extern const char* const kQuadVertex;
extern const char* const kCommon;
extern const char* const kPaletteCommon;
extern const char* const kGridFragment;
extern const char* const kAnalysisFragment;
extern const char* const kQuantiseFragment;
extern const char* const kDiffuseFragment;
extern const char* const kSettleFragment;
extern const char* const kGatherFragment;
extern const char* const kDisplayFragment;
extern const char* const kResampleFragment;

enum class Pass
{
	Grid,
	Analysis,
	Quantise,
	Diffuse,
	Settle,
	Gather,
	Display,
	Resample
};
std::string Assemble( Pass pass );

/// The adaptive palette's analysis grid, at most.
constexpr int kAnalysisWidth  = 96;
constexpr int kAnalysisHeight = 54;

/// Negative-control hooks: bits of the `Hooks` uniform. A shipped instance
/// sets 0, and every branch they guard is then the shipped model. The GLSL
/// repeats these values in kCommon.
enum Hook : int
{
	kHookRgbDistance  = 1 << 0,///< nearest by distance in code values, not OKLab
	kHookErrorOklab   = 1 << 1,///< Knoll's error kept in OKLab, not light
	kHookMovingDither = 1 << 2,///< the dither's threshold moved by the frame count
	kHookBlockMean    = 1 << 3 ///< the adaptive palette's samples the mean of each block, not one of its cells
};

} // namespace ramdac::shaders
