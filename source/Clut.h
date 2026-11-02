#pragma once

#include "Palettes.h"

/**
    The colour table: what each index shows this frame. Sixteen entries,
    rebuilt on the CPU every frame in double, and uploaded as the display
    pass's only notion of colour. Every swap, fade and cycle is done here,
    to the table, never to the picture -- which is what a RAMDAC is.

    **Swap.** Palette A is what the picture was quantised to; B is `Swap To`.
    Index i of an N-entry A meets entry j = ⌊(i + ½)·M/N⌋ of an M-entry B
    (the identity when M = N), both in the working order. Cut shows B from
    Swap ½; Fade moves each index from A_i to B_j along the straight line in
    OKLab, through the DAC; Ripple cuts index i at Swap ≥ (i + 1)/(N + 1),
    darkest first in Lightness order. Swap 0 is A and Swap 1 is B, exactly.

    **Cycle** is Deluxe Paint's CRNG: the range [start, start + length) of
    the table, clipped to the palette, rotated a register a step. Forward
    moves each colour one register up (CRNG's flag bit 1 clear, "cycle
    upwards"); Reverse down; Ping-Pong, which is not CRNG's, up the range and
    back. Step shows whole registers, as DPaint did; Smooth blends each
    register between its colour at this step and the next in OKLab, through
    the DAC. Cycling acts on the swapped table, so a cycling range carries on
    through a swap.
*/
namespace ramdac
{
struct TableInputs
{
	Palette a;                     ///< the working order, through the DAC
	Palette b;                     ///< likewise
	double swap         = 0.0;
	SwapStyle style     = SwapStyle::Cut;
	CycleMode cycle     = CycleMode::Off;
	int start           = 0;
	int length          = kMaxEntries;
	CycleBlend blend    = CycleBlend::Step;
	double phase        = 0.0;     ///< steps, from CycleClock
	int dacBits         = 8;
	bool reverseForTest = false;   ///< --cycle's negative control: Forward and Reverse turned round
};

struct Table
{
	int count = 0;
	std::array< Rgb8, kMaxEntries > entries {};
	long long step = 0;  ///< the whole steps the cycle has made
	double fraction = 0.0;///< ... and how far to the next
	int lo = 0, hi = 0;  ///< the range actually cycled, [lo, hi)
};

/// Where index i of an N-entry palette meets an M-entry one.
int SwapPartner( int i, int n, int m );

/// The register offset a cycle has reached after `step` whole steps over a
/// range of `length`: the colour shown at lo + k is the one at
/// lo + ((k - offset) mod length).
long long CycleOffset( CycleMode mode, long long step, int length );

/// A phase's whole steps. Within kPhaseSnap of the next whole step counts as
/// on it: the phase is a sum of rate × dt in double, which lands an ulp or
/// two either side of an exact step.
constexpr double kPhaseSnap = 1e-6;
long long WholeSteps( double phase );

Table BuildTable( const TableInputs& in );

/// The cycle's phase in steps, kept in double: Resolume's clock overflows a
/// float, so nothing the GPU sees is ever a time.
class CycleClock
{
public:
	void Advance( double rate, double units )
	{
		phase += rate * units;
	}
	double Phase() const
	{
		return phase;
	}
	void Reset()
	{
		phase = 0.0;
	}

private:
	double phase = 0.0;
};

} // namespace ramdac
