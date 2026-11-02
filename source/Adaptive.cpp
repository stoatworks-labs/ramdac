#include "Adaptive.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace ramdac
{
namespace
{
double axisOf( const Lab& c, int axis )
{
	return axis == 0 ? c.L : axis == 1 ? c.a : c.b;
}

struct Box
{
	std::vector< int > members;
	double sse = 0.0;
	int axis   = 0;
	bool splittable = false;
};

void measure( const std::vector< Sample >& samples, Box& box )
{
	double w = 0.0, m[ 3 ] = { 0.0, 0.0, 0.0 };
	double lo[ 3 ] = { 1e30, 1e30, 1e30 }, hi[ 3 ] = { -1e30, -1e30, -1e30 };
	for( int i : box.members )
	{
		const Sample& s = samples[ static_cast< size_t >( i ) ];
		w += s.weight;
		for( int a = 0; a < 3; ++a )
		{
			const double v = axisOf( s.lab, a );
			m[ a ] += s.weight * v;
			lo[ a ] = std::min( lo[ a ], v );
			hi[ a ] = std::max( hi[ a ], v );
		}
	}
	box.sse        = 0.0;
	box.splittable = false;
	if( w <= 0.0 )
		return;
	for( double& v : m )
		v /= w;
	for( int i : box.members )
	{
		const Sample& s = samples[ static_cast< size_t >( i ) ];
		for( int a = 0; a < 3; ++a )
		{
			const double d = axisOf( s.lab, a ) - m[ a ];
			box.sse += s.weight * d * d;
		}
	}
	box.axis = 0;
	for( int a = 1; a < 3; ++a )
		if( hi[ a ] - lo[ a ] > hi[ box.axis ] - lo[ box.axis ] )
			box.axis = a;
	box.splittable = box.members.size() >= 2 && hi[ box.axis ] > lo[ box.axis ];
}

Lab meanOf( const std::vector< Sample >& samples, const std::vector< int >& members )
{
	double w = 0.0;
	Lab m;
	for( int i : members )
	{
		const Sample& s = samples[ static_cast< size_t >( i ) ];
		w += s.weight;
		m.L += s.weight * s.lab.L;
		m.a += s.weight * s.lab.a;
		m.b += s.weight * s.lab.b;
	}
	if( w > 0.0 )
	{
		m.L /= w;
		m.a /= w;
		m.b /= w;
	}
	return m;
}

int nearestOf( const Lab& c, const std::vector< Lab >& centroids, double* distance = nullptr )
{
	int best         = 0;
	double bestValue = std::numeric_limits< double >::infinity();
	for( size_t j = 0; j < centroids.size(); ++j )
	{
		const double d = DistanceSquared( c, centroids[ j ] );
		if( d < bestValue )
		{
			bestValue = d;
			best      = static_cast< int >( j );
		}
	}
	if( distance )
		*distance = bestValue;
	return best;
}

void sortDarkestFirst( std::vector< Lab >& c )
{
	std::stable_sort( c.begin(), c.end(), []( const Lab& x, const Lab& y ) { return x.L < y.L; } );
}

/// `fresh` renumbered so each of `old`'s entries keeps the nearest fresh one
/// not yet taken, the closest pairs first.
std::vector< Lab > matchTo( const std::vector< Lab >& old, const std::vector< Lab >& fresh )
{
	const size_t n = fresh.size();
	std::vector< Lab > out( n );
	std::vector< bool > oldTaken( n, false ), freshTaken( n, false );
	for( size_t round = 0; round < n; ++round )
	{
		double best = std::numeric_limits< double >::infinity();
		size_t bo = 0, bf = 0;
		for( size_t o = 0; o < n && o < old.size(); ++o )
			if( !oldTaken[ o ] )
				for( size_t f = 0; f < n; ++f )
					if( !freshTaken[ f ] )
					{
						const double d = DistanceSquared( old[ o ], fresh[ f ] );
						if( d < best )
						{
							best = d;
							bo   = o;
							bf   = f;
						}
					}
		if( best == std::numeric_limits< double >::infinity() )
			break;
		oldTaken[ bo ]   = true;
		freshTaken[ bf ] = true;
		out[ bo ]        = fresh[ bf ];
	}
	return out;
}
} // namespace

double KMeansCost( const std::vector< Sample >& samples, const std::vector< Lab >& centroids )
{
	if( centroids.empty() )
		return 0.0;
	double total = 0.0;
	for( const Sample& s : samples )
	{
		double d = 0.0;
		nearestOf( s.lab, centroids, &d );
		total += s.weight * d;
	}
	return total;
}

std::vector< Lab > MedianCut( const std::vector< Sample >& samples, int k )
{
	Box all;
	for( size_t i = 0; i < samples.size(); ++i )
		if( samples[ i ].weight > 0.0 )
			all.members.push_back( static_cast< int >( i ) );
	if( all.members.empty() || k < 1 )
		return {};
	measure( samples, all );
	std::vector< Box > boxes = { all };
	while( static_cast< int >( boxes.size() ) < k )
	{
		int pick = -1;
		for( size_t b = 0; b < boxes.size(); ++b )
			if( boxes[ b ].splittable && ( pick < 0 || boxes[ b ].sse > boxes[ static_cast< size_t >( pick ) ].sse ) )
				pick = static_cast< int >( b );
		if( pick < 0 )
			break;
		Box& box       = boxes[ static_cast< size_t >( pick ) ];
		const int axis = box.axis;
		std::stable_sort( box.members.begin(), box.members.end(), [ & ]( int x, int y ) {
			return axisOf( samples[ static_cast< size_t >( x ) ].lab, axis ) < axisOf( samples[ static_cast< size_t >( y ) ].lab, axis );
		} );
		double total = 0.0;
		for( int i : box.members )
			total += samples[ static_cast< size_t >( i ) ].weight;
		//The weighted median: the first sample at which half the weight is
		//behind. Never a split that leaves a side empty, and never one between
		//two equal values, which would not be a split along the axis at all.
		size_t cut = 1;
		double run = 0.0;
		for( size_t m = 0; m + 1 < box.members.size(); ++m )
		{
			run += samples[ static_cast< size_t >( box.members[ m ] ) ].weight;
			cut = m + 1;
			if( run >= 0.5 * total )
				break;
		}
		auto valueAt = [ & ]( size_t m ) { return axisOf( samples[ static_cast< size_t >( box.members[ m ] ) ].lab, axis ); };
		while( cut < box.members.size() && valueAt( cut ) == valueAt( cut - 1 ) )
			++cut;
		if( cut >= box.members.size() )
		{
			cut = box.members.size() - 1;
			while( cut > 0 && valueAt( cut ) == valueAt( cut - 1 ) )
				--cut;
		}
		Box upper;
		upper.members.assign( box.members.begin() + static_cast< long >( cut ), box.members.end() );
		box.members.resize( cut );
		measure( samples, box );
		measure( samples, upper );
		boxes.push_back( upper );
	}
	std::vector< Lab > out;
	for( const Box& box : boxes )
		out.push_back( meanOf( samples, box.members ) );
	sortDarkestFirst( out );
	return out;
}

int RefineKMeans( const std::vector< Sample >& samples, std::vector< Lab >& centroids, int maxRounds )
{
	const size_t k = centroids.size();
	if( k == 0 || samples.empty() )
		return 0;
	std::vector< int > owner( samples.size(), -1 );
	int round = 0;
	for( ; round < maxRounds; ++round )
	{
		bool changed = false;
		for( size_t i = 0; i < samples.size(); ++i )
		{
			const int j = samples[ i ].weight > 0.0 ? nearestOf( samples[ i ].lab, centroids ) : -1;
			changed |= j != owner[ i ];
			owner[ i ] = j;
		}
		if( !changed && round > 0 )
			break;
		std::vector< double > w( k, 0.0 );
		std::vector< Lab > sum( k );
		for( size_t i = 0; i < samples.size(); ++i )
		{
			if( owner[ i ] < 0 )
				continue;
			const size_t j = static_cast< size_t >( owner[ i ] );
			w[ j ] += samples[ i ].weight;
			sum[ j ].L += samples[ i ].weight * samples[ i ].lab.L;
			sum[ j ].a += samples[ i ].weight * samples[ i ].lab.a;
			sum[ j ].b += samples[ i ].weight * samples[ i ].lab.b;
		}
		std::vector< bool > used( samples.size(), false );
		for( size_t j = 0; j < k; ++j )
		{
			if( w[ j ] > 0.0 )
			{
				centroids[ j ] = { sum[ j ].L / w[ j ], sum[ j ].a / w[ j ], sum[ j ].b / w[ j ] };
				continue;
			}
			//Empty: re-seed at the sample farthest from its own centroid, which
			//is where the clustering is worst.
			double farDistance = -1.0;
			size_t farthest = 0;
			for( size_t i = 0; i < samples.size(); ++i )
			{
				if( owner[ i ] < 0 || used[ i ] )
					continue;
				const double d = DistanceSquared( samples[ i ].lab, centroids[ static_cast< size_t >( owner[ i ] ) ] );
				if( d > farDistance )
				{
					farDistance = d;
					farthest = i;
				}
			}
			if( farDistance >= 0.0 )
			{
				used[ farthest ] = true;
				centroids[ j ]   = samples[ farthest ].lab;
			}
		}
	}
	return round;
}

void Adaptive::Reset()
{
	const bool keepCold = cold;
	*this               = Adaptive {};
	cold                = keepCold;
}

void Adaptive::Update( const float* rgba, int count, int colours, double tau, double dt, int dacBits )
{
	samples.clear();
	for( int i = 0; i < count; ++i )
	{
		const float* c = rgba + static_cast< size_t >( i ) * 4;
		const double w = std::isfinite( c[ 3 ] ) ? std::clamp( static_cast< double >( c[ 3 ] ), 0.0, 1.0 ) : 0.0;
		if( w <= 0.0 )
			continue;
		const double r = std::clamp( static_cast< double >( c[ 0 ] ), 0.0, 1.0 );
		const double g = std::clamp( static_cast< double >( c[ 1 ] ), 0.0, 1.0 );
		const double b = std::clamp( static_cast< double >( c[ 2 ] ), 0.0, 1.0 );
		samples.push_back( { OklabOfLinear( r, g, b ), w } );
	}
	colours = std::clamp( colours, 2, kMaxEntries );

	std::vector< Lab > median = MedianCut( samples, colours );
	if( median.empty() )
		median.assign( 1, Lab {} );//nothing covered: black
	//Fewer distinct colours than entries: the darkest repeated, which the
	//quantiser never picks over the first (a tie goes to the lower index).
	while( static_cast< int >( median.size() ) < colours )
		median.insert( median.begin(), median.front() );
	medianCost = KMeansCost( samples, median );
	reseeded   = false;

	const bool fresh = cold || k != colours || target.size() != static_cast< size_t >( colours );
	if( fresh )
	{
		target = median;
		rounds = cold ? 0 : RefineKMeans( samples, target, kMaxRounds );
		shown  = target;
		k      = colours;
		reseeded = true;
	}
	else
	{
		std::vector< Lab > warm = target;
		rounds                  = RefineKMeans( samples, warm, kMaxRounds );
		if( KMeansCost( samples, warm ) > medianCost )
		{
			std::vector< Lab > refined = median;
			rounds += RefineKMeans( samples, refined, kMaxRounds );
			warm     = matchTo( target, refined );
			reseeded = true;
		}
		target = warm;

		//What is shown follows. Adapt 0 is at once.
		const double alpha = tau > 0.0 ? 1.0 - std::exp( -std::max( dt, 0.0 ) / tau ) : 1.0;
		for( size_t j = 0; j < target.size(); ++j )
		{
			Lab& s       = shown[ j ];
			const Lab& t = target[ j ];
			s.L += alpha * ( t.L - s.L );
			s.a += alpha * ( t.a - s.a );
			s.b += alpha * ( t.b - s.b );
			if( DistanceSquared( s, t ) <= kSnap * kSnap || DacRound( Rgb8OfOklab( s ), dacBits ) == DacRound( Rgb8OfOklab( t ), dacBits ) )
				s = t;
		}
	}
	cost = KMeansCost( samples, target );

	shown8.count = static_cast< int >( shown.size() );
	for( size_t j = 0; j < shown.size(); ++j )
		shown8.entries[ j ] = DacRound( Rgb8OfOklab( shown[ j ] ), dacBits );
}

} // namespace ramdac
