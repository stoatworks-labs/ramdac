// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \` for a backtick inside a comment.
export const VERSION = '#version 410 core\n';

// kQuadVertex, source/Shaders.cpp
export const QUAD_VERTEX = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

// kCommon, source/Shaders.cpp
export const COMMON = `
//Shaders.h's Hook, which this text must repeat.
const int kHookRgbDistance  = 1;
const int kHookErrorOklab   = 2;
const int kHookMovingDither = 4;
const int kHookBlockMean    = 8;
const int kMaxEntries       = 16;

uniform int Hooks;

out vec4 fragColor;

//sRGB, both ways. Light adds; code values are what a clip and a palette hold.
vec3 toLinear( vec3 code )
{
	vec3 c = max( code, vec3( 0.0 ) );
	return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), step( vec3( 0.04045 ), c ) );
}
vec3 toCode( vec3 light )
{
	vec3 l = max( light, vec3( 0.0 ) );
	return mix( l * 12.92, 1.055 * pow( l, vec3( 1.0 / 2.4 ) ) - 0.055, step( vec3( 0.0031308 ), l ) );
}
`;

// kPaletteCommon, source/Shaders.cpp
export const PALETTE_COMMON = `
uniform int Count;                  //entries, 1..16
uniform vec3 PalLin[ kMaxEntries ]; //linear light
uniform vec3 PalLab[ kMaxEntries ]; //OKLab, from the CPU in double
uniform vec3 PalCode[ kMaxEntries ];//code values, for the RGB-distance hook
uniform int LightOrder[ kMaxEntries ];//the entries darkest first

//Björn Ottosson's OKLab (2020; the matrices of 2021-01-25), from linear sRGB.
vec3 oklabOf( vec3 lin )
{
	vec3 lms = vec3( dot( vec3( 0.4122214708, 0.5363325363, 0.0514459929 ), lin ),
	                 dot( vec3( 0.2119034982, 0.6806995451, 0.1073969566 ), lin ),
	                 dot( vec3( 0.0883024619, 0.2817188376, 0.6299787005 ), lin ) );
	//The cube root. pow() is undefined below 0 and these are never below 0
	//for a colour in the cube; the floor keeps pow( 0, y ) out of it.
	lms = sign( lms ) * pow( max( abs( lms ), vec3( 1e-30 ) ), vec3( 1.0 / 3.0 ) );
	return vec3( dot( vec3( 0.2104542553, 0.7936177850, -0.0040720468 ), lms ),
	             dot( vec3( 1.9779984951, -2.4285922050, 0.4505937099 ), lms ),
	             dot( vec3( 0.0259040371, 0.7827717662, -0.8086757660 ), lms ) );
}

//The nearest entry; a tie goes to the lower index.
int nearestLab( vec3 lab )
{
	int best    = 0;
	float bestD = 3.4e38;
	for( int i = 0; i < kMaxEntries; ++i )
	{
		if( i >= Count )
			break;
		vec3 d   = lab - PalLab[ i ];
		float dd = dot( d, d );
		if( dd < bestD )
		{
			bestD = dd;
			best  = i;
		}
	}
	return best;
}
int nearestCode( vec3 code )
{
	int best    = 0;
	float bestD = 3.4e38;
	for( int i = 0; i < kMaxEntries; ++i )
	{
		if( i >= Count )
			break;
		vec3 d   = code - PalCode[ i ];
		float dd = dot( d, d );
		if( dd < bestD )
		{
			bestD = dd;
			best  = i;
		}
	}
	return best;
}
int nearestEntry( vec3 lin )
{
	if( ( Hooks & kHookRgbDistance ) != 0 )
		return nearestCode( toCode( lin ) );
	return nearestLab( oklabOf( lin ) );
}
`;

// kGridFragment, source/Shaders.cpp
export const GRID_FRAGMENT = `
uniform sampler2D InputTexture;
uniform ivec2 Size;//the clip's
uniform int Cell;  //Pixel Size

void main()
{
	ivec2 lo = ivec2( gl_FragCoord.xy ) * Cell;
	ivec2 hi = min( lo + ivec2( Cell ), Size );
	vec3 light = vec3( 0.0 );
	float covered = 0.0, n = 0.0;
	for( int y = lo.y; y < hi.y; ++y )
		for( int x = lo.x; x < hi.x; ++x )
		{
			vec4 c  = texelFetch( InputTexture, ivec2( x, y ), 0 );
			float a = clamp( c.a, 0.0, 1.0 );
			n += 1.0;
			if( a <= 0.0 )
				continue;
			//Premultiplied code values: the colour is rgb / a.
			light += toLinear( clamp( c.rgb / a, 0.0, 1.0 ) ) * a;
			covered += a;
		}
	fragColor = vec4( covered > 0.0 ? light / covered : vec3( 0.0 ), n > 0.0 ? covered / n : 0.0 );
}
`;

