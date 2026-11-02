#include "Dither.h"

namespace ramdac::dither
{
const int kKnuthClass[ 8 ][ 8 ] = {
	{ 34, 48, 40, 32, 29, 15, 23, 31 },
	{ 42, 58, 56, 53, 21, 5, 7, 10 },
	{ 50, 62, 61, 45, 13, 1, 2, 18 },
	{ 38, 46, 54, 37, 25, 17, 9, 26 },
	{ 28, 14, 22, 30, 35, 49, 41, 33 },
	{ 20, 4, 6, 11, 43, 59, 57, 52 },
	{ 12, 0, 3, 19, 51, 63, 60, 44 },
	{ 24, 16, 8, 27, 39, 47, 55, 36 },
};

namespace
{
int classAt( int x, int y )
{
	return kKnuthClass[ ( ( y % 8 ) + 8 ) % 8 ][ ( ( x % 8 ) + 8 ) % 8 ];
}

int weightOf( int dx, int dy, bool swapped )
{
	const bool orthogonal = dx == 0 || dy == 0;
	return swapped ? ( orthogonal ? 1 : 2 ) : ( orthogonal ? 2 : 1 );
}

/// A cell's weights over its neighbours of higher class: what its error is
/// shared between.
int outgoingWeight( int x, int y, bool swapped )
{
	int total = 0;
	for( int dy = -1; dy <= 1; ++dy )
		for( int dx = -1; dx <= 1; ++dx )
			if( ( dx || dy ) && classAt( x + dx, y + dy ) > classAt( x, y ) )
				total += weightOf( dx, dy, swapped );
	return total;
}

int floorDiv8( int v )
{
	return v >= 0 ? v / 8 : -( ( -v + 7 ) / 8 );
}
} // namespace

std::array< ClassPlan, kClasses > BuildPlan( bool swappedWeights )
{
	std::array< ClassPlan, kClasses > plan {};
	for( int y = 0; y < 8; ++y )
		for( int x = 0; x < 8; ++x )
		{
			ClassPlan& p = plan[ static_cast< size_t >( kKnuthClass[ y ][ x ] ) ];
			p.x          = x;
			p.y          = y;
			for( int dy = -1; dy <= 1; ++dy )
				for( int dx = -1; dx <= 1; ++dx )
				{
					if( !dx && !dy )
						continue;
					const int nx = x + dx, ny = y + dy;
					const int donor = classAt( nx, ny );
					if( donor >= kKnuthClass[ y ][ x ] )
						continue;
					Donor& d     = p.donors[ static_cast< size_t >( p.donorCount++ ) ];
					d.dx         = dx;
					d.dy         = dy;
					d.donorClass = donor;
					d.tileDx     = floorDiv8( nx );
					d.tileDy     = floorDiv8( ny );
					//The donor is lower, so this cell is one of its higher
					//neighbours and its total is never 0.
					d.share = static_cast< double >( weightOf( dx, dy, swappedWeights ) )
					          / static_cast< double >( outgoingWeight( nx, ny, swappedWeights ) );
				}
		}
	return plan;
}

bool IsBaron( int cls )
{
	for( int y = 0; y < 8; ++y )
		for( int x = 0; x < 8; ++x )
			if( kKnuthClass[ y ][ x ] == cls )
				return outgoingWeight( x, y, false ) == 0;
	return false;
}

int BayerRank( int x, int y, int k )
{
	const int mask = ( 1 << k ) - 1;
	x &= mask;
	y &= mask;
	const int xy = x ^ y;
	int v        = 0;
	for( int bit = 0; bit < k; ++bit )
		v |= ( ( ( xy >> bit ) & 1 ) << ( 2 * k - 1 - 2 * bit ) ) | ( ( ( y >> bit ) & 1 ) << ( 2 * k - 2 - 2 * bit ) );
	return v;
}

} // namespace ramdac::dither
