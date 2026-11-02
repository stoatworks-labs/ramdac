#include "Clut.h"

#include <algorithm>
#include <cmath>

namespace ramdac
{
namespace
{
Rgb8 fade( const Rgb8& from, const Rgb8& to, double t, int dacBits )
{
	if( t <= 0.0 )
		return from;
	if( t >= 1.0 )
		return to;
	const Lab x = OklabOf( from ), y = OklabOf( to );
	const Lab m = { x.L + t * ( y.L - x.L ), x.a + t * ( y.a - x.a ), x.b + t * ( y.b - x.b ) };
	return DacRound( Rgb8OfOklab( m ), dacBits );
}

long long wrap( long long v, long long n )
{
	const long long r = v % n;
	return r < 0 ? r + n : r;
}
} // namespace

int SwapPartner( int i, int n, int m )
{
	if( n <= 0 || m <= 0 )
		return 0;
	//⌊(i + ½)·M/N⌋ in integers: ⌊(2i + 1)·M / 2N⌋.
	return std::clamp( ( 2 * i + 1 ) * m / ( 2 * n ), 0, m - 1 );
}

long long CycleOffset( CycleMode mode, long long step, int length )
{
	if( length < 2 )
		return 0;
	switch( mode )
	{
	case CycleMode::Forward: return wrap( step, length );
	case CycleMode::Reverse: return wrap( -step, length );
	case CycleMode::PingPong:
	{
		const long long period = 2LL * ( length - 1 );
		const long long p      = wrap( step, period );
		return p <= length - 1 ? p : period - p;
	}
	default: return 0;
	}
}

long long WholeSteps( double phase )
{
	return static_cast< long long >( std::floor( phase + kPhaseSnap ) );
}

Table BuildTable( const TableInputs& in )
{
	Table out;
	const int n = std::clamp( in.a.count, 0, kMaxEntries );
	out.count   = n;
	if( n == 0 )
		return out;

	//Swap, index by index.
	std::array< Rgb8, kMaxEntries > base {};
	const double s = std::clamp( in.swap, 0.0, 1.0 );
	for( int i = 0; i < n; ++i )
	{
		const Rgb8& a = in.a.entries[ static_cast< size_t >( i ) ];
		if( in.b.count <= 0 || s <= 0.0 )
		{
			base[ static_cast< size_t >( i ) ] = a;
			continue;
		}
		const Rgb8& b = in.b.entries[ static_cast< size_t >( SwapPartner( i, n, in.b.count ) ) ];
		Rgb8 c        = a;
		switch( in.style )
		{
		case SwapStyle::Cut: c = s >= 0.5 ? b : a; break;
		case SwapStyle::Fade: c = fade( a, b, s, in.dacBits ); break;
		case SwapStyle::Ripple: c = s * ( n + 1 ) >= i + 1 ? b : a; break;
		default: break;
		}
		base[ static_cast< size_t >( i ) ] = c;
	}

	//Cycle, register by register.
	out.entries = base;
	out.lo      = std::clamp( in.start, 0, n );
	out.hi      = std::clamp( in.start + in.length, out.lo, n );
	const int length = out.hi - out.lo;
	if( in.cycle == CycleMode::Off || length < 2 )
		return out;

	CycleMode mode = in.cycle;
	if( in.reverseForTest && mode != CycleMode::PingPong )
		mode = mode == CycleMode::Forward ? CycleMode::Reverse : CycleMode::Forward;
	out.step     = WholeSteps( in.phase );
	out.fraction = std::clamp( in.phase - static_cast< double >( out.step ), 0.0, 1.0 );
	const long long now  = CycleOffset( mode, out.step, length );
	const long long next = CycleOffset( mode, out.step + 1, length );
	const bool smooth    = in.blend == CycleBlend::Smooth && out.fraction > 0.0;
	for( int k = 0; k < length; ++k )
	{
		const Rgb8& here = base[ static_cast< size_t >( out.lo + wrap( k - now, length ) ) ];
		Rgb8 shown       = here;
		if( smooth )
			shown = fade( here, base[ static_cast< size_t >( out.lo + wrap( k - next, length ) ) ], out.fraction, in.dacBits );
		out.entries[ static_cast< size_t >( out.lo + k ) ] = shown;
	}
	return out;
}

} // namespace ramdac