// kAnalysisFragment, source/Shaders.cpp
export const ANALYSIS_FRAGMENT = `
const float kLike = 0.04;

uniform sampler2D Grid;
uniform ivec2 GridSize;
uniform ivec2 AnalysisSize;

uint lowbias32( uint h )
{
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return h;
}

void main()
{
	ivec2 at = ivec2( gl_FragCoord.xy );
	ivec2 lo = at * GridSize / AnalysisSize;
	ivec2 hi = max( ( at + 1 ) * GridSize / AnalysisSize, lo + 1 );
	if( ( Hooks & kHookBlockMean ) == 0 )
	{
		uvec2 span  = uvec2( hi - lo );
		uint h      = lowbias32( uint( at.x ) | ( uint( at.y ) << 16u ) );
		vec4 picked = texelFetch( Grid, lo + ivec2( uvec2( h % span.x, ( h >> 16u ) % span.y ) ), 0 );
		vec3 like   = oklabOf( picked.rgb );
		vec3 sum    = vec3( 0.0 );
		float held  = 0.0;
		for( int y = lo.y; y < hi.y; ++y )
			for( int x = lo.x; x < hi.x; ++x )
			{
				vec4 g = texelFetch( Grid, ivec2( x, y ), 0 );
				if( g.a > 0.0 && distance( oklabOf( g.rgb ), like ) <= kLike )
				{
					sum += g.rgb * g.a;
					held += g.a;
				}
			}
		fragColor = vec4( held > 0.0 ? sum / held : picked.rgb, picked.a );
		return;
	}
	vec3 light = vec3( 0.0 );
	float covered = 0.0, n = 0.0;
	for( int y = lo.y; y < hi.y; ++y )
		for( int x = lo.x; x < hi.x; ++x )
		{
			vec4 g = texelFetch( Grid, ivec2( x, y ), 0 );
			light += g.rgb * g.a;
			covered += g.a;
			n += 1.0;
		}
	fragColor = vec4( covered > 0.0 ? light / covered : vec3( 0.0 ), covered / n );
}
`;

// kQuantiseFragment, source/Shaders.cpp
export const QUANTISE_FRAGMENT = `
uniform sampler2D Grid;
uniform sampler2D Mask;    //64x64 ranks 0..4095, R32F
uniform int DitherIndex;   //0 None, 1-3 Bayer 2/4/8, 4 Blue Noise
uniform float Amount;
uniform int Frame;         //only for the moving-dither hook

//The cell's rank in the 2^k Bayer matrix: the xor of x and y and y itself,
//their bits interleaved and reversed. k = 1 is [[0, 2], [3, 1]].
int bayerRank( ivec2 p, int k )
{
	int mask = ( 1 << k ) - 1;
	int x = p.x & mask, y = p.y & mask, xy = x ^ y;
	int v = 0;
	for( int bit = 0; bit < 3; ++bit )
	{
		if( bit >= k )
			break;
		v |= ( ( ( xy >> bit ) & 1 ) << ( 2 * k - 1 - 2 * bit ) ) | ( ( ( y >> bit ) & 1 ) << ( 2 * k - 2 - 2 * bit ) );
	}
	return v;
}

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );
	vec4 g     = texelFetch( Grid, cell, 0 );
	vec3 t     = g.rgb;
	int index  = 0;
	if( DitherIndex == 0 )
		index = nearestEntry( t );
	else
	{
		ivec2 p = cell;
		if( ( Hooks & kHookMovingDither ) != 0 )
			p += ivec2( 5 * Frame, 3 * Frame );
		int n = 64, rank = 0;
		if( DitherIndex <= 3 )
		{
			n    = 1 << ( 2 * DitherIndex );
			rank = bayerRank( p, DitherIndex );
		}
		else
			rank = int( texelFetch( Mask, p & 63, 0 ).r ) >> 6;

		int counts[ kMaxEntries ];
		for( int i = 0; i < kMaxEntries; ++i )
			counts[ i ] = 0;
		vec3 error = vec3( 0.0 );
		vec3 tLab  = oklabOf( t );
		for( int i = 0; i < 64; ++i )
		{
			if( i >= n )
				break;
			int e = 0;
			if( ( Hooks & kHookErrorOklab ) != 0 )
			{
				e = nearestLab( tLab + error * Amount );
				error += tLab - PalLab[ e ];
			}
			else
			{
				e = nearestEntry( clamp( t + error * Amount, 0.0, 1.0 ) );
				error += t - PalLin[ e ];
			}
			counts[ e ] += 1;
		}
		int seen = 0;
		index    = LightOrder[ 0 ];
		for( int j = 0; j < kMaxEntries; ++j )
		{
			if( j >= Count )
				break;
			int e = LightOrder[ j ];
			seen += counts[ e ];
			if( rank < seen )
			{
				index = e;
				break;
			}
		}
	}
	fragColor = vec4( float( index ), 0.0, 0.0, g.a );
}
`;

