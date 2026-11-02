#pragma once

#include "StoatworksAboutLinks.h"

/**
    The host's parameters, and what they mean.

    Every STANDARD parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange`
    could widen it; the conversions live in Controls.cpp. The four counts are
    `FF_TYPE_INTEGER`, which is exempt, so they hold real integers with real
    ranges. Option parameters hold the element index. Custom is text.
*/
namespace ramdac
{
/// Every control, in the order Resolume shows them. The About block is
/// LAST, so the day a user guide adds a button no control moves.
enum ParamId : unsigned int
{
	// -- Palette: what the indices mean -----------------------------------------
	PT_PALETTE = 0,
	PT_COLOURS,
	PT_ADAPT,
	PT_CUSTOM,
	PT_ORDER,
	PT_DAC,

	// -- Dither: how a colour between two entries is drawn ----------------------
	PT_DITHER,
	PT_DITHER_AMOUNT,
	PT_PIXEL_SIZE,

	// -- Swap: a second table behind the same indices ---------------------------
	PT_SWAP_TO,
	PT_SWAP,
	PT_SWAP_STYLE,

	// -- Cycle: a range of the table rotated, Deluxe Paint's CRNG ---------------
	PT_CYCLE,
	PT_CYCLE_RATE,
	PT_CYCLE_SYNC,
	PT_CYCLE_START,
	PT_CYCLE_LENGTH,
	PT_CYCLE_BLEND,

	// -- Output -----------------------------------------------------------------
	PT_FREEZE,
	PT_ALPHA,
	PT_MIX,

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_FIRST,
	PT_COUNT = PT_ABOUT_FIRST + 1 + stoatworks::about::kButtonCount
};

enum class Kind
{
	Standard,///< 0..1, converted in Controls.cpp
	Option,  ///< an element index
	Integer, ///< a real integer between lo and hi
	Boolean, ///< off or on
	Text     ///< typed
};

/// One control as the host is told about it.
struct ControlInfo
{
	unsigned int id;
	const char* name;  ///< at most 16 characters: FFGL's name field is not terminated
	const char* group;
	Kind kind;
	float defaultValue;///< 0..1, the index, the integer, or 0/1
	const char* const* options;
	int optionCount;
	int lo, hi;        ///< an Integer's range
	const char* text;  ///< a Text control's default
};

/// The table, indexed by ParamId, for every id below PT_ABOUT_FIRST.
const ControlInfo& InfoOf( unsigned int id );

//---------------------------------------------------------------------------
// Options. Saved compositions hold the element index, so every list is
// append-only from v0.1.0.
//---------------------------------------------------------------------------
enum class PaletteId
{
	Adaptive = 0,///< chosen from the clip: median cut, then k-means
	Custom,      ///< the Custom text's hex colours
	Teletext,    ///< the RGB cube's eight corners, teletext's code order
	CgaMode4,    ///< 320x200, palette 1 high intensity
	Cga16,       ///< RGBI, as the IBM 5153 showed it
	C64,         ///< Pepto, 2001
	Spectrum,    ///< 8 basic, 7 bright
	Cpc,         ///< Mode 0's steady inks at power-on
	Nes,         ///< 16 of the 2C02's 64
	GameBoy,     ///< the DMG's four greens
	Pico8,
	Workbench13, ///< Amiga OS 1.x's four registers
	Workbench20, ///< Amiga OS 2.x's four registers
	Count
};

/// What "index i" means: the i-th lightest entry, or the machine's own i.
enum class Order
{
	Lightness = 0,
	Native,
	Count
};

/// The DAC's precision: bits a gun.
enum class Dac
{
	Bits8 = 0,///< 24-bit
	Bits6,    ///< 18-bit, VGA's palette DAC
	Bits4,    ///< 12-bit, the Amiga's OCS registers
	Bits3,    ///< 9-bit, the Atari ST's
	Count
};

enum class Dither
{
	None = 0,
	Bayer2,
	Bayer4,
	Bayer8,
	BlueNoise,
	DotDiffusion,
	Count
};

enum class SwapStyle
{
	Cut = 0,///< the second table from Swap 1/2
	Fade,   ///< every index along its own line in OKLab
	Ripple, ///< index by index, in the working order
	Count
};

enum class CycleMode
{
	Off = 0,
	Forward, ///< each colour one register up a step (CRNG's "upwards")
	Reverse, ///< one register down
	PingPong,///< up the range and back: not CRNG's
	Count
};

/// What Cycle Rate is measured in: per second, or per beat or bar of Resolume's.
enum class CycleSync
{
	Free = 0,
	Beat,
	Bar,
	Count
};

enum class CycleBlend
{
	Step = 0,///< whole registers, as DPaint did
	Smooth,  ///< an OKLab blend between steps
	Count
};

enum class AlphaMode
{
	Keep = 0,///< the clip's own coverage
	Opaque,
	Count
};

/// An option's index from the value the host stored (it reads back 0..1
/// whatever the count -- the fleet's trap -- so the table's own count is
/// what bounds it).
int OptionIndex( float value, int count );

/// An Integer control's value, rounded and clamped to its range.
int IntegerOf( unsigned int id, float value );

const char* PaletteName( int index );

//---------------------------------------------------------------------------
// The mappings. Each says its range and its shape.
//---------------------------------------------------------------------------

/// Adapt: the adaptive palette's time constant, 0 to 4 seconds,
/// quadratically, so the quick end has most of the travel. 0 is at once.
double AdaptFromParam( float value );
float ParamFromAdapt( double seconds );

/// Cycle Rate: 0 to 60 steps a second (or a beat, or a bar), quadratically.
/// 60 a second is CRNG's whole range (rate 16384).
double CycleRateFromParam( float value );
float ParamFromCycleRate( double steps );

constexpr int kMaxEntries = 16;

} // namespace ramdac
