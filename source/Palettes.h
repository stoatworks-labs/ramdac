#pragma once

#include "Controls.h"

#include <array>
#include <cstdint>
#include <string>

/**
    The palettes, and the colour science they are measured in, on the CPU in
    double.

    A palette is up to sixteen colours as 8-bit sRGB code values, in the
    machine's own order. Every published table here is the values its source
    gives, transcribed once; ATTRIBUTIONS.md says where each comes from, and
    `rmtest --palettes` holds the transcription to the harness's own copy.

    **OKLab** is Björn Ottosson's (2020), with the matrices as he revised them
    on 2021-01-25, straight from linear sRGB. Distances between colours are
    measured there: it is close to perceptually uniform, so "nearest" means
    what it looks like, and a straight line in it is a fade that does not dip
    through grey.

    **The DAC** is the precision the palette is shown at, bits a gun. A code
    is rounded to the level whose bit-replicated expansion is nearest it: at 4
    bits that is ×17, the Amiga's $RGB → 8 bits exactly as copperlist's
    `chip::Expand12` does it; at 6 bits, VGA's DAC, the top two bits copied
    down; at 3 bits the Atari ST's 512.
*/
namespace ramdac
{
struct Rgb8
{
	uint8_t r = 0, g = 0, b = 0;
	bool operator==( const Rgb8& o ) const
	{
		return r == o.r && g == o.g && b == o.b;
	}
	bool operator!=( const Rgb8& o ) const
	{
		return !( *this == o );
	}
};

struct Lab
{
	double L = 0.0, a = 0.0, b = 0.0;
};

struct Palette
{
	int count = 0;
	std::array< Rgb8, kMaxEntries > entries {};
};

//---------------------------------------------------------------------------
// Colour.
//---------------------------------------------------------------------------
double LinearOfCode( double code );
double CodeOfLinear( double light );

/// Linear sRGB to OKLab, Ottosson's two matrices and a cube root between.
Lab OklabOfLinear( double r, double g, double b );
/// ... and back. Out-of-gamut results are not clamped here.
void LinearOfOklab( const Lab& lab, double rgb[ 3 ] );

Lab OklabOf( const Rgb8& c );

/// A colour in OKLab back to 8-bit code values: linear clamped into the cube,
/// then each gun rounded to the nearest code.
Rgb8 Rgb8OfOklab( const Lab& lab );

double DistanceSquared( const Lab& x, const Lab& y );

//---------------------------------------------------------------------------
// The DAC.
//---------------------------------------------------------------------------
int DacBits( Dac dac );
/// The 8-bit code a `bits`-bit gun shows nearest `code` (0..255): a level n
/// shows its bits replicated down to fill eight.
int DacRound( int code, int bits );
Rgb8 DacRound( const Rgb8& c, int bits );
/// What level n of a `bits`-bit gun expands to, in 8 bits.
int DacExpand( int level, int bits );

//---------------------------------------------------------------------------
// The library.
//---------------------------------------------------------------------------
/// A fixed palette in its machine's order. Adaptive and Custom have no fixed
/// table and return an empty palette.
const Palette& LibraryPalette( PaletteId id );

/// Custom's text: up to sixteen colours, each `#RRGGBB` or `RRGGBB` (24-bit)
/// or `$RGB` / `#RGB` (a 12-bit register, ×17 a gun, as the Amiga and
/// copperlist write them), separated by spaces, commas or semicolons. Whatever
/// is not a colour is skipped and counted in `rejected`. Fewer than two
/// colours is not a palette, and gives the default instead (`usedDefault`).
Palette ParseCustom( const std::string& text, int* rejected = nullptr, bool* usedDefault = nullptr );

/// The Custom control's default text.
const char* DefaultCustomText();

/// A permutation of 0..count-1 that lists the palette darkest first, by OKLab
/// L; equal L keeps the machine's order.
std::array< int, kMaxEntries > LightnessOrder( const Palette& p );

/// The palette as "index i" means it to the swap and the cycle: lightest
/// last (Order Lightness), or as the machine numbered it.
Palette Ordered( const Palette& p, Order order );

/// Every entry through the DAC.
Palette ThroughDac( const Palette& p, int bits );

} // namespace ramdac