// kDiffuseFragment, source/Shaders.cpp
export const DIFFUSE_FRAGMENT = `
uniform sampler2D Grid;
uniform sampler2D Classes;    //every class decided so far, block by block
uniform ivec2 GridSize;
uniform ivec2 ClassPos;       //where this class sits in the 8x8 tile
uniform ivec2 BlockOrigin;    //this class's block
uniform float Amount;
uniform int DonorCount;
uniform ivec2 DonorOffset[ 8 ];//to the donor, in cells
uniform ivec2 DonorTexel[ 8 ]; //the donor's block, plus its tile's offset from ours
uniform float DonorShare[ 8 ];

void main()
{
	ivec2 tile = ivec2( gl_FragCoord.xy ) - BlockOrigin;
	ivec2 cell = tile * 8 + ClassPos;
	if( cell.x >= GridSize.x || cell.y >= GridSize.y )
	{
		fragColor = vec4( 0.0 );
		return;
	}
	vec3 incoming = vec3( 0.0 );
	for( int i = 0; i < 8; ++i )
	{
		if( i >= DonorCount )
			break;
		ivec2 donor = cell + DonorOffset[ i ];
		if( donor.x < 0 || donor.y < 0 || donor.x >= GridSize.x || donor.y >= GridSize.y )
			continue;
		incoming += DonorShare[ i ] * texelFetch( Classes, DonorTexel[ i ] + tile, 0 ).rgb;
	}
	vec3 attempt = clamp( texelFetch( Grid, cell, 0 ).rgb + incoming * Amount, 0.0, 1.0 );
	int e        = nearestEntry( attempt );
	fragColor    = vec4( attempt - PalLin[ e ], float( e ) );
}
`;

// kSettleFragment, source/Shaders.cpp
export const SETTLE_FRAGMENT = `
uniform sampler2D Fresh;

void main()
{
	fragColor = texelFetch( Fresh, ivec2( gl_FragCoord.xy ), 0 );
}
`;

// kGatherFragment, source/Shaders.cpp
export const GATHER_FRAGMENT = `
uniform sampler2D Classes;
uniform sampler2D Grid;
uniform ivec2 Tiles;     //a block's size
uniform int ClassOf[ 64 ];//the class matrix, row 0 the bottom

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );
	int c      = ClassOf[ ( cell.y & 7 ) * 8 + ( cell.x & 7 ) ];
	vec4 v     = texelFetch( Classes, ivec2( c & 7, c >> 3 ) * Tiles + cell / 8, 0 );
	fragColor  = vec4( v.a, 0.0, 0.0, texelFetch( Grid, cell, 0 ).a );
}
`;

// kDisplayFragment, source/Shaders.cpp
export const DISPLAY_FRAGMENT = `
uniform sampler2D Indices;
uniform sampler2D InputTexture;
uniform ivec2 Size;
uniform ivec2 ViewOrigin;
uniform int Cell;
uniform int TableCount;
uniform vec3 Table[ kMaxEntries ];//code values, k / 255
uniform int AlphaIndex;           //0 Keep, 1 Opaque
uniform float MixAmount;

//mix(), exact at its ends: Mix 0 is the clip and Mix 1 the table, as they
//are. GLSL does not promise mix( x, y, 1.0 ) == y.
vec4 blend( vec4 x, vec4 y, float a )
{
	return a <= 0.0 ? x : a >= 1.0 ? y : mix( x, y, a );
}

void main()
{
	ivec2 at    = clamp( ivec2( gl_FragCoord.xy ) - ViewOrigin, ivec2( 0 ), Size - 1 );
	vec4 src    = texelFetch( InputTexture, at, 0 );
	vec4 cell   = texelFetch( Indices, at / Cell, 0 );
	int index   = clamp( int( cell.r + 0.5 ), 0, TableCount - 1 );
	vec3 colour = Table[ index ];
	vec4 shown  = AlphaIndex == 0 ? vec4( colour * cell.a, cell.a ) : vec4( colour, 1.0 );
	fragColor   = blend( src, shown, MixAmount );
}
`;

// kResampleFragment, source/Shaders.cpp
export const RESAMPLE_FRAGMENT = `
uniform sampler2D Frozen;
uniform ivec2 OldSize;
uniform ivec2 NewSize;

void main()
{
	//The old cell the new cell's centre falls in, in integers: at 2:1 every
	//centre is on an old edge, and a float division (2.5 ulp in GLSL) could
	//floor either way.
	ivec2 cell = ivec2( gl_FragCoord.xy );
	ivec2 from = ( ( 2 * cell + 1 ) * OldSize ) / ( 2 * NewSize );
	fragColor  = texelFetch( Frozen, clamp( from, ivec2( 0 ), OldSize - 1 ), 0 );
}
`;
